#include "core/Types.h"

#include <QHash>
#include <QSet>

namespace folia {

qint64 SaveBatch::payloadBytes() const
{
    qint64 n = title.size() * 2;
    for (const BlockRecord &b : upserts)
        n += (b.text.size() + b.lang.size()) * 2 + qint64(sizeof(BlockRecord));
    n += (deletes.size() + order.size()) * qint64(sizeof(BlockId));
    return n;
}

void SaveBatch::mergeNewer(const SaveBatch &newer)
{
    Q_ASSERT(newer.page == page);
    rev = qMax(rev, newer.rev);
    if (newer.hasTitle) {
        hasTitle = true;
        title = newer.title;
    }
    // Upserts: newer content wins; a block deleted by the newer batch is dropped.
    QSet<BlockId> newerDeletes(newer.deletes.cbegin(), newer.deletes.cend());
    QHash<BlockId, int> index;
    QVector<BlockRecord> merged;
    merged.reserve(upserts.size() + newer.upserts.size());
    for (const BlockRecord &b : std::as_const(upserts)) {
        if (newerDeletes.contains(b.id))
            continue;
        index.insert(b.id, merged.size());
        merged.push_back(b);
    }
    for (const BlockRecord &b : newer.upserts) {
        auto it = index.find(b.id);
        if (it != index.end())
            merged[*it] = b;
        else {
            index.insert(b.id, merged.size());
            merged.push_back(b);
        }
    }
    upserts = std::move(merged);
    // Deletes: union, minus blocks the newer batch (re)creates.
    QSet<BlockId> del(deletes.cbegin(), deletes.cend());
    for (BlockId id : newer.deletes)
        del.insert(id);
    for (const BlockRecord &b : newer.upserts)
        del.remove(b.id);
    deletes = QVector<BlockId>(del.cbegin(), del.cend());
    if (newer.hasOrder) {
        hasOrder = true;
        order = newer.order;
    }
    if (newer.hasSelection) {
        hasSelection = true;
        selection = newer.selection;
    }
}

} // namespace folia
