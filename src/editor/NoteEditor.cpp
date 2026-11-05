#include "editor/NoteEditor.h"

#include "core/Instrument.h"
#include "editor/EditorContext.h"
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
    if (handleSmartKeys(e)) {
        e->accept();
        ensureCursorVisible();
        return;
    }
    QTextEdit::keyPressEvent(e);
}

void NoteEditor::inputMethodEvent(QInputMethodEvent *e)
{
    // A preedit-only update (no commit, no replacement, nothing selected to
    // replace) changes presentation, not the page: keep the revision.
    const bool presentationOnly = e->commitString().isEmpty() && e->replacementLength() == 0
        && !textCursor().hasSelection();
    if (m_session)
        m_session->setPresentationOnly(presentationOnly);
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
}

void NoteEditor::scrollContentsBy(int dx, int dy)
{
    QTextEdit::scrollContentsBy(dx, dy);
    updateLayoutViewport();
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
            QString t = b.text().mid(s, qMax(0, e - s));
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
    c.insertText(t);
    setTextCursor(c);
    ensureCursorVisible();
}

} // namespace folia
