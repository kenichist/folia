// Frozen v1 module contracts. The walking skeleton is wired through exactly
// these six calls: openPage, fetchChildren, applyEdit, commit, renderPreview,
// dispatchCommand. Implementations:
//   IStore           -> storage/Store        (one background writer, one reader)
//   IPageSession     -> editor/PageSession   (QTextDocument-backed page buffer)
//   IPreviewService  -> math/PreviewService  (bounded worker pool, byte-capped cache)
//   ICommandBus      -> shell/CommandRegistry (validated; shared by Lua and native UI)
//
// Threading: every callback is delivered on the thread of `ctx` (queued), and
// is dropped if `ctx` has been destroyed. No callback ever runs Lua.
#pragma once

#include "Types.h"

#include <QColor>
#include <QObject>
#include <QVariantMap>
#include <memory>

namespace folia {

// ---------------------------------------------------------------- storage --
class IStore {
public:
    virtual ~IStore() = default;

    // Query: full content of one page (blocks only; never descendants).
    virtual void openPage(PageId id, QObject *ctx,
                          std::function<void(PageContent)> done) = 0;

    // Query: one page of child metadata, ordered by sibling order. `total` is
    // the full child count so views can size scrollbars without loading rows.
    virtual void fetchChildren(PageId parent, int offset, int limit, QObject *ctx,
                               std::function<void(QVector<PageMeta> rows, int total)> done) = 0;

    // Command: make `batch` durable. Batches for the same page that are still
    // queued are coalesced (newer wins). Exactly one of committed/failed is
    // reported per page revision that reaches the writer.
    virtual void commit(SaveBatch batch) = 0;
};

// ----------------------------------------------------------------- editor --
// Programmatic edit applied to the open page (commands, link maintenance,
// tests). User typing goes straight through the native text control and is
// reported through the same revision counter.
struct Edit {
    enum class Op { InsertText, ReplaceBlockText, InsertBlock, RemoveBlock };
    Op op = Op::InsertText;
    BlockId block = 0;         // target / anchor block
    qint32 offset = 0;         // character offset inside the block (InsertText)
    QString text;
    BlockRecord record;        // InsertBlock payload
    bool after = true;         // InsertBlock: after (true) or before `block`
    bool undoable = true;
};

class IPageSession {
public:
    virtual ~IPageSession() = default;
    virtual PageId pageId() const = 0;
    virtual Revision revision() const = 0;
    virtual Revision committedRevision() const = 0;
    // Returns the new revision, or 0 if the edit was rejected (unknown block...).
    virtual Revision applyEdit(const Edit &edit) = 0;
    // Snapshot everything changed since the last snapshot; empty batch (rev==0)
    // when clean. Called by the autosave scheduler on the main thread.
    virtual SaveBatch takeSaveBatch() = 0;
};

// ---------------------------------------------------------------- preview --
enum class PreviewKind : quint8 { InlineMath, DisplayMath };

struct PreviewRequest {
    BlockId block = 0;
    Revision blockRev = 0;      // revision of the source being rendered
    PreviewKind kind = PreviewKind::DisplayMath;
    QString source;
    qreal pointSize = 14;
    QColor color;
};

class MathDisplayList; // math/MathRenderer.h

struct PreviewResult {
    BlockId block = 0;
    Revision blockRev = 0;
    bool ok = false;
    QString error;              // when !ok: editable source stays visible
    int errorOffset = -1;
    std::shared_ptr<const MathDisplayList> list;
    qint64 renderMicros = 0;
    bool cacheHit = false;
};

class IPreviewService {
public:
    virtual ~IPreviewService() = default;
    // Supersedes any outstanding request for the same block. The callback is
    // invoked only for the newest request of that block.
    virtual void renderPreview(const PreviewRequest &req, QObject *ctx,
                               std::function<void(PreviewResult)> done) = 0;
    virtual void cancel(BlockId block) = 0;
    // Synchronous cache probe (main thread); never renders. Optional.
    virtual bool lookup(const PreviewRequest &, PreviewResult *) { return false; }
    // Render now on the calling thread and cache the result, when that is
    // cheap (renderer warmed up, within maxMillis). False: use renderPreview.
    virtual bool renderNow(const PreviewRequest &, PreviewResult *, int /*maxMillis*/) { return false; }
};

// --------------------------------------------------------------- commands --
struct CommandResult {
    bool ok = false;
    QString error;
};

class ICommandBus {
public:
    virtual ~ICommandBus() = default;
    virtual CommandResult dispatchCommand(const QString &name, const QVariantMap &args) = 0;
};

} // namespace folia
