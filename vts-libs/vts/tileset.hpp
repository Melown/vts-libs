/**
 * \file vts/tileset.hpp
 * \author Vaclav Blazek <vaclav.blazek@citationtech.net>
 *
 * Tile set access.
 */

#ifndef vadstena_libs_vts_tileset_hpp_included_
#define vadstena_libs_vts_tileset_hpp_included_

#include <memory>
#include <cmath>
#include <list>
#include <string>
#include <array>

#include <boost/optional.hpp>
#include <boost/noncopyable.hpp>
#include <boost/filesystem/path.hpp>

#include "utility/runnable.hpp"

#include "../storage/lod.hpp"
#include "../storage/range.hpp"
#include "../storage/resources.hpp"

#include "./basetypes.hpp"
#include "./types.hpp"
#include "./mesh.hpp"
#include "./metatile.hpp"
#include "./atlas.hpp"
#include "./tileset/properties.hpp"
#include "./mapconfig.hpp"
#include "./tileindex.hpp"
#include "./tilesource.hpp"
#include "./options.hpp"

namespace vadstena { namespace vts {

/** Driver that implements physical aspects of tile set.
 */
class Driver;

/** TileSet interface.
 */
class TileSet {
public:
    ~TileSet();

    /** Get tile set propeties.
     * \return tile set properties
     */
    TileSetProperties getProperties() const;

    /** Sets new position.
     */
    void setPosition(const registry::Position &position);

    /** Adds given credits into list of credits used by this tileset.
     */
    void addCredits(const registry::IdSet &credits);

    /** Adds given bound layers into list of bound layers used by this tileset.
     */
    void addBoundLayers(const registry::IdSet &boundLayers);

    /** Generates map configuration for this single tile set.
     * \param root root path of all datasets (surfaces and glues)
     * \param includeExtra include extra configuration in the output.
     * \return map configuration
     */
    MapConfig mapConfig(bool includeExtra = true) const;

    /** Returns tile's mesh.
     */
    Mesh getMesh(const TileId &tileId) const;

    /** Returns tiles' atlas.
     */
    void getAtlas(const TileId &tileId, Atlas &atlas) const;

    /** Set tile content.
     *  \param tileId tile identifier
     *  \param tile tile content
     */
    void setTile(const TileId &tileId, const Tile &tile);

    /** Set tile content.
     *
     * Supplied nodeInfo must be correct!
     *
     *  \param tileId tile identifier
     *  \param tile tile content
     *  \param nodeInfo information about node
     */
    void setTile(const TileId &tileId, const Tile &tile
                 , const NodeInfo &nodeInfo);

    /** Set tile content.
     *  \param tileId tile identifier
     *  \param tile tile content source
     */
    void setTile(const TileId &tileId, const TileSource &tile);

    /** Set tile content.
     *
     * Supplied nodeInfo must be correct!
     *
     *  \param tileId tile identifier
     *  \param tile tile content source
     *  \param nodeInfo information about node
     */
    void setTile(const TileId &tileId, const TileSource &tile
                 , const NodeInfo &nodeInfo);

    /** Sets tile's navtile. Tile must already have mesh.
     *  Only navtile is stored.
     */
    void setNavTile(const TileId &tileId, const NavTile &navtile);

    /** Returns tile's navtile.
     */
    void getNavTile(const TileId &tileId, NavTile &navtile) const;

    /** Returns tile's metanode.
     */
    MetaNode getMetaNode(const TileId &tileId) const;

    /** Returns metatile.
     */
    MetaTile getMetaTile(const TileId &metaId) const;

    /** Returns ID of metatile tileId belongs to.
     */
    TileId metaId(const TileId &tileId) const;

    /** Returns tile's content source.
     */
    TileSource getTileSource(const TileId &tileId) const;

    /** Returns reference for given tile.
     *  Reference is number > 0. 0 means no reference.
     */
    int getReference(const TileId &tileId) const;

    /** Checks whether tile exist.
     */
    bool exists(const TileId &tileId) const;

    /** Checks whether tile extist and covers whole tile.
     */
    bool fullyCovered(const TileId &tileId) const;

    /** Flushes tileset.
     *
     * Must be called before close otherwise tileset is useless.
     */
    void flush();

    /** Empties any cached content at once. Fails if there are any modified data.
     */
    void emptyCache() const;

    /** Starts watching runnable without entering a transaction.
     */
    void watch(utility::Runnable *runnable);

    /** Is the tile set empty (i.e. has it no tile?)
     */
    bool empty() const;

    /** Remove storage.
     */
    void drop();

    /** Returns lod range covered by tiles.
     */
    LodRange lodRange() const;

    /** Referce frame in charge.
     */
    registry::ReferenceFrame referenceFrame() const;

    /** Returns constant driver. Used in delivery system.
     */
    const Driver& driver() const;

    /** Root path of this tileset.
     */
    boost::filesystem::path root() const;

    /** Get tileset's ID.
     */
    std::string id() const { return getProperties().id; }

    /** Returns tile index.
     */
    const TileIndex& tileIndex() const;

    /** Returns tile index in given lod range
     */
    TileIndex tileIndex(const LodRange &lodRange) const;

    /** Returns sphere of influence of this tileset.
     *
     *  SoI are all tiles that have giventype and also tiles above and below
     *  them.
     *
     * \param range optional LOD range in which to generate the SoI;
     *              defaults to tileset's LOD range
     * \param type type of tile to check
     * \return SoI where only influenced tile has non-zero value
     */
    TileIndex sphereOfInfluence(const LodRange &range = LodRange::emptyRange()
                                , TileIndex::Flag::value_type type
                                = TileIndex::Flag::mesh)
        const;

    typedef std::vector<const TileSet*> const_ptrlist;

    /** Creates glue from given sets into this set.
     *
     *  Priority grows from left to right.
     */
    void createGlue(const const_ptrlist &sets, int textureQuality);

    bool externallyChanged() const;

    /** Returns time of last modification. Recorded at read-only open.
     */
    std::time_t lastModified() const;

    /** Returns information about used resources.
     */
    storage::Resources resources() const;

    /** Tells caller whether this tileset can contain given tile set.
     */
    bool canContain(const NodeInfo &nodeInfo) const;

    /** Pastes other tileset into this one.
     */
    void paste(const TileSet &src
               , const boost::optional<LodRange> &lodRange = boost::none);

    /** Returns type information.
     */
    std::string typeInfo() const;

    /** Returns mapConfig for given path.
     *
     * \param root root path of all datasets (surfaces and glues)
     * \param includeExtra include extra configuration in the output.
     * \return map configuration
     */
    static MapConfig mapConfig(const boost::filesystem::path &root
                               , bool includeExtra = true);

    /** Check for tileset at given path.
     */
    static bool check(const boost::filesystem::path &root);

    static void relocate(const boost::filesystem::path &root
                         , const RelocateOptions &options
                         , const std::string &prefix = "");

    /** Internals. Public to ease library developers' life, not to allow users
     *  to put their dirty hands in the tileset's guts!
     */
    struct Detail;

    /** Full tileset properties.
     */
    typedef FullTileSetProperties Properties;

private:
    TileSet(const std::shared_ptr<Driver> &driver
            , const TileSet::Properties &properties);
    TileSet(const std::shared_ptr<Driver> &driver);

    std::shared_ptr<Detail> detail_;

public:
    Detail& detail() { return *detail_; }
    const Detail& detail() const { return *detail_; }

    /** Needed to instantiate.
     */
    class Factory; friend class Factory;
};

/** Low-level create operation.
 */
TileSet createTileSet(const boost::filesystem::path &path
                      , const TileSet::Properties &properties
                      , CreateMode mode);

} } // namespace vadstena::vts

#endif // vadstena_libs_vts_tileset_hpp_included_
