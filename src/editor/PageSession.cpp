#include "editor/PageSession.h"

#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/Links.h"
#include "editor/VirtualLayout.h"
#include "storage/AttachmentStore.h"

#include <QTextDocument>
#include <QTextFragment>

namespace folia {

namespace {
enum Carry { Normal = 0, InCode = 1, InMath = 2 };

int carryOf(LineRole r)
{
    if (roleInCode(r))
        return InCode;
    if (roleInMath(r))
        return InMath;
    return Normal;
}

const QChar kObj = QChar::ObjectReplacementCharacter;
const QString kFence = QStringLiteral("```");
const QString kMath = QStringLiteral("$$");
} // namespace

PageSession::PageSession(PageId id, EditorContext *ctx, IdAllocator alloc, QObject *parent)
    : QObject(parent), m_id(id), m_ctx(ctx), m_alloc(std::move(alloc))
{
    m_doc = new QTextDocument(this);
    m_doc->setDocumentMargin(0);
    m_doc->setUndoRedoEnabled(false);
    m_doc->setDefaultFont(m_ctx->theme().body);
}

PageSession::~PageSession()
{
    emit closing();
    m_tracker.detached = true; // block deletions during teardown are not edits
    delete m_doc;
    // The undo stack is gone: what only it referenced may now be collected.
    if (m_ctx->attachments)
        for (const QString &h : std::as_const(m_pinned))
            m_ctx->attachments->unpin(h);
}

LineRole PageSession::roleOf(const QTextBlock &b)
{
    const int s = b.userState();
    return s < 0 ? LineRole::Para : LineRole(s);
}

BlockData *PageSession::dataOf(const QTextBlock &b)
{
    return static_cast<BlockData *>(b.userData());
}

QTextBlock PageSession::segmentHead(QTextBlock b)
{
    while (b.isValid()) {
        const LineRole r = roleOf(b);
        if (r != LineRole::CodeLine && r != LineRole::CodeClose && r != LineRole::MathLine
            && r != LineRole::MathClose)
            break;
        QTextBlock p = b.previous();
        if (!p.isValid())
            break;
        b = p;
    }
    return b;
}

QTextCharFormat PageSession::imageFormat(const QString &hash, const QString &alt)
{
    QTextCharFormat f;
    f.setObjectType(kImageObjectType);
    f.setProperty(kAttachmentHashProperty, hash);
    f.setProperty(kImageAltProperty, alt);
    return f;
}

QString PageSession::imageHash(const QTextBlock &b)
{
    return b.begin().fragment().charFormat().property(kAttachmentHashProperty).toString();
}

QTextCharFormat PageSession::pageLinkFormat(PageId target)
{
    QTextCharFormat f;
    f.setObjectType(kPageLinkObjectType);
    f.setProperty(kPageLinkTargetProperty, qint64(target));
    return f;
}

LineRole PageSession::classify(const QTextBlock &b, int carry) const
{
    const QString t = b.text();
    if (carry == InCode)
        return t == kFence ? LineRole::CodeClose : LineRole::CodeLine;
    if (carry == InMath)
        return t == kMath ? LineRole::MathClose : LineRole::MathLine;
    if (t.startsWith(kFence))
        return LineRole::CodeOpen;
    if (t == kMath)
        return LineRole::MathOpen;
    if (t.size() >= 4 && t.startsWith(kMath) && t.endsWith(kMath))
        return LineRole::MathSingle;
    if (t.size() == 1 && t[0] == kObj) {
        const QTextFragment f = b.begin().fragment();
        if (f.isValid()) {
            const int ot = f.charFormat().objectType();
            if (ot == kPageLinkObjectType)
                return LineRole::PageLink;
            if (ot == kImageObjectType)
                return LineRole::Image;
        }
    }
    return LineRole::Para;
}

BlockData *PageSession::ensureData(QTextBlock &b)
{
    BlockData *d = dataOf(b);
    if (!d) {
        d = new BlockData(m_alloc(), &m_tracker);
        b.setUserData(d);
        m_tracker.structureChanged = true;
    }
    return d;
}

void PageSession::attachLayout()
{
    m_layout = new VirtualLayout(m_doc, m_ctx);
    m_doc->setDocumentLayout(m_layout);
    m_lastDocRevision = m_doc->revision();
    connect(m_doc, &QTextDocument::contentsChange, this, &PageSession::onContentsChange);
}

void PageSession::initNew(PageId parent, const QString &title)
{
    m_parent = parent;
    m_title = title;
    m_rev = m_snapshotRev = m_committedRev = 0;
    QTextBlock b = m_doc->firstBlock();
    b.setUserData(new BlockData(m_alloc(), &m_tracker));
    b.setUserState(int(LineRole::Para));
    m_tracker.structureChanged = true; // first save writes the order
    m_doc->setUndoRedoEnabled(true);
    attachLayout();
}

void PageSession::load(const PageContent &pc)
{
    instr::Scope scope("session_build_us");
    m_building = true;
    m_parent = pc.parent;
    m_title = pc.title;
    m_rev = m_snapshotRev = m_committedRev = pc.rev;

    // 1. Flatten records into lines, remembering the id of each line.
    std::vector<BlockId> ids;
    QStringList lines;
    // Object lines (child-page links, images) need their char format.
    struct ObjLine { int line; QTextCharFormat format; };
    std::vector<ObjLine> objects;
    // Named web links: lines are stored as Markdown `[label](url)`; the
    // document holds the label with an anchor format.
    struct WebLinks { int line; QVector<links::Span> spans; };
    std::vector<WebLinks> webLinks;
    ids.reserve(size_t(pc.blocks.size()) + 16);
    m_savedOrder.reserve(pc.blocks.size());
    for (const BlockRecord &r : pc.blocks) {
        m_savedOrder.push_back(r.id);
        m_savedIds.insert(r.id);
        switch (r.kind) {
        case BlockKind::Text: {
            QString hash, alt;
            if (attachments::parseImageMarkdown(r.text, &hash, &alt)) {
                objects.push_back({int(lines.size()), imageFormat(hash, alt)});
                lines.push_back(QString(kObj));
                ids.push_back(r.id);
                if (m_ctx->attachments && !m_pinned.contains(hash)) {
                    m_ctx->attachments->pin(hash);
                    m_pinned.insert(hash);
                }
                break;
            }
            const QStringList parts = r.text.split(QLatin1Char('\n'));
            for (int i = 0; i < parts.size(); ++i) {
                QVector<links::Span> spans;
                QString shown = links::parseMarkdown(parts[i], &spans);
                if (!spans.isEmpty())
                    webLinks.push_back({int(lines.size()), std::move(spans)});
                lines.push_back(std::move(shown));
                ids.push_back(i == 0 ? r.id : m_alloc());
            }
            break;
        }
        case BlockKind::Code: {
            lines.push_back(kFence + r.lang);
            ids.push_back(r.id);
            if (!r.text.isEmpty() || true) {
                const QStringList parts = r.text.split(QLatin1Char('\n'));
                for (const QString &p : parts) {
                    lines.push_back(p);
                    ids.push_back(m_alloc());
                }
            }
            lines.push_back(kFence);
            ids.push_back(m_alloc());
            break;
        }
        case BlockKind::Math:
            if (r.lang == QLatin1String("single")) {
                lines.push_back(kMath + r.text + kMath);
                ids.push_back(r.id);
            } else {
                lines.push_back(kMath);
                ids.push_back(r.id);
                for (const QString &p : r.text.split(QLatin1Char('\n'))) {
                    lines.push_back(p);
                    ids.push_back(m_alloc());
                }
                lines.push_back(kMath);
                ids.push_back(m_alloc());
            }
            break;
        case BlockKind::PageLink:
            objects.push_back({int(lines.size()), pageLinkFormat(r.target)});
            lines.push_back(QString(kObj));
            ids.push_back(r.id);
            break;
        }
    }
    if (lines.isEmpty()) {
        lines.push_back(QString());
        ids.push_back(m_alloc());
    }

    // 2. Insert text in as few calls as possible (links need their format).
    QTextCursor c(m_doc);
    c.beginEditBlock();
    int next = 0;
    auto flushTo = [&](int end) { // lines [next, end)
        if (end <= next)
            return;
        QString chunk;
        qsizetype total = 0;
        for (int i = next; i < end; ++i)
            total += lines[i].size() + 1;
        chunk.reserve(total);
        for (int i = next; i < end; ++i) {
            if (i > 0)
                chunk += QLatin1Char('\n');
            chunk += lines[i];
        }
        c.insertText(chunk, QTextCharFormat()); // never the previous object line's format
        next = end;
    };
    for (const ObjLine &l : objects) {
        flushTo(l.line);
        if (l.line > 0)
            c.insertText(QStringLiteral("\n"), QTextCharFormat());
        c.insertText(QString(kObj), l.format);
        next = l.line + 1;
    }
    flushTo(int(lines.size()));
    for (const WebLinks &w : webLinks) {
        const QTextBlock lb = m_doc->findBlockByNumber(w.line);
        for (const links::Span &sp : w.spans) {
            c.setPosition(lb.position() + sp.start);
            c.setPosition(lb.position() + sp.start + sp.length, QTextCursor::KeepAnchor);
            c.mergeCharFormat(links::anchorFormat(sp.href));
        }
    }
    c.endEditBlock();

    // 3. Attach identities and derive roles.
    QTextBlock b = m_doc->firstBlock();
    int carry = Normal;
    QHash<BlockId, int> headPos;
    for (size_t i = 0; b.isValid(); b = b.next(), ++i) {
        const BlockId id = i < ids.size() ? ids[i] : m_alloc();
        auto *d = new BlockData(id, &m_tracker);
        d->dirty = false;
        b.setUserData(d);
        const LineRole r = classify(b, carry);
        b.setUserState(int(r));
        carry = carryOf(r);
        if (r != LineRole::CodeLine && r != LineRole::CodeClose && r != LineRole::MathLine
            && r != LineRole::MathClose)
            headPos.insert(id, b.position());
    }
    m_tracker.structureChanged = false;

    // 4. Restore the persisted selection.
    auto posFor = [&](BlockId id, int off) {
        auto it = headPos.find(id);
        if (it == headPos.end())
            return 0;
        return qBound(0, *it + off, m_doc->characterCount() - 1);
    };
    m_anchor = posFor(pc.selection.anchorBlock, pc.selection.anchorOffset);
    m_position = posFor(pc.selection.posBlock, pc.selection.posOffset);
    m_scrollY = pc.selection.scrollY;

    m_doc->setUndoRedoEnabled(true);
    m_building = false;
    attachLayout();
}

void PageSession::onContentsChange(int pos, int removed, int added)
{
    if (m_building || m_presentationOnly)
        return;
    const int docRev = m_doc->revision();
    if (docRev == m_lastDocRevision && removed == added)
        return; // IME preedit / layout-only notification, no text change
    m_lastDocRevision = docRev;
    QTextBlock first = m_doc->findBlock(pos);
    QTextBlock last = m_doc->findBlock(pos + added);
    if (!first.isValid())
        first = m_doc->lastBlock();
    if (!last.isValid())
        last = m_doc->lastBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        BlockData *d = ensureData(b);
        d->dirty = true;
        d->hlValid = false;
        if (b == last)
            break;
    }
    propagateRoles(first, last);
    pinImages(first, last);
    ++m_rev;
    emit revisionChanged(m_rev);
}

void PageSession::pinImages(QTextBlock first, QTextBlock last)
{
    if (!m_ctx->attachments)
        return;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        if (roleOf(b) == LineRole::Image) {
            const QString h = imageHash(b);
            if (!h.isEmpty() && !m_pinned.contains(h)) {
                m_ctx->attachments->pin(h);
                m_pinned.insert(h);
            }
        }
        if (b == last)
            break;
    }
}

void PageSession::propagateRoles(QTextBlock first, QTextBlock last)
{
    QTextBlock prev = first.previous();
    int carry = prev.isValid() ? carryOf(roleOf(prev)) : Normal;
    bool pastLast = false;
    int dirtyFrom = -1, dirtyTo = -1;
    bool touchesMath = false;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        const LineRole old = LineRole(b.userState());
        const LineRole nr = classify(b, carry);
        if (roleIsMathSegment(old) || roleIsMathSegment(nr))
            touchesMath = !pastLast || touchesMath;
        if (nr != old) {
            b.setUserState(int(nr));
            BlockData *d = ensureData(b);
            d->dirty = true;
            d->hlValid = false;
            if (pastLast) {
                if (dirtyFrom < 0)
                    dirtyFrom = b.position();
                dirtyTo = b.position() + b.length();
            }
        } else if (pastLast) {
            // Code lines after an edit may need new highlight carry states,
            // which the layout propagates itself (until stable).
            break;
        }
        carry = carryOf(nr);
        if (b == last)
            pastLast = true;
    }
    // A display formula's preview lives on its closing line: re-render it when
    // any line of the formula changed.
    if (touchesMath) {
        for (QTextBlock b = last; b.isValid(); b = b.next()) {
            const LineRole r = roleOf(b);
            if (r == LineRole::MathClose || r == LineRole::MathSingle) {
                if (dirtyFrom < 0 || b.position() < dirtyFrom)
                    dirtyFrom = qMin(dirtyFrom < 0 ? b.position() : dirtyFrom, b.position());
                dirtyTo = qMax(dirtyTo, b.position() + b.length());
                break;
            }
            if (!roleInMath(r))
                break;
        }
    }
    if (dirtyFrom >= 0 && dirtyTo > dirtyFrom)
        m_doc->markContentsDirty(dirtyFrom, qMin(dirtyTo, m_doc->characterCount()) - dirtyFrom);
}

QTextBlock PageSession::findBlockById(BlockId id) const
{
    for (QTextBlock b = m_doc->firstBlock(); b.isValid(); b = b.next())
        if (BlockData *d = dataOf(b); d && d->id == id)
            return b;
    return QTextBlock();
}

void PageSession::setTitle(const QString &title)
{
    if (title == m_title)
        return;
    m_title = title;
    m_titleDirty = true;
    ++m_rev;
    emit titleChanged(m_title);
    emit revisionChanged(m_rev);
}

void PageSession::setCommitted(Revision rev)
{
    if (rev <= m_committedRev)
        return;
    m_committedRev = rev;
    emit committedChanged(rev);
}

bool PageSession::hasUnsnapshotted() const
{
    return m_rev != m_snapshotRev || m_titleDirty || m_viewDirty || m_tracker.structureChanged;
}

void PageSession::setViewState(int anchor, int position, int scrollY)
{
    if (anchor == m_anchor && position == m_position && scrollY == m_scrollY)
        return;
    m_anchor = anchor;
    m_position = position;
    m_scrollY = scrollY;
    m_viewDirty = true;
}

SelectionAnchor PageSession::anchorsFor(int anchor, int position, int scrollY) const
{
    SelectionAnchor s;
    auto one = [this](int pos, BlockId *id, qint32 *off) {
        pos = qBound(0, pos, m_doc->characterCount() - 1);
        QTextBlock h = segmentHead(m_doc->findBlock(pos));
        BlockData *d = dataOf(h);
        *id = d ? d->id : 0;
        *off = pos - h.position();
    };
    one(anchor, &s.anchorBlock, &s.anchorOffset);
    one(position, &s.posBlock, &s.posOffset);
    s.scrollY = scrollY;
    return s;
}

SaveBatch PageSession::takeSaveBatch()
{
    SaveBatch batch;
    if (!hasUnsnapshotted())
        return batch;
    instr::Scope scope("save_snapshot_us");
    instr::add(instr::SaveSnapshots);
    batch.page = m_id;
    batch.rev = qMax<Revision>(m_rev, 1);
    if (m_titleDirty || m_rev == 0 || m_committedRev == 0) {
        batch.hasTitle = true;
        batch.title = m_title;
    }
    if (m_viewDirty) {
        batch.hasSelection = true;
        batch.selection = anchorsFor(m_anchor, m_position, m_scrollY);
    }
    const bool contentChanged = m_rev != m_snapshotRev || m_tracker.structureChanged;
    if (contentChanged) {
        instr::add(instr::SaveSnapshotFullWalks);
        QVector<BlockId> order;
        order.reserve(m_savedOrder.size() + 8);
        qint64 bytes = 0;
        auto emitRec = [&](BlockRecord &&r) {
            bytes += (r.text.size() + r.lang.size()) * 2;
            batch.upserts.push_back(std::move(r));
        };
        QTextBlock b = m_doc->firstBlock();
        while (b.isValid()) {
            BlockData *d = ensureData(b);
            const LineRole role = roleOf(b);
            if (role == LineRole::CodeOpen || role == LineRole::MathOpen) {
                const bool code = role == LineRole::CodeOpen;
                const LineRole inner = code ? LineRole::CodeLine : LineRole::MathLine;
                const LineRole close = code ? LineRole::CodeClose : LineRole::MathClose;
                bool dirty = d->dirty;
                QTextBlock e = b.next();
                while (e.isValid() && roleOf(e) == inner) {
                    dirty = dirty || ensureData(e)->dirty;
                    e = e.next();
                }
                if (e.isValid() && roleOf(e) == close) {
                    dirty = dirty || ensureData(e)->dirty;
                    order.push_back(d->id);
                    if (dirty) {
                        BlockRecord r;
                        r.id = d->id;
                        r.kind = code ? BlockKind::Code : BlockKind::Math;
                        if (code)
                            r.lang = b.text().mid(3);
                        QString src;
                        for (QTextBlock x = b.next(); x != e; x = x.next()) {
                            if (x != b.next())
                                src += QLatin1Char('\n');
                            src += links::blockSource(x); // keeps a link's address if a linked line was fenced
                        }
                        r.text = std::move(src);
                        emitRec(std::move(r));
                    }
                    for (QTextBlock x = b; ; x = x.next()) {
                        dataOf(x)->dirty = false;
                        if (x == e)
                            break;
                    }
                    b = e.next();
                    continue;
                }
                // Unclosed fence: store the lines as text so they round-trip.
                for (QTextBlock x = b; x.isValid() && x != e; x = x.next()) {
                    BlockData *xd = ensureData(x);
                    order.push_back(xd->id);
                    if (xd->dirty || !m_savedIds.contains(xd->id)) {
                        BlockRecord r;
                        r.id = xd->id;
                        r.text = links::blockSource(x);
                        emitRec(std::move(r));
                    }
                    xd->dirty = false;
                }
                b = e;
                continue;
            }
            order.push_back(d->id);
            if (d->dirty || !m_savedIds.contains(d->id)) {
                BlockRecord r;
                r.id = d->id;
                if (role == LineRole::PageLink) {
                    r.kind = BlockKind::PageLink;
                    r.target = b.begin().fragment().charFormat().property(kPageLinkTargetProperty).toLongLong();
                } else if (role == LineRole::Image) {
                    const QTextCharFormat f = b.begin().fragment().charFormat();
                    r.kind = BlockKind::Text;
                    r.text = attachments::imageMarkdown(f.property(kAttachmentHashProperty).toString(),
                                                        f.property(kImageAltProperty).toString());
                } else if (role == LineRole::MathSingle) {
                    r.kind = BlockKind::Math;
                    r.lang = QStringLiteral("single");
                    const QString t = b.text();
                    r.text = t.mid(2, t.size() - 4);
                } else {
                    r.kind = BlockKind::Text;
                    r.text = links::blockSource(b); // named links -> [label](url)
                }
                emitRec(std::move(r));
            }
            d->dirty = false;
            b = b.next();
        }
        instr::add(instr::BytesCopiedForSave, bytes);
        if (m_tracker.structureChanged || order != m_savedOrder) {
            batch.hasOrder = true;
            QSet<BlockId> now(order.cbegin(), order.cend());
            for (BlockId id : std::as_const(m_savedIds))
                if (!now.contains(id))
                    batch.deletes.push_back(id);
            batch.order = order;
            m_savedOrder = order;
            m_savedIds = std::move(now);
        }
        m_tracker.structureChanged = false;
    }
    m_snapshotRev = m_rev;
    m_titleDirty = false;
    m_viewDirty = false;
    if (m_rev == 0) {
        // Never-edited new page: commit as revision 1 so "saved" is reachable.
        m_rev = 1;
        batch.rev = 1;
        m_snapshotRev = 1;
    }
    return batch;
}

Revision PageSession::applyEdit(const Edit &e)
{
    QTextBlock b = findBlockById(e.block);
    if (!b.isValid() && e.op != Edit::Op::InsertBlock)
        return 0;
    const Revision before = m_rev;
    QTextCursor c(m_doc);
    c.beginEditBlock();
    switch (e.op) {
    case Edit::Op::InsertText:
        c.setPosition(b.position() + qBound(0, e.offset, b.length() - 1));
        c.insertText(e.text);
        break;
    case Edit::Op::ReplaceBlockText: {
        c.setPosition(b.position());
        c.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
        QString hash, alt;
        if (attachments::parseImageMarkdown(e.text, &hash, &alt))
            c.insertText(QString(kObj), imageFormat(hash, alt));
        else
            c.insertText(e.text, QTextCharFormat()); // never inherit an object format
        break;
    }
    case Edit::Op::InsertBlock: {
        if (!b.isValid())
            b = m_doc->lastBlock();
        if (e.after) {
            c.setPosition(b.position() + b.length() - 1);
            c.insertText(QStringLiteral("\n"));
        } else {
            c.setPosition(b.position());
            c.insertText(QStringLiteral("\n"));
            c.movePosition(QTextCursor::PreviousCharacter);
        }
        const BlockRecord &r = e.record;
        switch (r.kind) {
        case BlockKind::Text: {
            QString hash, alt;
            if (attachments::parseImageMarkdown(r.text, &hash, &alt))
                c.insertText(QString(kObj), imageFormat(hash, alt));
            else
                c.insertText(r.text, QTextCharFormat());
            break;
        }
        case BlockKind::Code: c.insertText(kFence + r.lang + QLatin1Char('\n') + r.text + QLatin1Char('\n') + kFence); break;
        case BlockKind::Math: c.insertText(kMath + QLatin1Char('\n') + r.text + QLatin1Char('\n') + kMath); break;
        case BlockKind::PageLink: c.insertText(QString(kObj), pageLinkFormat(r.target)); break;
        }
        break;
    }
    case Edit::Op::RemoveBlock: {
        QTextBlock end = b;
        const LineRole r = roleOf(b);
        if (r == LineRole::CodeOpen || r == LineRole::MathOpen) {
            const LineRole close = r == LineRole::CodeOpen ? LineRole::CodeClose : LineRole::MathClose;
            for (QTextBlock x = b.next(); x.isValid(); x = x.next()) {
                end = x;
                if (roleOf(x) == close)
                    break;
            }
        }
        int from = b.position();
        int to = end.position() + end.length() - 1;
        if (end.next().isValid())
            to += 1; // eat the line break after
        else if (b.previous().isValid())
            from -= 1; // last line: eat the break before
        c.setPosition(from);
        c.setPosition(to, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        break;
    }
    }
    c.endEditBlock();
    return m_rev != before ? m_rev : m_rev;
}

int PageSession::beginImport(int position, int count)
{
    QTextBlock b = m_doc->findBlock(position);
    if (!b.isValid())
        b = m_doc->lastBlock();
    // Never split a code/math segment: images go after its closing line.
    const LineRole r = roleOf(b);
    if (roleIsCodeSegment(r) || roleIsMathSegment(r)) {
        for (QTextBlock x = b; x.isValid(); x = x.next()) {
            b = x;
            const LineRole xr = roleOf(x);
            if (xr == LineRole::CodeClose || xr == LineRole::MathClose)
                break;
        }
    }
    BlockData *d = ensureData(b);
    ++d->pendingImports;
    if (m_layout)
        m_layout->relayoutBlock(b);
    ImportBatch batch;
    batch.anchor = d->id;
    batch.remaining = qMax(1, count);
    batch.replaceEmpty = b.length() <= 1 && roleOf(b) == LineRole::Para;
    const int id = m_nextImport++;
    m_imports.insert(id, batch);
    return id;
}

void PageSession::finishImport(int id, bool ok, const AttachmentInfo &info, const QString &alt)
{
    auto it = m_imports.find(id);
    if (it == m_imports.end())
        return;
    ImportBatch &batch = *it;
    --batch.remaining;
    QTextBlock a = findBlockById(batch.anchor);
    if (a.isValid()) {
        BlockData *d = dataOf(a);
        if (d && d->pendingImports > 0)
            --d->pendingImports;
        if (m_layout)
            m_layout->relayoutBlock(a);
    }
    if (ok) {
        const QString md = attachments::imageMarkdown(info.hash, alt);
        Edit e;
        e.block = batch.anchor;
        e.text = md;
        e.record.kind = BlockKind::Text;
        e.record.text = md;
        QTextBlock inserted;
        if (a.isValid() && batch.replaceEmpty && a.length() <= 1 && roleOf(a) == LineRole::Para) {
            e.op = Edit::Op::ReplaceBlockText;
            applyEdit(e);
            inserted = findBlockById(batch.anchor);
        } else {
            e.op = Edit::Op::InsertBlock;
            e.after = true;
            applyEdit(e);
            inserted = a.isValid() ? a.next() : m_doc->lastBlock();
        }
        batch.replaceEmpty = false;
        if (inserted.isValid()) {
            batch.anchor = ensureData(inserted)->id; // keep the batch in order
            a = inserted;
        }
    }
    if (batch.remaining > 0) {
        if (a.isValid()) {
            ++ensureData(a)->pendingImports;
            if (m_layout)
                m_layout->relayoutBlock(a);
        }
    } else {
        m_imports.erase(it);
    }
}

QVector<PageId> PageSession::linkTargets() const
{
    QVector<PageId> out;
    for (QTextBlock b = m_doc->firstBlock(); b.isValid(); b = b.next())
        if (roleOf(b) == LineRole::PageLink)
            out.push_back(b.begin().fragment().charFormat().property(kPageLinkTargetProperty).toLongLong());
    return out;
}

void PageSession::appendPageLink(PageId target)
{
    QTextBlock last = m_doc->lastBlock();
    QTextCursor c(m_doc);
    c.beginEditBlock();
    if (last.length() <= 1 && last.previous().isValid() && roleOf(last) == LineRole::Para) {
        c.setPosition(last.position());
    } else {
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n"));
    }
    c.insertText(QString(kObj), pageLinkFormat(target));
    c.endEditBlock();
}

void PageSession::insertPageLinkAfter(int position, PageId target)
{
    QTextBlock b = m_doc->findBlock(position);
    if (!b.isValid())
        b = m_doc->lastBlock();
    // Never split a code/math segment: insert after its closing line.
    const LineRole r = roleOf(b);
    if (roleIsCodeSegment(r) || roleIsMathSegment(r)) {
        for (QTextBlock x = b; x.isValid(); x = x.next()) {
            b = x;
            const LineRole xr = roleOf(x);
            if (xr == LineRole::CodeClose || xr == LineRole::MathClose)
                break;
        }
    }
    QTextCursor c(m_doc);
    c.beginEditBlock();
    if (b.length() <= 1 && roleOf(b) == LineRole::Para) {
        c.setPosition(b.position());
    } else {
        c.setPosition(b.position() + b.length() - 1);
        c.insertText(QStringLiteral("\n"));
    }
    c.insertText(QString(kObj), pageLinkFormat(target));
    c.endEditBlock();
}

void PageSession::removePageLinks(PageId target)
{
    QVector<int> starts;
    for (QTextBlock b = m_doc->firstBlock(); b.isValid(); b = b.next())
        if (roleOf(b) == LineRole::PageLink
            && b.begin().fragment().charFormat().property(kPageLinkTargetProperty).toLongLong() == target)
            starts.push_back(b.position());
    if (starts.isEmpty())
        return;
    QTextCursor c(m_doc);
    c.beginEditBlock();
    for (int i = starts.size() - 1; i >= 0; --i) {
        QTextBlock b = m_doc->findBlock(starts[i]);
        int from = b.position();
        int to = b.position() + b.length() - 1;
        if (b.next().isValid())
            ++to;
        else if (b.previous().isValid())
            --from;
        c.setPosition(from);
        c.setPosition(to, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    c.endEditBlock();
}

void PageSession::refreshLinkTo(PageId target)
{
    if (!m_layout)
        return;
    for (QTextBlock b = m_doc->firstBlock(); b.isValid(); b = b.next())
        if (roleOf(b) == LineRole::PageLink
            && b.begin().fragment().charFormat().property(kPageLinkTargetProperty).toLongLong() == target) {
            b.layout()->clearLayout();
            m_layout->relayoutBlock(b);
        }
}

qint64 PageSession::estimatedBytes() const
{
    const qint64 chars = m_doc->characterCount();
    const qint64 blocks = m_doc->blockCount();
    return chars * 6 + blocks * 220 + (m_layout ? m_layout->estimatedLayoutBytes() : 0);
}

int PageSession::lineCount() const { return m_doc->blockCount(); }

} // namespace folia
