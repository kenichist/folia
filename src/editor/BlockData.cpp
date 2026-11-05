#include "editor/BlockData.h"


namespace folia {

BlockData::BlockData(BlockId i, BlockTracker *t) : id(i), tracker(t)
{
    if (tracker)
        ++tracker->liveBlocks;
}

BlockData::~BlockData()
{
    if (tracker && !tracker->detached) {
        --tracker->liveBlocks;
        tracker->structureChanged = true;
    }
}

} // namespace folia
