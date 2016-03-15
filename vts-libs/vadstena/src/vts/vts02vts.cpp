#include <cstdlib>
#include <string>
#include <iostream>
#include <algorithm>
#include <iterator>

#include <boost/algorithm/string/split.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include <opencv2/highgui/highgui.hpp>

#include "dbglog/dbglog.hpp"
#include "utility/streams.hpp"

#include "utility/buildsys.hpp"
#include "utility/gccversion.hpp"
#include "utility/progress.hpp"
#include "utility/streams.hpp"
#include "utility/openmp.hpp"

#include "math/transform.hpp"

#include "imgproc/scanconversion.hpp"
#include "imgproc/jpeg.hpp"

#include "geo/csconvertor.hpp"
#include "geo/coordinates.hpp"

#include "vadstena-libs/utility.hpp"

// old stuff
#include "geometry/binmesh.hpp"
#include "vts-libs/vts0.hpp"
#include "vts-libs/vts0/tileset-advanced.hpp"
#include "vts-libs/vts0/io.hpp"

// new stuff
#include "vts-libs/vts.hpp"
#include "vts-libs/vts/encoder.hpp"
#include "vts-libs/vts/opencv/navtile.hpp"
#include "vts-libs/vts/io.hpp"
#include "vts-libs/vts/csconvertor.hpp"

#include "./heightmap.hpp"

namespace po = boost::program_options;
namespace vs = vadstena::storage;
namespace vr = vadstena::registry;
namespace vts0 = vadstena::vts0;
namespace vts = vadstena::vts;
namespace va = vadstena;
namespace ba = boost::algorithm;
namespace fs = boost::filesystem;
namespace ublas = boost::numeric::ublas;

namespace {

struct Config {
    boost::optional<std::uint16_t> textureLayer;
    vs::CreditIds credits;
    unsigned int ntLodPixelSize;
    double dtmExtractionRadius;

    Config() : ntLodPixelSize(1.0), dtmExtractionRadius(40.0) {}
};

class Vts02Vts : public va::UtilityBase
{
public:
    Vts02Vts()
        : UtilityBase("vts02vts", BUILD_TARGET_VERSION)
        , createMode_(vts::CreateMode::failIfExists)
    {
    }

private:
    virtual void configuration(po::options_description &cmdline
                               , po::options_description &config
                               , po::positional_options_description &pd)
        UTILITY_OVERRIDE;

    virtual void configure(const po::variables_map &vars)
        UTILITY_OVERRIDE;

    virtual bool help(std::ostream &out, const std::string &what) const
        UTILITY_OVERRIDE;

    virtual int run() UTILITY_OVERRIDE;

    fs::path input_;
    fs::path output_;

    vts::CreateMode createMode_;

    vts::TileSetProperties properties_;

    Config config_;
};

void Vts02Vts::configuration(po::options_description &cmdline
                             , po::options_description &config
                             , po::positional_options_description &pd)
{
    cmdline.add_options()
        ("input", po::value(&input_)->required()
         , "Path to input (vts0) tile set.")
        ("output", po::value(&output_)->required()
         , "Path to output (vts) tile set.")
        ("overwrite", "Existing tile set gets overwritten if set.")

        ("textureLayer", po::value<std::string>()
         , "String/numeric id of bound layer to be used as external texture "
         "in generated meshes.")

        ("credits", po::value<std::string>()
         , "Comma-separated list of string/numeric credit id.")

        ("navtileLodPixelSize"
         , po::value(&config_.ntLodPixelSize)
         ->default_value(config_.ntLodPixelSize)->required()
         , "Navigation data are generated at first LOD (starting from root) "
         "where rounded value of pixel size (in navigation grid) is less or "
         "equal to this value.")

        ("dtmExtraction.radius"
         , po::value(&config_.dtmExtractionRadius)
         ->default_value(config_.dtmExtractionRadius)->required()
         , "Radius (in meters) of DTM extraction element (in meters).")
        ;

    registryConfiguration(cmdline);

    pd.add("input", 1);
    pd.add("output", 1);

    (void) config;
}

void Vts02Vts::configure(const po::variables_map &vars)
{
    registryConfigure(vars);

    createMode_ = (vars.count("overwrite")
                   ? vts::CreateMode::overwrite
                   : vts::CreateMode::failIfExists);

    if (vars.count("textureLayer")) {
        auto value(vars["textureLayer"].as<std::string>());

        vr::BoundLayer layer;
        try {
            layer = vr::Registry::boundLayer(boost::lexical_cast<int>(value));
        } catch (boost::bad_lexical_cast) {
            layer = vr::Registry::boundLayer(value);
        }

        if (layer.type != vr::BoundLayer::Type::raster) {
            throw po::validation_error
                (po::validation_error::invalid_option_value, "textureLayer");
        }
        config_.textureLayer = layer.numericId;
    }

    if (vars.count("credits")) {
        std::vector<std::string> parts;
        for (const auto &value
                 : ba::split(parts, vars["credits"].as<std::string>()
                             , ba::is_any_of(",")))
        {
            vr::Credit credit;
            try {
                credit = vr::Registry::credit(boost::lexical_cast<int>(value));
            } catch (boost::bad_lexical_cast) {
                credit = vr::Registry::credit(value);
            }

            config_.credits.insert(credit.numericId);
        }
    }
}

bool Vts02Vts::help(std::ostream &out, const std::string &what) const
{
    if (what.empty()) {
        out << R"RAW(vts02vts
usage
    vts02vts INPUT OUTPUT [OPTIONS]

)RAW";
    }
    return false;
}

typedef vts::opencv::NavTile NavTile;

// determine first navtile LOD: LOD where navtile pixel size reaches one
// meter; source LOD is first navtile LOD + 1; both are clamped inside
// available LOD range

vs::Lod clampLod(const vs::LodRange &lodRange, vs::Lod lod)
{
    if (lod < lodRange.min) { return lodRange.min; }
    if (lod > lodRange.max) { return lodRange.max; }
    return lod;
}

vr::TileRange tileRange(const vts0::RasterMask &layer)
{
    vr::TileRange tileRange(math::InvalidExtents{});
    layer.forEachQuad([&](long x, long y, long xsize, long ysize, bool)
    {
        update(tileRange, vr::TileRange::point_type(x, y));
        update(tileRange, vr::TileRange::point_type
               (x + xsize - 1, y + ysize - 1));
    }, vts0::RasterMask::Filter::white);

    return tileRange;
}

std::tuple<vs::Lod, double>
determineNtLod(const vts0::TileIndex &ti, const vts0::Properties &prop
               , vr::ReferenceFrame referenceFrame, double pixelSize)
{
    geo::SrsFactors sf
        (vr::Registry::srs(referenceFrame.model.physicalSrs).srsDef);

    const auto lodRange(ti.lodRange());

    double lodPixelSize(0.0);
    for (const auto lod : lodRange) {

        const auto *layer(ti.mask(lod));
        if (!layer) { continue; }

        const auto tr(tileRange(*layer));
        if (!valid(tr)) { continue; }

        // dataset extents at given LOD
        math::Extents2 extents(math::InvalidExtents{});
        update(extents
               , vts0::extents
               (prop, vts0::TileId(lod, tr.ll(0), tr.ll(1))).ll);
        update(extents
               , vts0::extents
               (prop, vts0::TileId(lod, tr.ur(0), tr.ur(1))).ur);

        // dataset center at given LOD
        const auto cent(center(extents));

        lodPixelSize
            = ((vts0::tileSize(prop, lod).height
                * sf(cent).meridionalScale)
               / (NavTile::size().height - 1));

        if (std::round(lodPixelSize) <= pixelSize) {
            return std::tuple<vs::Lod, double>(lod, lodPixelSize);
        }
    }

    // no such LOD available, take bottom
    return std::tuple<vs::Lod, double>(lodRange.max, lodPixelSize);
}

struct EncoderBase {
    EncoderBase(const Config &config, const vts0::TileSet::pointer &input
                , const vr::ReferenceFrame &referenceFrame)
        : ntLodRange_(input->lodRange())
    {
        std::tie(ntLodRange_.max, ntSourceLodPixelSize_)
            = determineNtLod
            (input->advancedApi().tileIndex(), input->getProperties()
             , referenceFrame, config.ntLodPixelSize);

        if ((ntLodRange_.max + 1) <= input->lodRange().max) {
            ntSourceLod_ = ntLodRange_.max + 1;
            ntSourceLodPixelSize_ /= 2.0;
        } else {
            ntSourceLod_ = ntLodRange_.max;
        }

        LOG(info2)
            << "Navtile data are generated in LOD range: "
            << ntLodRange_ << ".";
        LOG(info2)
            << "Navtile data extracted from LOD: " << ntSourceLod_
            << " with pixel size " << ntSourceLodPixelSize_;
    }

    vs::LodRange ntLodRange_;
    vs::Lod ntSourceLod_;
    double ntSourceLodPixelSize_;
};

class Encoder
    : public vts::Encoder
    , private EncoderBase
{
public:
    Encoder(const boost::filesystem::path &path
            , const vts::TileSetProperties &properties, vts::CreateMode mode
            , const vts0::TileSet::pointer &input
            , const Config &config)
        : vts::Encoder(path, properties, mode)
        , EncoderBase(config, input, referenceFrame())
        , config_(config), input_(input), aa_(input_->advancedApi())
        , ti_(aa_.tileIndex()), cti_(ti_)
        , hma_(ntSourceLod_)
    {
        cti_.makeFull().makeComplete();

        // set constraints: from zero to max LOD
        setConstraints(Constraints()
                       .setLodRange
                       (vs::LodRange(0, input->lodRange().max)));
        setEstimatedTileCount(ti_.count());
    }

private:
    virtual TileResult
    generate(const vts::TileId &tileId, const vts::NodeInfo &nodeInfo
             , const TileResult&)
        UTILITY_OVERRIDE;

    virtual void finish(vts::TileSet&) UTILITY_OVERRIDE;

    void generateHeightMap(const vts::TileId &tileId, const vts0::Mesh &m
                           , const math::Extents2 &divisionExtents);

    const Config config_;

    vts0::TileSet::pointer input_;
    vts0::TileSet::AdvancedApi aa_;
    const vts0::TileIndex &ti_;
    vts0::TileIndex cti_;
    HeightMap::Accumulator hma_;
};

vts0::Mesh loadMesh(const vs::IStream::pointer &is)
{
    auto mesh(geometry::loadBinaryMesh(is->get()));
    is->close();
    return mesh;
}

class Atlas : public vts::Atlas {
public:
    Atlas(const vs::IStream::pointer &stream)
        : stream_(stream)
        , size_(imgproc::jpegSize(*stream, stream->name()))
    {}

private:
    virtual std::size_t size() const UTILITY_OVERRIDE { return 1; }

    virtual vts::multifile::Table serialize_impl(std::ostream &os) const
        UTILITY_OVERRIDE
    {
        stream_->get().seekg(0);
        auto start(os.tellp());
        os << stream_->get().rdbuf();
        auto end(os.tellp());

        vts::multifile::Table table;
        table.entries.emplace_back(start, end - start);
        return table;
    }

    virtual void deserialize_impl(std::istream&
                                  , const boost::filesystem::path&
                                  , const vts::multifile::Table&)
        UTILITY_OVERRIDE
    {
        LOGTHROW(err4, std::runtime_error)
            << "This atlas is serialize-only.";
    }

    virtual math::Size2 imageSize_impl(std::size_t index) const {
        UTILITY_OVERRIDE
        if (index) {
            LOGTHROW(err4, std::runtime_error)
                << "This atlas has just one image.";
        }

        return size_;
    }

    vs::IStream::pointer stream_;
    math::Size2 size_;
};

inline vts0::TileId asVts(const vts::TileId tileId) {
    return vts0::TileId(tileId.lod, tileId.x, tileId.y);
}

inline vts::TileId asVts0(const vts0::TileId tileId) {
    return vts::TileId(tileId.lod, tileId.x, tileId.y);
}

class TextureNormalizer {
public:
    TextureNormalizer(const math::Extents2 &divisionExtents)
        : size_(size(divisionExtents))
        , origin_(-size_.width / 2.0, -size_.height / 2.0)
    {}

    math::Point2 operator()(const math::Point3 &p) const {
        // NB: origin is in the upper-left corner
        return { (p(0) - origin_(0)) / size_.width
                , (p(1) - origin_(1)) / size_.height };
    };

private:
    math::Size2f size_;
    math::Point2 origin_;
};

/** Constructs transformation matrix that maps everything in extents into a grid
 *  of defined size so the grid (0, 0) matches to upper-left extents corner and
 *  grid(gridSize.width - 1, gridSize.width - 1) matches lower-right extents
 *  corner.
 */
inline math::Matrix4 mesh2grid(const math::Extents2 &extents
                              , const math::Size2 &gridSize)
{
    math::Matrix4 trafo(ublas::identity_matrix<double>(4));

    auto es(size(extents));

    // scales
    math::Size2f scale((gridSize.width - 1) / es.width
                       , (gridSize.height - 1) / es.height);

    // scale to grid
    trafo(0, 0) = scale.width;
    trafo(1, 1) = -scale.height;

    // place zero to upper-left corner
    trafo(0, 3) = gridSize.width / 2.0;
    trafo(1, 3) = gridSize.height / 2.0;

    return trafo;
}

/** Geo coordinates to coverage mask mapping.
 * NB: result is in pixel system: pixel centers have integral indices
 */
inline math::Matrix4 mesh2mask(const math::Extents2 &extents
                               , const math::Size2 &gridSize)
{
    math::Matrix4 trafo(boost::numeric::ublas::identity_matrix<double>(4));

    auto es(size(extents));

    // scales
    math::Size2f scale(gridSize.width / es.width
                       , gridSize.height / es.height);

    // scale to grid
    trafo(0, 0) = scale.width;
    trafo(1, 1) = -scale.height;

    // move upper left corner to -0.5, -0.5
    trafo(0, 3) = gridSize.width / 2.0 - 0.5;
    trafo(1, 3) = gridSize.height / 2.0 - 0.5;

    return trafo;
}

template <typename Op>
void rasterizeMesh(const vts0::Mesh &mesh, const math::Matrix4 &trafo
                   , const math::Size2 &rasterSize, Op op)
{
    std::vector<imgproc::Scanline> scanlines;
    cv::Point3f tri[3];
    for (const auto &face : mesh.facets) {
        for (int i : { 0, 1, 2 }) {
            auto p(transform(trafo, mesh.vertices[face.v[i]]));
            tri[i].x = p(0); tri[i].y = p(1); tri[i].z = p(2);
        }

        scanlines.clear();
        imgproc::scanConvertTriangle(tri, 0, rasterSize.height, scanlines);

        for (const auto &sl : scanlines) {
            imgproc::processScanline(sl, 0, rasterSize.width, op);
        }
    }
}

/** Rasterizes mesh to generate mesh mask and mesh heightmap.
 */
void createMeshMask(const vts::TileId &tileId, const math::Extents2 &extents
                    , const vts0::Mesh &mesh, vts::Mesh::CoverageMask &cm)
{
    (void) tileId;

    const auto cms(vts::Mesh::coverageSize());

    // build heights and mask matrices
    cv::Mat mask(cms.height, cms.width, CV_8UC1, cv::Scalar(0));

    // draw all faces into the mask
    rasterizeMesh(mesh, mesh2mask(extents, cms)
                  , cms, [&](int x, int y, float)
    {
        // remember mask
        mask.at<unsigned char>(y, x) = 0xff;
    });

    // convert into rastermask; we are optimistic so we start with full mask
    cm.reset();
    for (int j(0); j < cms.height; ++j) {
        for (int i(0); i < cms.width; ++i) {
            if (!mask.at<std::uint8_t>(j, i)) {
                cm.set(i, j, false);
            }
        }
    }
}

vts::Mesh::pointer
createMesh(const vts::TileId &tileId, const vts0::Mesh &m
           , const math::Extents2 &divisionExtents
           , bool externalTextureCoordinates
           , boost::optional<std::uint16_t> textureLayer)
{
    // just one submesh
    auto mesh(std::make_shared<vts::Mesh>());
    mesh->submeshes.emplace_back();
    auto &sm(mesh->submeshes.back());

    // copy vertices
    TextureNormalizer tn(divisionExtents);
    auto t2g(geo::local2geo(divisionExtents));
    for (const auto &v : m.vertices) {
        // convert v from local coordinates to physical SRS
        sm.vertices.push_back(transform(t2g, v));

        // generate external texture coordinates if instructed
        if (externalTextureCoordinates) { sm.etc.push_back(tn(v)); }
    }

    if (externalTextureCoordinates) {
        sm.textureLayer = textureLayer;
    }

    // copy texture coordinates
    std::transform(m.texcoords.begin(), m.texcoords.end()
                   , std::back_inserter(sm.tc)
                   , [&](const math::Point3 &p)
    {
        return math::Point2(p(0), p(1));
    });

    for (const auto &f : m.facets) {
        sm.faces.emplace_back(f.v[0], f.v[1], f.v[2]);
        sm.facesTc.emplace_back(f.t[0], f.t[1], f.t[2]);
    }

    // create mesh mask
    createMeshMask(tileId, divisionExtents, m, mesh->coverageMask);

    // done
    return mesh;
}

void Encoder::generateHeightMap(const vts::TileId &tileId
                                , const vts0::Mesh &mesh
                                , const math::Extents2 &extents)
{
    auto& hm([&]() -> cv::Mat&
    {
        cv::Mat *t(nullptr);
        UTILITY_OMP(critical)
            t = &hma_.tile(tileId);
        return *t;
    }());

    // invalid heightmap value (i.e. initial value) is +oo and we take minimum
    // of all rasterized heights in given place
    rasterizeMesh(mesh, mesh2grid(extents, hma_.tileSize())
                  , hma_.tileSize()
                  , [&](int x, int y, float z)
    {
        auto &value(hm.at<float>(y, x));
        if (z < value) { value = z; }
    });
}

Encoder::TileResult
Encoder::generate(const vts::TileId &tileId, const vts::NodeInfo &nodeInfo
                  , const TileResult&)
{
    auto vts0Id(asVts(tileId));

    if (!cti_.exists(vts0Id)) {
        // neither this nor any child tile exists -> no data
        return TileResult::Result::noData;
    }

    if (!ti_.exists(vts0Id)) {
        return TileResult::Result::noDataYet;
    }

    // load mesh; NB: mesh is in space division srs, just convert to physical
    // and here we go
    vts0::Mesh mesh;
    vs::IStream::pointer atlasStream;

    UTILITY_OMP(critical)
    {
        // NB: access to tileset is not thread safe!
        mesh = loadMesh(aa_.input(vts0Id, vs::TileFile::mesh));
        atlasStream = aa_.input(vts0Id, vs::TileFile::atlas);
    }

    vts::Encoder::TileResult result;
    auto &tile(result.tile());

    // use original atlas (file is piped)
    tile.atlas = std::make_shared<Atlas>(atlasStream);

    // convert mesh from old one
    tile.mesh = createMesh(tileId, mesh, nodeInfo.node.extents
                           , nodeInfo.node.externalTexture
                           , config_.textureLayer);

    // set credits
    tile.credits = config_.credits;

    if (tileId.lod == ntSourceLod_) {
        // we have to generate source data for navtiles
        generateHeightMap(tileId, mesh, nodeInfo.node.extents);
    }

    return result;
}

void Encoder::finish(vts::TileSet &ts)
{
    HeightMap hm(std::move(hma_), referenceFrame()
                 , config_.dtmExtractionRadius / ntSourceLodPixelSize_);

    HeightMap::BestPosition bestPosition;

    // iterate in nt lod range backwards: iterate from start and invert forward
    // lod into backward lod
    for (const auto fLod : ntLodRange_) {
        const vts::Lod lod(ntLodRange_.min + ntLodRange_.max - fLod);

        // resize heightmap for given lod
        hm.resize(lod);

        // generate and store navtiles
        traverse(ts.tileIndex(), lod
                 , [&](const vts::TileId &tileId, vts::QTree::value_type mask)
        {
            // process only tiles with mesh
            if (!(mask & vts::TileIndex::Flag::mesh)) { return; }

            if (auto nt = hm.navtile(tileId)) {
                ts.setNavTile(tileId, *nt);
            }
        });

        if (lod == ntLodRange_.max) {
            bestPosition = hm.bestPosition();
        }
    }

    {
        vr::Position pos;
        pos.position = bestPosition.location;

        pos.type = vr::Position::Type::objective;
        pos.heightMode = vr::Position::HeightMode::fixed;
        pos.orientation = { 0.0, -90.0, 0.0 };
        pos.verticalExtent = bestPosition.verticalExtent;
        pos.verticalFov = 90;
        ts.setPosition(pos);
    }
}

int Vts02Vts::run()
{
    // open vts0 tileset
    auto input(vts0::openTileSet(input_));

    {
        auto oldprop(input->getProperties());
        properties_.id = oldprop.id;
        properties_.referenceFrame = oldprop.referenceFrame;
    }

    // run the encoder
    Encoder(output_, properties_, createMode_, input, config_).run();

    // all done
    LOG(info4) << "All done.";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char *argv[])
{
    return Vts02Vts()(argc, argv);
}
