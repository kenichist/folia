#include "editor/NoteEditor.h"

#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/LinkPopups.h"
#include "editor/Links.h"
#include "editor/PageSession.h"
#include "editor/VirtualLayout.h"
#ifdef FOLIA_WITH_MATH
#include "math/MathRenderer.h"
#endif

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTimer>

namespace folia {

namespace {

LineRole roleAt(const QTextBlock &b) { return PageSession::roleOf(b); }

PageId linkTarget(const QTextBlock &b)
{
    return b.begin().fragment().charFormat().property(kPageLinkTargetProperty).toLongLong();
}

QString listMarker(const QString &t)
{
    if (t.size() >= 2 && (t[0] == QLatin1Char('-') || t[0] == QLatin1Char('*') || t[0] == QLatin1Char('+'))
        && t[1] == QLatin1Char(' '))
        return t.left(2);
    int i = 0;
    while (i < t.size() && i < 4 && t[i].isDigit())
        ++i;
    if (i > 0 && i + 1 < t.size() && t[i] == QLatin1Char('.') && t[i + 1] == QLatin1Char(' '))
        return t.left(i + 2);
    return QString();
}

QString leadingSpaces(const QString &t)
{
    int i = 0;
    while (i < t.size() && (t[i] == QLatin1Char(' ') || t[i] == QLatin1Char('\t')))
        ++i;
    return t.left(i);
}

bool hasLaterClose(QTextBlock b, LineRole close)
{
    for (b = b.next(); b.isValid(); b = b.next()) {
        const LineRole r = roleAt(b);
        if (r == close)
            return true;
        if (!roleInCode(r) && !roleInMath(r))
            return false;
    }
    return false;
}

} // namespace

NoteEditor::NoteEditor(EditorContext *ctx, QWidget *parent) : QTextEdit(parent), m_ctx(ctx)
{
    setAcceptRichText(false);
    setFrameStyle(QFrame::NoFrame);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setTabChangesFocus(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setObjectName(QStringLiteral("folia-editor"));
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::updateInlineMath);
    m_flashTime = QGuiApplication::styleHints()->cursorFlashTime();
    m_caretIdle.setSingleShot(true);
    m_caretIdle.setInterval(qEnvironmentVariableIntValue("FOLIA_CARET_IDLE_MS") > 0
                                ? qEnvironmentVariableIntValue("FOLIA_CARET_IDLE_MS") : 5000);
    connect(&m_caretIdle, &QTimer::timeout, this, [this] {
        if (QGuiApplication::styleHints()->cursorFlashTime() != 0)
            QGuiApplication::styleHints()->setCursorFlashTime(0); // steady caret
    });
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::caretActivity);
    // Link bubble: coalesced to the next event-loop turn so it never runs
    // (or lays out) in the middle of a document change.
    m_linkTimer.setSingleShot(true);
    m_linkTimer.setInterval(0);
    connect(&m_linkTimer, &QTimer::timeout, this, &NoteEditor::updateLinkBubble);
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::scheduleLinkPopups);
    connect(this, &QTextEdit::selectionChanged, this, &NoteEditor::scheduleLinkPopups);
    caretActivity();
    applyTheme();
}

void NoteEditor::caretActivity()
{
    if (m_flashTime > 0 && QGuiApplication::styleHints()->cursorFlashTime() != m_flashTime)
        QGuiApplication::styleHints()->setCursorFlashTime(m_flashTime);
    m_caretIdle.start();
}

NoteEditor::~NoteEditor() = default;

void NoteEditor::applyTheme()
{
    const EditorTheme &t = m_ctx->theme();
    QPalette p = palette();
    p.setColor(QPalette::Base, t.background);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.text);
    setPalette(p);
    viewport()->setPalette(p);
    viewport()->setAutoFillBackground(true);
    if (m_bubble)
        m_bubble->applyTheme(t);
    if (m_linkEditor)
        m_linkEditor->applyTheme(t);
}

void NoteEditor::storeViewState()
{
    if (!m_session)
        return;
    const QTextCursor c = textCursor();
    m_session->setViewState(c.anchor(), c.position(), verticalScrollBar()->value());
}

void NoteEditor::detachSession()
{
    closeLinkEditor();
    if (m_bubble)
        m_bubble->hide();
    storeViewState();
    if (m_session)
        disconnect(m_session, &PageSession::closing, this, nullptr);
    m_session = nullptr;
    static QTextDocument *blank = nullptr;
    if (!blank)
        blank = new QTextDocument(qApp);
    setDocument(blank);
    setReadOnly(true);
}

void NoteEditor::setSession(PageSession *s)
{
    if (m_session == s)
        return;
    storeViewState();
    if (m_session)
        disconnect(m_session, &PageSession::closing, this, nullptr);
    m_session = s;
    if (!s) {
        detachSession();
        return;
    }
    setReadOnly(false);
    closeLinkEditor();
    if (m_bubble)
        m_bubble->hide();
    // QTextEdit does not own the document: never outlive it.
    connect(s, &PageSession::closing, this, [this, s] {
        if (m_session == s)
            detachSession();
    }, Qt::DirectConnection);
    m_inlineStart = m_inlineEnd = -1;
    m_inlineList.reset();
    setDocument(s->document());
    QTextCursor c(s->document());
    const int maxPos = s->document()->characterCount() - 1;
    c.setPosition(qBound(0, s->viewAnchor(), maxPos));
    c.setPosition(qBound(0, s->viewPosition(), maxPos), QTextCursor::KeepAnchor);
    setTextCursor(c);
    const int y = s->viewScrollY();
    verticalScrollBar()->setValue(y);
    if (y == 0 && (s->viewPosition() > 0))
        ensureCursorVisible();
    updateLayoutViewport();
}

bool NoteEditor::event(QEvent *e)
{
    const QEvent::Type t = e->type();
    if (t == QEvent::FocusIn || t == QEvent::MouseButtonPress)
        caretActivity();
    if (t == QEvent::FocusIn || t == QEvent::FocusOut)
        scheduleLinkPopups();
    if (t != QEvent::KeyPress && t != QEvent::InputMethod)
        return QTextEdit::event(e);
    caretActivity();
    if (!m_pendingInput) {
        m_inputTimer.start();
        m_pendingInput = true;
    }
    QElapsedTimer w;
    w.start();
    const bool r = QTextEdit::event(e);
    instr::record("edit_main_thread_us", w.nsecsElapsed() / 1000);
    return r;
}

bool NoteEditor::handleSmartKeys(QKeyEvent *e)
{
    if (!m_session || isReadOnly())
        return false;
    QTextCursor c = textCursor();
    QTextBlock b = c.block();
    const LineRole role = roleAt(b);
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const bool plain = mods == Qt::NoModifier;
    const int key = e->key();

    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (mods & Qt::ControlModifier)) {
        if (role == LineRole::PageLink) {
            emit openPageRequested(linkTarget(b));
            return true;
        }
        return false;
    }
    if ((key == Qt::Key_C) && mods == (Qt::ControlModifier | Qt::ShiftModifier))
        return copyCodeBlockAtCursor();

    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && plain && !c.hasSelection()) {
        if (role == LineRole::PageLink) {
            c.movePosition(QTextCursor::EndOfBlock);
            c.insertText(QStringLiteral("\n"));
            setTextCursor(c);
            return true;
        }
        if ((role == LineRole::CodeOpen || role == LineRole::MathOpen) && c.atBlockEnd()) {
            const bool code = role == LineRole::CodeOpen;
            if (!hasLaterClose(b, code ? LineRole::CodeClose : LineRole::MathClose)) {
                c.beginEditBlock();
                c.insertText(code ? QStringLiteral("\n\n```") : QStringLiteral("\n\n$$"));
                c.endEditBlock();
                c.movePosition(QTextCursor::PreviousBlock);
                c.movePosition(QTextCursor::EndOfBlock);
                setTextCursor(c);
                return true;
            }
        }
        if (role == LineRole::CodeLine) {
            const QString ind = leadingSpaces(b.text().left(c.positionInBlock()));
            if (!ind.isEmpty()) {
                c.insertText(QLatin1Char('\n') + ind);
                setTextCursor(c);
                ensureCursorVisible();
                return true;
            }
        }
        if (role == LineRole::Para && c.atBlockEnd()) {
            const QString text = b.text();
            const QString ind = leadingSpaces(text);
            const QString marker = listMarker(text.mid(ind.size()));
            if (!marker.isEmpty()) {
                if (text.size() == ind.size() + marker.size()) {
                    // Empty item: end the list.
                    c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
                    c.removeSelectedText();
                    setTextCursor(c);
                    return true;
                }
                QString next = marker;
                if (marker[0].isDigit())
                    next = QString::number(marker.left(marker.size() - 2).toInt() + 1) + QStringLiteral(". ");
                c.insertText(QLatin1Char('\n') + ind + next);
                setTextCursor(c);
                ensureCursorVisible();
                return true;
            }
        }
        return false;
    }
    if (key == Qt::Key_Tab && plain) {
        if (roleIsCodeSegment(role) || roleIsMathSegment(role)) {
            c.insertText(QStringLiteral("    "));
            setTextCursor(c);
            return true;
        }
        if (role == LineRole::Para && !listMarker(b.text().trimmed() + QLatin1Char(' ')).isEmpty()) {
            QTextCursor s(b);
            s.insertText(QStringLiteral("  "));
            return true;
        }
        return false;
    }
    if (key == Qt::Key_Backtab) {
        const QString text = b.text();
        int n = 0;
        while (n < 4 && n < text.size() && text[n] == QLatin1Char(' '))
            ++n;
        if (n > 0) {
            QTextCursor s(b);
            s.setPosition(b.position() + n, QTextCursor::KeepAnchor);
            s.removeSelectedText();
        }
        return true;
    }
    if (key == Qt::Key_Backspace && plain && !c.hasSelection() && c.atBlockStart()) {
        QTextBlock prev = b.previous();
        if (prev.isValid() && roleAt(prev) == LineRole::PageLink) {
            QTextCursor d(document());
            d.setPosition(prev.position());
            d.setPosition(b.position(), QTextCursor::KeepAnchor);
            d.removeSelectedText(); // remove the whole link line, keep this one
            return true;
        }
        if (role == LineRole::PageLink && prev.isValid()) {
            c.setPosition(prev.position() + prev.length() - 1);
            setTextCursor(c);
            return true;
        }
    }
    if (key == Qt::Key_Delete && plain && !c.hasSelection() && c.atBlockEnd()) {
        QTextBlock next = b.next();
        if (next.isValid() && roleAt(next) == LineRole::PageLink) {
            QTextCursor d(document());
            d.setPosition(b.position() + b.length() - 1);
            d.setPosition(next.position() + next.length() - 1, QTextCursor::KeepAnchor);
            d.removeSelectedText();
            return true;
        }
    }
    if (key == Qt::Key_Up && plain && !b.previous().isValid()) {
        QTextLayout *tl = b.layout();
        if (!tl || tl->lineCount() == 0 || tl->lineForTextPosition(c.positionInBlock()).lineNumber() == 0) {
            emit escapePressed(); // first line: move focus to the title
            return true;
        }
    }
    if (role == LineRole::PageLink && !(mods & Qt::ControlModifier) && !e->text().isEmpty()
        && e->text().at(0).isPrint()) {
        // Typing on a link line starts a new paragraph after it.
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertText(QStringLiteral("\n"));
        setTextCursor(c);
        return false;
    }
    return false;
}

void NoteEditor::keyPressEvent(QKeyEvent *e)
{
    const bool inserts = !e->text().isEmpty() && !(e->modifiers() & Qt::ControlModifier);
    if (inserts || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        preventLinkGrowth();
    if (handleSmartKeys(e)) {
        e->accept();
        ensureCursorVisible();
        return;
    }
    QTextEdit::keyPressEvent(e);
    if (e->text() == QLatin1String(")"))
        autoLinkTypedMarkdown();
}

void NoteEditor::inputMethodEvent(QInputMethodEvent *e)
{
    // A preedit-only update (no commit, no replacement, nothing selected to
    // replace) changes presentation, not the page: keep the revision.
    const bool presentationOnly = e->commitString().isEmpty() && e->replacementLength() == 0
        && !textCursor().hasSelection();
    if (m_session)
        m_session->setPresentationOnly(presentationOnly);
    if (!e->commitString().isEmpty())
        preventLinkGrowth();
    QTextEdit::inputMethodEvent(e);
    if (m_session)
        m_session->setPresentationOnly(false);
    const bool p = !e->preeditString().isEmpty();
    if (p != m_preedit) {
        m_preedit = p;
        emit preeditChanged(p);
    }
}

bool NoteEditor::copyCodeBlockAtCursor()
{
    QTextBlock b = textCursor().block();
    if (!roleIsCodeSegment(roleAt(b)))
        return false;
    b = PageSession::segmentHead(b);
    QStringList lines;
    for (QTextBlock x = b.next(); x.isValid() && roleAt(x) == LineRole::CodeLine; x = x.next())
        lines << x.text();
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    return true;
}

void NoteEditor::mouseReleaseEvent(QMouseEvent *e)
{
    QTextEdit::mouseReleaseEvent(e);
    if (e->button() != Qt::LeftButton || !m_session || textCursor().hasSelection())
        return;
    const QPointF docPt = e->position() + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    if (e->modifiers() & Qt::ControlModifier) { // Cmd-click (Ctrl-click elsewhere) opens a web link
        const LinkHit h = linkUnderPoint(docPt);
        if (h.valid()) {
            emit openUrlRequested(h.href);
            return;
        }
    }
    VirtualLayout *vl = m_session->layout();
    const int pos = vl->hitTest(docPt, Qt::FuzzyHit);
    if (pos < 0)
        return;
    QTextBlock b = document()->findBlock(pos);
    const LineRole r = roleAt(b);
    if (r == LineRole::PageLink) {
        QTextLayout *tl = b.layout();
        if (tl && tl->lineCount()) {
            const QRectF br = vl->blockBoundingRect(b);
            const QTextLine l = tl->lineAt(0);
            const QRectF chip(l.x(), br.top() + l.y(), l.naturalTextWidth(), l.height());
            if (chip.contains(docPt))
                emit openPageRequested(linkTarget(b));
        }
    } else if (r == LineRole::CodeOpen) {
        if (vl->codeCopyRect(b).contains(docPt)) {
            QTextCursor c(b);
            c.movePosition(QTextCursor::NextBlock);
            setTextCursor(c);
            copyCodeBlockAtCursor();
        }
    }
}

void NoteEditor::mouseMoveEvent(QMouseEvent *e)
{
    QTextEdit::mouseMoveEvent(e);
    if (e->buttons() != Qt::NoButton || !m_session)
        return;
    const QPointF docPt = e->position() + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    VirtualLayout *vl = m_session->layout();
    const int pos = vl->hitTest(docPt, Qt::FuzzyHit);
    Qt::CursorShape shape = Qt::IBeamCursor;
    if (pos >= 0) {
        QTextBlock b = document()->findBlock(pos);
        const LineRole r = roleAt(b);
        if (r == LineRole::PageLink) {
            QTextLayout *tl = b.layout();
            if (tl && tl->lineCount()) {
                const QRectF br = vl->blockBoundingRect(b);
                const QTextLine l = tl->lineAt(0);
                if (QRectF(l.x(), br.top() + l.y(), l.naturalTextWidth(), l.height()).contains(docPt))
                    shape = Qt::PointingHandCursor;
            }
        } else if (r == LineRole::CodeOpen && vl->codeCopyRect(b).contains(docPt)) {
            shape = Qt::PointingHandCursor;
        } else if ((e->modifiers() & Qt::ControlModifier) && linkUnderPoint(docPt).valid()) {
            shape = Qt::PointingHandCursor;
        }
    }
    if (viewport()->cursor().shape() != shape)
        viewport()->setCursor(shape);
}

void NoteEditor::paintEvent(QPaintEvent *e)
{
    instr::Scope paintScope("editor_paint_us");
    QTextEdit::paintEvent(e);
#ifdef FOLIA_WITH_MATH
    if (m_inlineStart >= 0 && (m_inlineList || !m_inlineError.isEmpty()) && hasFocus()) {
        const EditorTheme &t = m_ctx->theme();
        QTextCursor c(document());
        c.setPosition(qMin(m_inlineStart, document()->characterCount() - 1));
        const QRect cr = cursorRect(c);
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing, true);
        QSizeF sz;
        if (m_inlineList)
            sz = QSizeF(m_inlineList->width + 20, m_inlineList->height() + 14);
        else
            sz = QSizeF(QFontMetricsF(font()).horizontalAdvance(m_inlineError) + 24, 26);
        qreal y = cr.top() - sz.height() - 6;
        if (y < 2)
            y = cr.bottom() + 6;
        const QRectF box(qMax<qreal>(4, cr.left() - 10), y, sz.width(), sz.height());
        QColor bg = t.background;
        bg = bg.lightness() > 128 ? bg.darker(103) : bg.lighter(130);
        p.setPen(QPen(t.dim, 0.6));
        p.setBrush(bg);
        p.drawRoundedRect(box, 7, 7);
        if (m_inlineList) {
            m_inlineList->paint(&p, QPointF(box.left() + 10, box.top() + 7 + m_inlineList->ascent), t.text);
        } else {
            p.setPen(t.mathError);
            p.drawText(box, Qt::AlignCenter, m_inlineError);
        }
    }
#endif
    emit painted();
    if (m_pendingInput) {
        m_pendingInput = false;
        const qint64 us = m_inputTimer.nsecsElapsed() / 1000;
        instr::record("input_to_paint_us", us);
        if (instr::samplingEnabled()) {
            QElapsedTimer t = m_inputTimer;
            // Fires after this paint's backing-store flush completes.
            QTimer::singleShot(0, [t] { instr::record("input_to_flush_us", t.nsecsElapsed() / 1000); });
        }
    }
}

void NoteEditor::resizeEvent(QResizeEvent *e)
{
    QTextEdit::resizeEvent(e);
    updateLayoutViewport();
    scheduleLinkPopups();
}

void NoteEditor::scrollContentsBy(int dx, int dy)
{
    QTextEdit::scrollContentsBy(dx, dy);
    updateLayoutViewport();
    if ((m_bubble && m_bubble->isVisible()) || isEditingLink())
        scheduleLinkPopups();
}

void NoteEditor::updateLayoutViewport()
{
    if (!m_session || !m_session->layout())
        return;
    const int y = verticalScrollBar()->value();
    m_session->layout()->setViewport(y, y + viewport()->height());
}

void NoteEditor::updateInlineMath()
{
#ifdef FOLIA_WITH_MATH
    const int oldStart = m_inlineStart;
    auto clear = [&] {
        if (m_inlineStart >= 0) {
            m_inlineStart = m_inlineEnd = -1;
            m_inlineHash = 0;
            m_inlineList.reset();
            m_inlineError.clear();
            viewport()->update();
        }
    };
    if (!m_session || !m_ctx->previews) {
        clear();
        return;
    }
    const QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    if (c.hasSelection() || roleAt(b) != LineRole::Para) {
        clear();
        return;
    }
    const QString text = b.text();
    const int p = c.positionInBlock();
    int s = -1, e = -1, idx = 0, spanNo = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (text[i] == QLatin1Char('`')) {
            const int close = text.indexOf(QLatin1Char('`'), i + 1);
            if (close > i) {
                i = close;
                continue;
            }
        }
        if (text[i] != QLatin1Char('$') || (i + 1 < text.size() && text[i + 1] == QLatin1Char('$')))
            continue;
        int j = i + 1;
        while (j < text.size() && !(text[j] == QLatin1Char('$') && text[j - 1] != QLatin1Char('\\')))
            ++j;
        if (j >= text.size())
            break;
        ++spanNo;
        if (p > i && p <= j) {
            s = i;
            e = j;
            idx = spanNo;
            break;
        }
        i = j;
    }
    if (s < 0) {
        clear();
        return;
    }
    const QString src = text.mid(s + 1, e - s - 1);
    const quint64 hash = quint64(qHash(src)) ^ 0x1234567ull;
    const int absStart = b.position() + s;
    if (hash == m_inlineHash && absStart == oldStart)
        return;
    m_inlineStart = absStart;
    m_inlineEnd = b.position() + e + 1;
    m_inlineHash = hash;
    m_inlineList.reset();
    m_inlineError.clear();
    PreviewRequest req;
    BlockData *d = PageSession::dataOf(b);
    req.block = (d ? d->id : 0) * 64 + idx + (qint64(1) << 60); // distinct from display blocks
    req.blockRev = hash;
    req.kind = PreviewKind::InlineMath;
    req.source = src;
    req.pointSize = m_ctx->theme().body.pointSizeF();
    req.color = m_ctx->theme().text;
    QPointer<NoteEditor> self(this);
    m_ctx->previews->renderPreview(req, this, [self, hash](PreviewResult r) {
        if (!self || self->m_inlineHash != hash)
            return; // stale: the user has moved on
        self->m_inlineList = r.ok ? r.list : nullptr;
        self->m_inlineError = r.ok ? QString() : r.error;
        self->viewport()->update();
    });
    viewport()->update();
#endif
}

QString NoteEditor::plainTextForRange(int from, int to) const
{
    QStringList out;
    QTextBlock b = document()->findBlock(from);
    for (; b.isValid() && b.position() <= to; b = b.next()) {
        const int bs = b.position();
        const int be = bs + b.length() - 1;
        const int s = qMax(from, bs) - bs;
        const int e = qMin(to, be) - bs;
        if (roleAt(b) == LineRole::PageLink) {
            const QString title = m_ctx->titleOf ? m_ctx->titleOf(linkTarget(b)) : QString();
            out << QStringLiteral("[[%1]]").arg(title.isEmpty() ? QStringLiteral("Untitled") : title);
        } else {
            QString t = links::blockSource(b, s, qMax(s, e)); // named links copy as [label](url)
            t.remove(QChar::ObjectReplacementCharacter);
            out << t;
        }
        if (be >= to)
            break;
    }
    return out.join(QLatin1Char('\n'));
}

QMimeData *NoteEditor::createMimeDataFromSelection() const
{
    const QTextCursor c = textCursor();
    auto *m = new QMimeData;
    m->setText(plainTextForRange(c.selectionStart(), c.selectionEnd()));
    return m;
}

bool NoteEditor::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasText();
}

void NoteEditor::insertFromMimeData(const QMimeData *source)
{
    QString t = source->text();
    t.remove(QChar::ObjectReplacementCharacter);
    t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QTextCursor c = textCursor();
    const QTextBlock first = document()->findBlock(c.selectionStart());
    const bool prose = roleAt(first) == LineRole::Para;
    // Pasting an address onto selected words links them (as in Google Docs).
    if (prose && c.hasSelection() && first == document()->findBlock(c.selectionEnd()) && links::looksLikeUrl(t)) {
        const int end = c.selectionEnd();
        c.setCharFormat(links::anchorFormat(links::normalizeUrl(t)));
        c.setPosition(end);
        setTextCursor(c);
        setCurrentCharFormat(QTextCharFormat());
        return;
    }
    if (!c.hasSelection())
        preventLinkGrowth();
    c = textCursor();
    // Text keeps the format it is typed into (plain unless inside a link);
    // never inherit a page-link object format.
    QTextCharFormat fmt = c.charFormat();
    if (!links::isLink(fmt) || c.hasSelection())
        fmt = QTextCharFormat();
    c.beginEditBlock();
    if (c.hasSelection())
        c.removeSelectedText();
    // Markdown links in pasted prose become real links (unless the paste
    // carries code or math fences, which stay verbatim).
    if (prose && t.contains(QLatin1String("](")) && !t.contains(QLatin1String("```")) && !t.contains(QLatin1String("$$"))) {
        const QStringList lines = t.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (i > 0)
                c.insertText(QStringLiteral("\n"), QTextCharFormat());
            QVector<links::Span> spans;
            const QString shown = links::parseMarkdown(lines[i], &spans);
            int at = 0;
            for (const links::Span &sp : std::as_const(spans)) {
                if (sp.start > at)
                    c.insertText(shown.mid(at, sp.start - at), i == 0 ? fmt : QTextCharFormat());
                c.insertText(shown.mid(sp.start, sp.length), links::anchorFormat(sp.href));
                at = sp.start + sp.length;
            }
            if (at < shown.size())
                c.insertText(shown.mid(at), (i == 0 && spans.isEmpty()) ? fmt : QTextCharFormat());
        }
    } else {
        c.insertText(t, fmt);
    }
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}

// =============================================================== links ====
NoteEditor::LinkHit NoteEditor::linkAt(int pos) const
{
    LinkHit h;
    QTextDocument *d = document();
    if (!m_session || pos < 0 || pos >= d->characterCount() - 1)
        return h;
    const QTextBlock b = d->findBlock(pos);
    if (!b.isValid() || pos >= b.position() + b.length() - 1 || roleAt(b) == LineRole::PageLink)
        return h;
    // Named link: the contiguous run of fragments with the same address.
    struct Frag { int s, e; QString href; };
    QVector<Frag> frags;
    int hit = -1;
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const QTextCharFormat cf = f.charFormat();
        frags.push_back({f.position(), f.position() + f.length(), links::isLink(cf) ? cf.anchorHref() : QString()});
        if (pos >= f.position() && pos < f.position() + f.length())
            hit = int(frags.size()) - 1;
    }
    if (hit >= 0 && !frags[hit].href.isEmpty()) {
        int a = hit, z = hit;
        while (a > 0 && frags[a - 1].href == frags[hit].href && frags[a - 1].e == frags[a].s)
            --a;
        while (z + 1 < frags.size() && frags[z + 1].href == frags[hit].href && frags[z + 1].s == frags[z].e)
            ++z;
        h.start = frags[a].s;
        h.end = frags[z].e;
        h.href = frags[hit].href;
        h.named = true;
        return h;
    }
    // Bare URL: recognized by the layout (which knows code spans and math).
    if (VirtualLayout *vl = m_session->layout())
        vl->blockBoundingRect(b); // lays the block out if needed
    if (QTextLayout *tl = b.layout()) {
        const int rel = pos - b.position();
        for (const QTextLayout::FormatRange &r : tl->formats()) {
            if (r.format.hasProperty(kLinkHrefProperty) && rel >= r.start && rel < r.start + r.length) {
                h.start = b.position() + r.start;
                h.end = h.start + r.length;
                h.href = r.format.property(kLinkHrefProperty).toString();
                return h;
            }
        }
    }
    return h;
}

NoteEditor::LinkHit NoteEditor::linkNearCaret() const
{
    const QTextCursor c = textCursor();
    if (c.hasSelection())
        return {};
    LinkHit h = linkAt(c.position());
    if (!h.valid() && !c.atBlockStart())
        h = linkAt(c.position() - 1);
    return h;
}

NoteEditor::LinkHit NoteEditor::linkUnderPoint(const QPointF &docPt) const
{
    if (!m_session || !m_session->layout())
        return {};
    VirtualLayout *vl = m_session->layout();
    const int pos = vl->hitTest(docPt, Qt::FuzzyHit);
    if (pos < 0)
        return {};
    const QTextBlock b = document()->findBlock(pos);
    const QRectF br = vl->blockBoundingRect(b);
    QTextLayout *tl = b.layout();
    if (!tl)
        return {};
    const qreal y = docPt.y() - br.top();
    for (int i = 0; i < tl->lineCount(); ++i) {
        const QTextLine l = tl->lineAt(i);
        if (y < l.y() || y >= l.y() + l.height())
            continue;
        if (docPt.x() < l.x() || docPt.x() > l.x() + l.naturalTextWidth())
            return {};
        const int idx = l.xToCursor(docPt.x(), QTextLine::CursorOnCharacter);
        return linkAt(b.position() + idx);
    }
    return {};
}

bool NoteEditor::isEditingLink() const { return m_linkEditor && m_linkEditor->isVisible(); }

void NoteEditor::scheduleLinkPopups() { m_linkTimer.start(); }

void NoteEditor::updateLinkBubble()
{
    if (isEditingLink()) {
        placePopup(m_linkEditor, m_editStart);
        if (m_bubble)
            m_bubble->hide();
        return;
    }
    LinkHit h;
    if (m_session && !isReadOnly() && hasFocus() && !m_preedit)
        h = linkNearCaret();
    if (!h.valid()) {
        if (m_bubble)
            m_bubble->hide();
        return;
    }
    if (!m_bubble) {
        m_bubble = new LinkBubble(this);
        m_bubble->applyTheme(m_ctx->theme());
        connect(m_bubble, &LinkBubble::openRequested, this, [this] {
            if (!m_bubble->href().isEmpty())
                emit openUrlRequested(m_bubble->href());
        });
        connect(m_bubble, &LinkBubble::copyRequested, this, [this] {
            QApplication::clipboard()->setText(m_bubble->href());
        });
        connect(m_bubble, &LinkBubble::editRequested, this, [this] { editLink(); });
        connect(m_bubble, &LinkBubble::removeRequested, this, &NoteEditor::removeLink);
    }
    m_bubble->setLink(h.href, h.named);
    placePopup(m_bubble, h.start);
    m_bubble->show();
    m_bubble->raise();
}

void NoteEditor::placePopup(QWidget *w, int start)
{
    if (!w)
        return;
    w->adjustSize();
    QTextDocument *d = document();
    const int maxPos = d->characterCount() - 1;
    QTextCursor sc(d);
    sc.setPosition(qBound(0, start, maxPos));
    const QRect sr = cursorRect(sc);
    const QRect cr = cursorRect(textCursor());
    // Below the caret's line, aligned with the link (or selection) start
    // when it is on that line.
    const bool sameLine = qAbs(sr.center().y() - cr.center().y()) < qMax(4, cr.height() / 2);
    const int x = (sameLine ? sr.left() : cr.left()) - 10;
    const QPoint below = viewport()->mapTo(this, QPoint(x, cr.bottom() + 6));
    const QPoint above = viewport()->mapTo(this, QPoint(x, (sameLine ? qMin(sr.top(), cr.top()) : cr.top()) - 6));
    int px = qBound(4, below.x(), qMax(4, width() - w->width() - 4));
    int py = below.y();
    if (py + w->height() > height() - 4 && above.y() - w->height() >= 4)
        py = above.y() - w->height();
    w->move(px, py);
}

bool NoteEditor::editLink(QString *why)
{
    auto refuse = [why](const QString &m) {
        if (why)
            *why = m;
        return false;
    };
    if (!m_session || isReadOnly())
        return refuse(tr("no page is open"));
    const QTextCursor c = textCursor();
    const QTextBlock b = document()->findBlock(c.selectionStart());
    if (roleAt(b) != LineRole::Para)
        return refuse(tr("links can be added to text, not inside code, math or a page link"));
    const int lineEnd = b.position() + b.length() - 1;
    QString text, href;
    if (c.hasSelection()) {
        m_editStart = c.selectionStart();
        m_editEnd = qMin(c.selectionEnd(), lineEnd); // one line at a time
        const LinkHit h = linkAt(m_editStart);
        if (h.valid() && h.named && m_editEnd <= h.end)
            href = h.href;
    } else {
        const LinkHit h = linkNearCaret();
        if (h.valid()) {
            m_editStart = h.start;
            m_editEnd = h.end;
            href = h.href;
        } else {
            m_editStart = m_editEnd = c.position();
        }
    }
    text = b.text().mid(m_editStart - b.position(), m_editEnd - m_editStart);
    if (!m_linkEditor) {
        m_linkEditor = new LinkEditor(this);
        m_linkEditor->applyTheme(m_ctx->theme());
        connect(m_linkEditor, &LinkEditor::applied, this, &NoteEditor::applyLink);
        connect(m_linkEditor, &LinkEditor::cancelled, this, &NoteEditor::closeLinkEditor);
    }
    if (m_bubble)
        m_bubble->hide();
    m_linkEditor->start(text, href);
    placePopup(m_linkEditor, m_editStart);
    return true;
}

void NoteEditor::closeLinkEditor()
{
    if (!isEditingLink())
        return;
    QWidget *fw = QApplication::focusWidget();
    const bool refocus = !fw || fw == this || m_linkEditor->isAncestorOf(fw);
    m_linkEditor->hide();
    if (refocus)
        setFocus(Qt::OtherFocusReason);
    scheduleLinkPopups();
}

void NoteEditor::applyLink(const QString &textIn, const QString &hrefIn)
{
    const QString href = links::normalizeUrl(hrefIn);
    closeLinkEditor();
    setFocus(Qt::OtherFocusReason);
    if (href.isEmpty() || !m_session)
        return;
    QTextDocument *d = document();
    const int maxPos = d->characterCount() - 1;
    const int s = qBound(0, m_editStart, maxPos);
    const QTextBlock b = d->findBlock(s);
    if (roleAt(b) != LineRole::Para)
        return;
    const int e = qBound(s, m_editEnd, b.position() + b.length() - 1);
    QString label = textIn;
    label.remove(QChar::ObjectReplacementCharacter);
    label.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (label.trimmed().isEmpty())
        label = e > s ? b.text().mid(s - b.position(), e - s) : hrefIn.trimmed();
    QTextCursor c(d);
    c.beginEditBlock();
    c.setPosition(s);
    c.setPosition(e, QTextCursor::KeepAnchor);
    if (c.selectedText() != label)
        c.insertText(label, links::anchorFormat(href));
    else
        c.setCharFormat(links::anchorFormat(href));
    c.endEditBlock();
    QTextCursor after(d);
    after.setPosition(s + int(label.size()));
    setTextCursor(after);
    setCurrentCharFormat(QTextCharFormat()); // typing on continues as plain text
    ensureCursorVisible();
}

void NoteEditor::removeLink()
{
    const LinkHit h = linkNearCaret();
    if (!h.valid() || !h.named)
        return;
    QTextCursor c(document());
    c.beginEditBlock();
    c.setPosition(h.start);
    c.setPosition(h.end, QTextCursor::KeepAnchor);
    c.setCharFormat(QTextCharFormat());
    c.endEditBlock();
    scheduleLinkPopups();
}

void NoteEditor::preventLinkGrowth()
{
    const QTextCursor c = textCursor();
    if (c.hasSelection() || !links::isLink(c.charFormat()))
        return;
    const LinkHit before = c.atBlockStart() ? LinkHit() : linkAt(c.position() - 1);
    const LinkHit after = c.atBlockEnd() ? LinkHit() : linkAt(c.position());
    const bool inside = before.valid() && after.valid() && before.named && after.named && before.start == after.start;
    if (!inside)
        setCurrentCharFormat(QTextCharFormat());
}

bool NoteEditor::autoLinkTypedMarkdown()
{
    QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    if (c.hasSelection() || roleAt(b) != LineRole::Para)
        return false;
    const int rel = c.positionInBlock();
    const QString text = b.text().left(rel);
    // The link must end at the caret and not sit in code or math.
    QVector<links::Span> spans;
    const QString shown = links::parseMarkdown(text, &spans);
    if (spans.isEmpty() || spans.last().start + spans.last().length != shown.size())
        return false;
    for (int k = int(text.lastIndexOf(QLatin1String("]("))) - 1; k >= 0; --k) {
        if (text[k] != QLatin1Char('[') || (k > 0 && text[k - 1] == QLatin1Char('\\')))
            continue;
        QString label, href;
        int end = 0;
        if (!links::parseLink(text, k, &label, &href, &end) || end != rel)
            continue;
        QTextCursor r(document());
        r.beginEditBlock();
        r.setPosition(b.position() + k);
        r.setPosition(b.position() + rel, QTextCursor::KeepAnchor);
        r.insertText(label, links::anchorFormat(href));
        r.endEditBlock();
        setTextCursor(r);
        setCurrentCharFormat(QTextCharFormat());
        return true;
    }
    return false;
}

} // namespace folia
