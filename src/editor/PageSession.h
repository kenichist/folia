// One open page: a QTextDocument holding the page as plain source lines, its
// virtual layout, stable block identities, the page revision counter and the
// save snapshot logic. Implements IPageSession.
#pragma once

#include "core/Api.h"
#include "editor/BlockData.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTextBlock>
#include <QTextCursor>

class QTextDocument;

namespace folia {

class EditorContext;
class VirtualLayout;

class PageSession : public QObject, public IPageSession {
    Q_OBJECT
public:
    using IdAllocator = std::function<BlockId()>;

    PageSession(PageId id, EditorContext *ctx, IdAllocator alloc, QObject *parent = nullptr);
    ~PageSession() override;

    // Build from stored content (not undoable; revision = stored revision).
    void load(const PageContent &pc);
    // A brand-new page (nothing stored yet).
    void initNew(PageId parent, const QString &title);

    QTextDocument *document() const { return m_doc; }
    VirtualLayout *layout() const { return m_layout; }

    // IPageSession
    PageId pageId() const override { return m_id; }
    Revision revision() const override { return m_rev; }
    Revision committedRevision() const override { return m_committedRev; }
    Revision applyEdit(const Edit &edit) override;
    SaveBatch takeSaveBatch() override;

    PageId parentId() const { return m_parent; }
    void setParentId(PageId p) { m_parent = p; }
    QString title() const { return m_title; }
    void setTitle(const QString &title);
    void setCommitted(Revision rev);
    bool hasUnsnapshotted() const;
    bool isUncommitted() const { return m_rev > m_committedRev; }

    // Live caret/selection/scroll for this page (kept while cached, persisted
    // as (block id, offset) anchors).
    void setViewState(int anchor, int position, int scrollY);
    int viewAnchor() const { return m_anchor; }
    int viewPosition() const { return m_position; }
    int viewScrollY() const { return m_scrollY; }
    SelectionAnchor anchorsFor(int anchor, int position, int scrollY) const;

    qint64 estimatedBytes() const;
    int lineCount() const;

    // Child page links (page link lines).
    QVector<PageId> linkTargets() const;
    void appendPageLink(PageId target);
    void insertPageLinkAfter(int position, PageId target);
    void removePageLinks(PageId target);
    // Called when a linked page's title changed so chips re-measure.
    void refreshLinkTo(PageId target);

    static LineRole roleOf(const QTextBlock &b);
    static BlockData *dataOf(const QTextBlock &b);
    // First line of the stored segment containing `b`.
    static QTextBlock segmentHead(QTextBlock b);
    static QTextCharFormat pageLinkFormat(PageId target);

    BlockTracker *tracker() { return &m_tracker; }
    // While set, document notifications are presentation-only (IME preedit
    // updates): they neither bump the revision nor mark blocks dirty.
    void setPresentationOnly(bool on) { m_presentationOnly = on; }

signals:
    // Emitted at the start of destruction, while the document is still alive,
    // so views can detach before it is deleted.
    void closing();
    void revisionChanged(folia::Revision rev);
    void committedChanged(folia::Revision rev);
    void titleChanged(const QString &title);

private:
    void attachLayout();
    void onContentsChange(int pos, int removed, int added);
    void propagateRoles(QTextBlock first, QTextBlock last);
    BlockData *ensureData(QTextBlock &b);
    LineRole classify(const QTextBlock &b, int carry) const;
    QTextBlock findBlockById(BlockId id) const;

    PageId m_id;
    PageId m_parent = 0;
    EditorContext *m_ctx;
    IdAllocator m_alloc;
    QTextDocument *m_doc = nullptr;
    VirtualLayout *m_layout = nullptr;
    BlockTracker m_tracker;
    QString m_title;
    bool m_titleDirty = false;
    bool m_viewDirty = false;
    bool m_building = false;
    bool m_presentationOnly = false;
    Revision m_rev = 0;
    Revision m_snapshotRev = 0;
    Revision m_committedRev = 0;
    int m_lastDocRevision = -1;
    QVector<BlockId> m_savedOrder;
    QSet<BlockId> m_savedIds;
    int m_anchor = 0;
    int m_position = 0;
    int m_scrollY = 0;
};

} // namespace folia
