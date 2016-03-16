#ifndef vadstena_libs_vts_nodeinfo_hpp_included_
#define vadstena_libs_vts_nodeinfo_hpp_included_

#include <boost/logic/tribool.hpp>

#include "imgproc/rastermask/quadtree.hpp"

#include "./basetypes.hpp"

namespace vadstena { namespace vts {

class RFTreeSubtree {
public:
    RFTreeSubtree(const RFNode &root)
        : root_(&root)
    {}

    const RFNode& root() const { return *root_; }

    const RFNode::Id& id() const { return root_->id; }

    bool operator==(const RFTreeSubtree &other) const {
        return (root_ == other.root_);
    }

    /** Calculates node validity:
     *    * false: node is completely outside subtree's valid area
     *    * true: node is completely inside subtree's valid area
     *    * indeterminate: node is partially inside subtree's valid area
     */
    boost::tribool valid(const RFNode &node) const;

    /** Node coverage mask.
     */
    typedef imgproc::quadtree::RasterMask CoverageMask;

    /** Node coverage mask type: pixel or grid.
     */
    enum class CoverageType { pixel, grid };

    CoverageMask coverageMask(CoverageType type, const math::Size2 &size
                              , const RFNode &node) const;

private:
    bool initSampler() const;

    struct Sampler;

    const RFNode *root_;
    mutable std::shared_ptr<Sampler> sampler_;
};

/** Reference frame node information.
 */
class NodeInfo {
public:
    /** Creates node info from reference frame and tileId.
     *
     * Root node is found in reference frame and then current node is derived.
     */
    NodeInfo(const registry::ReferenceFrame &referenceFrame
             , const TileId &tileId);

    /** Root node info.
     */
    NodeInfo(const registry::ReferenceFrame &referenceFrame);

    /** Node.
     */
    const RFNode& node() const { return node_; }

    /** Node id.
     */
    const RFNode::Id& nodeId() const { return node_.id; }

    const math::Extents2& extents() const { return node_.extents; }

    const std::string& srs() const { return node_.srs; }

    /** Distance from root.
     */
    Lod distanceFromRoot() const { return node_.id.lod - subtree_.id().lod; }

    /** Returns child node. Uses same child assignment as children() functiom
     *  children() from tileop.
     */
    NodeInfo child(Child child) const;

    bool valid() const { return node_.valid(); }

    const RFTreeSubtree& subtree() const { return subtree_; }

    const registry::ReferenceFrame& referenceFrame() const {
        return *referenceFrame_;
    }

    /** Partial node is not fully inside valid bounds.
     *
     *  NB: node that is fully outside valid bounds is marked as invalid.
     */
    bool partial() const { return partial_; }

    typedef RFTreeSubtree::CoverageMask CoverageMask;
    typedef RFTreeSubtree::CoverageType CoverageType;

    /** Computes coverage mask:
     *    * invalid node: fully black
     *    * non-partial valid node: fully white
     *    * partial valid node: generated mask based on node constraints
     */
    CoverageMask coverageMask(CoverageType type, const math::Size2 &size)
        const;

private:
    /** Node info.
     */
    NodeInfo(const registry::ReferenceFrame &referenceFrame
             , const RFNode &node);

    /** Associated reference frame
     */
    const registry::ReferenceFrame *referenceFrame_;

    /** Subtree this node belongs to
     */
    RFTreeSubtree subtree_;

    /** Node.
     */
    RFNode node_;

    /** Partial node is partially inside valid bounds.
     *
     *  NB: node that is fully outside valid bounds is marked as invalid!
     */
    bool partial_;
};

/** Checks compatibility of two nodes.
 *  Both nodes must be in the same subtree
 */
bool compatible(const NodeInfo &ni1, const NodeInfo &ni2);

// inline functions

inline NodeInfo::NodeInfo(const registry::ReferenceFrame &referenceFrame)
    : referenceFrame_(&referenceFrame), subtree_(referenceFrame.root())
    , node_(subtree_.root()), partial_(false)
{}

inline bool compatible(const NodeInfo &ni1, const NodeInfo &ni2)
{
    return (ni1.subtree() == ni2.subtree());
}

} } // namespace vadstena::vts

#endif // vadstena_libs_vts_nodeinfo_hpp_included_