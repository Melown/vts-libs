#include <set>
#include <map>
#include <memory>

#include <boost/noncopyable.hpp>
#include <boost/format.hpp>

#include "utility/runnable.hpp"

#include "imgproc/png.hpp"

#include "../../../registry.hpp"
#include "../../../storage/sstreams.hpp"

#include "../../io.hpp"
#include "../../2d.hpp"
#include "../../tileset.hpp"
#include "../../debug.hpp"

#include "../delivery.hpp"
#include "../driver.hpp"
#include "../tilesetindex.hpp"
#include "../config.hpp"

namespace vadstena { namespace vts {

namespace fs = boost::filesystem;
namespace vs = vadstena::storage;

namespace {

std::shared_ptr<tileset::Index>
indexFromDriver(const FullTileSetProperties &properties
                , const Driver::pointer &driver)
{
    if (auto *i = driver->getTileIndex()) {
        // driver provides tile index, reuse with empty deleter
        return { i, [](void*) {} };
    }

    // no tileindex available -> load
    auto index(std::make_shared<tileset::Index>
               (registry::system.referenceFrames
                (properties.referenceFrame).metaBinaryOrder));

    tileset::loadTileSetIndex(*index, *driver);
    return index;
}

std::string filename(const fs::path &root, const TileId &tileId, TileFile type)
{
    return (root / str(boost::format("%s.%s") % tileId % type)).string();
}

IStream::pointer meta2d(const Driver &driver
                        , const tileset::Index &index
                        , const TileId &tileId
                        , bool noSuchFile)
{
    if (!Meta2d::isMetaId(tileId)) {
        if (noSuchFile) {
            LOGTHROW(err1, storage::NoSuchFile)
                << "Tile ID " << tileId << " is not valid for 2d metatile.";
        }
        return {};
    }

    // generate mask image from tileindex, serialize it as a png and wrap in
    // input stream
    return vs::memIStream(TileFile::mask
                          , imgproc::png::serialize
                          (meta2d(index.tileIndex, tileId), 9)
                          , driver.lastModified()
                          , filename(driver.root(), tileId, TileFile::meta2d));
}

namespace constants {

const auto EmptyMask = imgproc::png::serialize(emptyDebugMask(), 9);

} // namespace constants

IStream::pointer mask(const Driver &driver, const TileId &tileId
                      , FileFlavor flavor, bool noSuchFile)
{
    const auto debug(flavor == FileFlavor::debug);

    auto is((noSuchFile && !debug)
            ? driver.input(tileId, TileFile::mesh)
            : driver.input(tileId, TileFile::mesh, NullWhenNotFound));

    if (!is) {
        if (debug) {
            // no data and debug mode -> return transparent tile
            return vs::memIStream
                (TileFile::mask, constants::EmptyMask
                 , -1, filename(driver.root(), tileId, TileFile::mask));
        }

        // not found
        return {};
    }

    // generate mask image from mask, serialize it as a png and wrap in input
    // stream

    const auto meshMask(loadMeshMask(is));
    const auto png(debug
                   ? imgproc::png::serialize
                   (debugMask(meshMask, driver.capabilities().flattener), 9)
                   : imgproc::png::serialize
                   (mask2d(meshMask, driver.capabilities().flattener), 9));
    return vs::memIStream(TileFile::mask
                          , png, is->stat().lastModified
                          , filename(driver.root(), tileId, TileFile::mask));
}

IStream::pointer meta(const Driver &driver, const tileset::Index &index
                      , const FullTileSetProperties &properties
                      , const TileId &tileId, FileFlavor flavor
                      , bool noSuchFile)
{
    if (flavor != FileFlavor::debug) {
        return (noSuchFile
                ? driver.input(tileId, TileFile::meta)
                : driver.input(tileId, TileFile::meta, NullWhenNotFound));
    }

    // generate debug metanode
    const auto debugNode(getNodeDebugInfo(index.tileIndex, tileId));

    std::ostringstream os;
    saveDebug(os, debugNode);
    return vs::memIStream("application/json; charset=utf-8"
                          , os.str(), -1 // where to get last modified?
                          , filename(driver.root(), tileId, TileFile::meta));
    (void) properties;
}

bool creditsFromMetatiles(const Driver &driver
                          , const tileset::Index &index
                          , const TileId &creditsId
                          , bool noSuchFile
                          , registry::IdSet &credits
                          , std::size_t maxCount
                          , std::time_t &lastModified)
{
    auto metaOrder(index.metaBinaryOrder());
    if (CreditTile::binaryOrder < metaOrder) {
        LOGTHROW(err3, storage::Error)
            << "unimplemented: cannot create credit tile from bigger "
            "3D metatiles";
    }

    int depthDiff(CreditTile::binaryOrder - metaOrder);
    int count(1 << depthDiff);
    int skip(1 << metaOrder);
    for (int j(0); j < count; ++j) {
        int row(creditsId.y + j * skip);
        for (int i(0); i < count; ++i) {
            TileId metaId(creditsId.lod, creditsId.x + i * skip, row);
            if (!index.meta(metaId)) { continue; }

            LOG(info4) << "existing metaId: " << metaId;
            auto is(noSuchFile
                    ? driver.input(metaId, TileFile::meta)
                    : driver.input(metaId, TileFile::meta, NullWhenNotFound));
            if (!is) { return false; }
            loadCreditsFromMetaTile(is->get(), credits, is->name());

            // update last modified
            auto lm(is->stat().lastModified);
            if (lm > lastModified) { lastModified = lm; }
            is->close();

            // all credits seen
            if (credits.size() >= maxCount) { return true; }
        }
    }

    return true;
}

IStream::pointer credits(const Driver &driver
                         , const tileset::Index &index
                         , const FullTileSetProperties &properties
                         , const TileId &tileId
                         , bool noSuchFile)
{
    if (!CreditTile::isCreditId(tileId)) {
        if (noSuchFile) {
            LOGTHROW(err1, storage::NoSuchFile)
                << "Tile ID " << tileId << " is not valid for 2d credit tile.";
        }
        return {};
    }

    CreditTile tile;

    std::time_t lastModified(-1);

    auto extract([&](const registry::IdSet &credits)
    {
        for (const auto &cid : credits) {
            if (const auto *credit = registry::system.credits
                (cid, std::nothrow))
            {
                tile.credits.set(credit->id, boost::none);
            }
        }
    });

    if (properties.credits.size() <= 1) {
        extract(properties.credits);
    } else {
        registry::IdSet credits;
        if (!creditsFromMetatiles(driver, index, CreditTile::creditsId(tileId)
                                  , noSuchFile, credits
                                  , properties.credits.size()
                                  , lastModified))
        {
            return {};
        }

        extract(credits);
        if (lastModified <= 0) {
            lastModified = driver.lastModified();
        }
    }

    auto s(std::make_shared<StringIStream>
           (TileFile::credits
            , filename(driver.root(), tileId, TileFile::credits)
            , lastModified));

    // serialize credit tile
    saveCreditTile(s->sink(), tile, false);
    s->updateSize();

    // done
    return s;
}

IStream::pointer filterConfig(const IStream::pointer &raw)
{
    // load config and reset driver
    auto props(tileset::loadConfig(raw->get(), raw->name()));
    props.driverOptions = boost::any();
    auto s(std::make_shared<storage::StringIStream>(*raw));
    tileset::saveConfig(s->sink(), props);
    s->updateSize();
    return s;
}

} // namespace

Delivery::Delivery(AccessToken, const boost::filesystem::path &root)
    : driver_(Driver::open(root))
    , properties_(tileset::loadConfig(*driver_))
    , index_(indexFromDriver(properties_, driver_))
{}

Delivery::pointer Delivery::open(const boost::filesystem::path &root)
{
    return std::make_shared<Delivery>(AccessToken{}, root);
}

IStream::pointer Delivery::input(File type) const
{
    switch (type) {
    case File::config:
        return filterConfig(driver_->input(type));
    default: break;
    }
    return driver_->input(type);
}

IStream::pointer Delivery::input(File type, const NullWhenNotFound_t&) const
{
    switch (type) {
    case File::config:
        return filterConfig(driver_->input(type, NullWhenNotFound));
    default: break;
    }
    return driver_->input(type, NullWhenNotFound);
}

IStream::pointer Delivery::input(const TileId &tileId, TileFile type
                                 , FileFlavor flavor) const
{
    switch (type) {
    case TileFile::meta2d:
        return meta2d(*driver_, *index_, tileId, true);

    case TileFile::mask:
        return mask(*driver_, tileId, flavor, true);

    case TileFile::credits:
        return credits(*driver_, *index_, properties_, tileId, true);

    case TileFile::meta:
        return meta(*driver_, *index_, properties_, tileId, flavor, true);

    default:
        return driver_->input(tileId, type);
    }
}

IStream::pointer Delivery::input(const TileId &tileId, TileFile type
                                 , FileFlavor flavor
                                 , const NullWhenNotFound_t&) const
{
    switch (type) {
    case TileFile::meta2d:
        return meta2d(*driver_, *index_, tileId, false);

    case TileFile::mask:
        return mask(*driver_, tileId, flavor, false);

    case TileFile::credits:
        return credits(*driver_, *index_, properties_, tileId, false);

    case TileFile::meta:
        return meta(*driver_, *index_, properties_, tileId, flavor, false);

    default:
        return driver_->input(tileId, type, NullWhenNotFound);
    }
}

FileStat Delivery::stat(File type) const
{
    return driver_->stat(type);
}

FileStat Delivery::stat(const TileId &tileId, TileFile type) const
{
    return driver_->stat(tileId, type);
}

IStream::pointer Delivery::input(const std::string &name) const
{
    return driver_->input(name);
}

IStream::pointer Delivery::input(const std::string &name
                                 , const NullWhenNotFound_t&) const
{
    return driver_->input(name, NullWhenNotFound);
}

FileStat Delivery::stat(const std::string &name) const
{
    return driver_->stat(name);
}

Resources Delivery::resources() const
{
    return driver_->resources();
}

bool Delivery::externallyChanged() const
{
    return driver_->externallyChanged();
}

std::time_t Delivery::lastModified() const
{
    return driver_->lastModified();
}

MapConfig Delivery::mapConfig(bool includeExtra) const
{
    return TileSet::mapConfig(*driver_, includeExtra);
}

MeshTilesConfig Delivery::meshTilesConfig(bool includeExtra) const
{
    return TileSet::meshTilesConfig(*driver_, includeExtra);
}

} } // namespace vadstena::vts
