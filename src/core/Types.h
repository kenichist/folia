// Frozen v1 interface types shared by every module.
//
// Identity and revision semantics (see docs/ARCHITECTURE.md §2):
//  * PageId / BlockId are positive 64-bit integers allocated in-process by
//    IdAllocator, never reused. 0 means "none" (for PageId: the workspace root).
//  * Revision is per page and strictly increasing for the lifetime of the
//    workspace. Every user-visible edit bumps it. Storage persists the last
//    committed revision, so revisions keep increasing across restarts.
//  * "Saved" may be shown for a page only when committedRevision == revision.
#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>
#include <functional>

namespace folia {

using PageId = qint64;
using BlockId = qint64;
using Revision = quint64;

// Stored kind of a content block. Text, math and code source is authoritative;
// any preview data derived from it is disposable.
enum class BlockKind : quint8 {
    Text = 0,     // one paragraph line; markdown-ish prefixes (#, -, 1.) live in text
    Code = 1,     // fenced code; `lang` = info string, `text` = inner source ('\n' separated)
    Math = 2,     // display math; `text` = TeX source without the $$ fences
    PageLink = 3, // link to a child page; `target` = page id, `text` unused
};

struct PageMeta {
    PageId id = 0;
    PageId parent = 0;
    qint64 ord = 0;       // explicit sibling order (sparse integer keys)
    QString title;
    qint32 childCount = 0;
    Revision rev = 0;     // last committed revision
};

struct BlockRecord {
    BlockId id = 0;
    BlockKind kind = BlockKind::Text;
    QString lang;
    QString text;
    PageId target = 0;
};

// Caret/selection persisted per page as (block, offset) pairs so they survive
// edits elsewhere in the page.
struct SelectionAnchor {
    BlockId anchorBlock = 0;
    qint32 anchorOffset = 0;
    BlockId posBlock = 0;
    qint32 posOffset = 0;
    qint32 scrollY = 0;
};

struct PageContent {
    PageId id = 0;
    PageId parent = 0;
    QString title;
    Revision rev = 0;
    QVector<BlockRecord> blocks; // in document order
    SelectionAnchor selection;
    bool found = false;
};

// One durable unit of work for a page: everything needed to bring the stored
// page to revision `rev`. Produced by the editor session, consumed by the
// storage writer. Superseded batches for the same page are merged (see
// SaveBatch::mergeNewer) so the writer never falls behind the editor.
struct SaveBatch {
    PageId page = 0;
    Revision rev = 0;
    bool hasTitle = false;
    QString title;
    QVector<BlockRecord> upserts;   // changed or new blocks (content only)
    QVector<BlockId> deletes;       // blocks no longer in the page
    bool hasOrder = false;
    QVector<BlockId> order;         // full block order, present when structure changed
    bool hasSelection = false;
    SelectionAnchor selection;

    qint64 payloadBytes() const;
    // Fold a newer batch for the same page into this one (newer wins).
    void mergeNewer(const SaveBatch &newer);
};

enum class MoveResult { Ok, Cycle, NotFound, InvalidIndex, StorageError };

struct StorageError {
    int sqliteCode = 0;
    QString message;
    bool diskFull = false;
};

} // namespace folia
