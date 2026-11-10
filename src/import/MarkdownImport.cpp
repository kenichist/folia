#include "import/MarkdownImport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

namespace folia::import {

namespace {

bool isCancelled(const std::atomic<bool> *c)
{
    return c && c->load(std::memory_order_relaxed);
}

// Lines as views into the decoded text: no copy per line.
QVector<QStringView> splitLines(const QString &text)
{
    QVector<QStringView> out;
    out.reserve(int(text.count(QLatin1Char('\n'))) + 1);
    qsizetype start = 0;
    for (;;) {
        const qsizetype nl = text.indexOf(QLatin1Char('\n'), start);
        if (nl < 0) {
            out.push_back(QStringView(text).mid(start));
            break;
        }
        out.push_back(QStringView(text).mid(start, nl - start));
        start = nl + 1;
    }
    return out;
}

int leadingSpaces(QStringView l)
{
    int n = 0;
    while (n < l.size() && l[n] == QLatin1Char(' '))
        ++n;
    return n;
}

QString headingPrefix(int level)
{
    return QString(qBound(1, level, 3), QLatin1Char('#')) + QLatin1Char(' ');
}

struct Fence {
    QChar ch;
    int len = 0;
    QString info;
};

bool opensFence(QStringView line, Fence *f)
{
    const int i = leadingSpaces(line);
    if (i > 3 || i >= line.size())
        return false;
    const QChar c = line[i];
    if (c != QLatin1Char('`') && c != QLatin1Char('~'))
        return false;
    int n = 0;
    while (i + n < line.size() && line[i + n] == c)
        ++n;
    if (n < 3)
        return false;
    const QStringView info = line.mid(i + n).trimmed();
    if (c == QLatin1Char('`') && info.contains(QLatin1Char('`')))
        return false; // inline code, not a fence
    f->ch = c;
    f->len = n;
    f->info = info.toString();
    return true;
}

bool closesFence(QStringView line, const Fence &f)
{
    const QStringView t = line.trimmed();
    if (t.size() < f.len)
        return false;
    for (QChar c : t)
        if (c != f.ch)
            return false;
    return true;
}

bool isBlank(QStringView l)
{
    return l.trimmed().isEmpty();
}

const QRegularExpression &kHeading()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}(#{1,6})(?:[ \\t]+(.*?))?(?:[ \\t]+#+)?[ \\t]*$"));
    return re;
}
const QRegularExpression &kListItem()
{
    static const QRegularExpression re(QStringLiteral("^([ \\t]*)([-*+]|\\d{1,9}[.)])(?:[ \\t]+(.*))?$"));
    return re;
}
const QRegularExpression &kQuote()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}>[ ]?(.*)$"));
    return re;
}
const QRegularExpression &kBreak()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}([-*_])(?:[ \\t]*\\1){2,}[ \\t]*$"));
    return re;
}
const QRegularExpression &kSetext1()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}=+[ \\t]*$"));
    return re;
}
const QRegularExpression &kSetext2()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}-+[ \\t]*$"));
    return re;
}
const QRegularExpression &kTableDelimiter()
{
    static const QRegularExpression re(
        QStringLiteral("^[ \\t]*\\|?[ \\t]*:?-+:?[ \\t]*(\\|[ \\t]*:?-+:?[ \\t]*)+\\|?[ \\t]*$"));
    return re;
}
const QRegularExpression &kImage()
{
    static const QRegularExpression re(QStringLiteral(
        "!\\[([^\\]]*)\\]\\([ \\t]*(<[^>]*>|[^\\s)]+)(?:[ \\t]+(?:\"[^\"]*\"|'[^']*'|\\([^)]*\\)))?[ \\t]*\\)"));
    return re;
}

// A local file an image reference points to, or empty (remote, data:).
QString localPath(QString url, const QString &baseDir)
{
    if (url.startsWith(QLatin1Char('<')) && url.endsWith(QLatin1Char('>')))
        url = url.mid(1, url.size() - 2);
    const QUrl u(url);
    if (u.scheme() == QLatin1String("file"))
        return u.toLocalFile();
    if (u.scheme().size() > 1) // http, https, data… ("C:" is a drive)
        return {};
    QString p = QUrl::fromPercentEncoding(url.toUtf8());
    if (QDir::isRelativePath(p))
        p = QDir(baseDir).filePath(p);
    return QDir::cleanPath(p);
}

class MarkdownConverter {
public:
    MarkdownConverter(ImportedDoc *doc, const Limits &limits, const QString &baseDir, const ImageImporter &images)
        : m_doc(doc), m_b(doc, limits), m_baseDir(baseDir), m_images(images)
    {
    }

    bool frontMatter = false; // the first block is the YAML front matter

    void run(const QVector<QStringView> &lines, const std::atomic<bool> *cancel)
    {
        int i = 0;
        const int n = int(lines.size());
        // YAML front matter (Obsidian, static site generators).
        if (n > 1 && lines[0].trimmed() == QLatin1String("---")) {
            for (int j = 1; j < n && j < 1000; ++j) {
                const QStringView t = lines[j].trimmed();
                if (t == QLatin1String("---") || t == QLatin1String("...")) {
                    QStringList body;
                    for (int k = 1; k < j; ++k)
                        body << lines[k].toString();
                    if (!body.isEmpty()) {
                        m_b.code(QStringLiteral("yaml"), body.join(QLatin1Char('\n')));
                        frontMatter = true;
                    }
                    i = j + 1;
                    break;
                }
            }
        }
        while (i < n && !m_b.full()) {
            if ((i & 1023) == 0 && isCancelled(cancel)) {
                m_doc->error = QStringLiteral("cancelled");
                return;
            }
            const QStringView line = lines[i];
            Fence fence;
            if (opensFence(line, &fence)) {
                flush();
                QString body;
                int j = i + 1;
                for (; j < n && !closesFence(lines[j], fence); ++j) {
                    if (j > i + 1)
                        body += QLatin1Char('\n');
                    body += lines[j];
                }
                if (j >= n) // unclosed: runs to the end, without the file's trailing blank lines
                    while (body.endsWith(QLatin1Char('\n')))
                        body.chop(1);
                const QString lang = fence.info.section(QLatin1Char(' '), 0, 0);
                m_b.code(lang, body);
                i = j + 1; // an unclosed fence runs to the end, as in the editor
                m_lastList = false;
                continue;
            }
            const QStringView t = line.trimmed();
            if (t == QLatin1String("$$")) {
                flush();
                QString body;
                int j = i + 1;
                for (; j < n && lines[j].trimmed() != QLatin1String("$$"); ++j) {
                    if (j > i + 1)
                        body += QLatin1Char('\n');
                    body += lines[j];
                }
                m_b.math(body, false);
                i = j + 1;
                m_lastList = false;
                continue;
            }
            if (t.size() > 4 && t.startsWith(QLatin1String("$$")) && t.endsWith(QLatin1String("$$"))) {
                flush();
                m_b.math(t.mid(2, t.size() - 4).trimmed().toString(), true);
                ++i;
                m_lastList = false;
                continue;
            }
            if (t.isEmpty()) {
                flush();
                ++i;
                continue;
            }
            // Indented code (4 spaces or a tab), not inside a list.
            if (m_para.isEmpty() && !m_lastList
                && (line.startsWith(QLatin1String("    ")) || line.startsWith(QLatin1Char('\t')))) {
                QStringList body;
                int j = i;
                for (; j < n; ++j) {
                    const QStringView l = lines[j];
                    if (l.startsWith(QLatin1String("    ")))
                        body << l.mid(4).toString();
                    else if (l.startsWith(QLatin1Char('\t')))
                        body << l.mid(1).toString();
                    else if (isBlank(l) && j + 1 < n
                             && (lines[j + 1].startsWith(QLatin1String("    ")) || lines[j + 1].startsWith(QLatin1Char('\t'))))
                        body << QString();
                    else
                        break;
                }
                m_b.code(QString(), body.join(QLatin1Char('\n')));
                i = j;
                continue;
            }
            // Setext headings: a paragraph underlined with === or ---.
            if (m_kind == Kind::Plain && !m_para.isEmpty()) {
                if (kSetext1().matchView(line).hasMatch()) {
                    m_b.text(headingPrefix(1) + m_para);
                    m_para.clear();
                    ++i;
                    continue;
                }
                if (kSetext2().matchView(line).hasMatch()) {
                    m_b.text(headingPrefix(2) + m_para);
                    m_para.clear();
                    ++i;
                    continue;
                }
            }
            if (kBreak().matchView(line).hasMatch()) {
                flush();
                m_b.text(QStringLiteral("---"));
                m_lastList = false;
                ++i;
                continue;
            }
            if (const QRegularExpressionMatch h = kHeading().matchView(line); h.hasMatch()) {
                flush();
                m_b.text(headingPrefix(int(h.capturedLength(1))) + h.captured(2).trimmed());
                m_lastList = false;
                ++i;
                continue;
            }
            // Tables: kept row by row (a header followed by its delimiter row).
            if (t.contains(QLatin1Char('|')) && i + 1 < n && kTableDelimiter().matchView(lines[i + 1]).hasMatch()) {
                flush();
                int j = i;
                for (; j < n && !isBlank(lines[j]) && lines[j].contains(QLatin1Char('|')); ++j)
                    m_b.text(lines[j].trimmed().toString());
                i = j;
                m_lastList = false;
                continue;
            }
            if (const QRegularExpressionMatch q = kQuote().matchView(line); q.hasMatch()) {
                const QString content = q.captured(1).trimmed();
                if (m_kind != Kind::Quote || content.isEmpty())
                    flush();
                if (!content.isEmpty()) {
                    if (m_para.isEmpty()) {
                        m_kind = Kind::Quote;
                        m_para = QStringLiteral("> ") + content;
                    } else {
                        m_para += QLatin1Char(' ') + content;
                    }
                }
                m_lastList = false;
                ++i;
                continue;
            }
            if (const QRegularExpressionMatch li = kListItem().matchView(line); li.hasMatch()) {
                flush();
                QString indent = li.captured(1);
                indent.replace(QLatin1Char('\t'), QStringLiteral("  "));
                QString marker = li.captured(2);
                if (marker.endsWith(QLatin1Char(')')))
                    marker[marker.size() - 1] = QLatin1Char('.');
                m_kind = Kind::List;
                m_para = indent + marker + QLatin1Char(' ') + li.captured(3).trimmed();
                m_lastList = true;
                ++i;
                continue;
            }
            // Paragraph text; soft-wrapped lines join into one line (a hard
            // break, two trailing spaces or a backslash, starts a new one).
            const bool hardBreak = i > 0 && !m_para.isEmpty()
                && (lines[i - 1].endsWith(QLatin1String("  ")) || lines[i - 1].endsWith(QLatin1Char('\\')));
            if (hardBreak) {
                if (m_para.endsWith(QLatin1Char('\\')))
                    m_para.chop(1);
                flush();
            }
            if (m_para.isEmpty()) {
                m_kind = Kind::Plain;
                m_para = t.toString();
                m_lastList = false;
            } else {
                m_para += QLatin1Char(' '); // continues the paragraph, list item or quote above
                m_para += t;
            }
            ++i;
        }
        flush();
    }

private:
    enum class Kind { Plain, List, Quote };

    void flush()
    {
        if (m_para.isEmpty())
            return;
        emitParagraph(m_para);
        m_para.clear();
        m_kind = Kind::Plain;
    }

    void emitText(QString s)
    {
        while (s.endsWith(QLatin1Char(' ')))
            s.chop(1);
        const QString t = s.trimmed();
        if (t.isEmpty() || t == QLatin1String(">"))
            return;
        if (const QRegularExpressionMatch li = kListItem().match(t); li.hasMatch() && li.captured(3).isEmpty())
            return; // only a list marker left after an image was split out
        m_b.text(s);
    }

    // Images on their own become image lines; the text around them stays.
    void emitParagraph(const QString &para)
    {
        if (!para.contains(QLatin1String("!["))) {
            m_b.text(para);
            return;
        }
        if (!m_images) {
            emitText(linksForImages(para));
            return;
        }
        QString pending;
        qsizetype at = 0;
        bool afterImage = false; // text after an image line starts a new line: no leading spaces
        QRegularExpressionMatchIterator it = kImage().globalMatch(para);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString alt = m.captured(1);
            QString url = m.captured(2);
            QStringView before = QStringView(para).mid(at, m.capturedStart() - at);
            if (afterImage && pending.isEmpty())
                before = before.trimmed();
            pending += before;
            at = m.capturedEnd();
            const QString path = localPath(url, m_baseDir);
            const QString line = path.isEmpty() ? QString() : m_images(path, alt);
            if (!line.isEmpty()) {
                emitText(pending);
                pending.clear();
                m_b.text(line);
                afterImage = true;
            } else {
                pending += linkFor(alt, url);
            }
        }
        QStringView rest = QStringView(para).mid(at);
        if (afterImage && pending.isEmpty())
            rest = rest.trimmed();
        pending += rest;
        emitText(pending);
    }

    QString linksForImages(const QString &para)
    {
        QString out;
        qsizetype at = 0;
        QRegularExpressionMatchIterator it = kImage().globalMatch(para);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += QStringView(para).mid(at, m.capturedStart() - at);
            out += linkFor(m.captured(1), m.captured(2));
            at = m.capturedEnd();
        }
        out += QStringView(para).mid(at);
        return out;
    }

    QString linkFor(const QString &alt, QString url)
    {
        if (url.startsWith(QLatin1Char('<')) && url.endsWith(QLatin1Char('>')))
            url = url.mid(1, url.size() - 2);
        if (url.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
            if (!m_dataNoted)
                m_doc->notes << QStringLiteral("Images embedded in the text (data: addresses) were left out.");
            m_dataNoted = true;
            return alt;
        }
        return QLatin1Char('[') + (alt.isEmpty() ? url : alt) + QStringLiteral("](") + url + QLatin1Char(')');
    }

    ImportedDoc *m_doc;
    DocBuilder m_b;
    QString m_baseDir;
    ImageImporter m_images;
    QString m_para;
    Kind m_kind = Kind::Plain;
    bool m_lastList = false;     // the last block was a list item (indented lines continue it)
    bool m_dataNoted = false;
};

} // namespace

QString decodeText(const QByteArray &bytes)
{
    QString s;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        s = QString::fromUtf8(bytes.constData() + 3, bytes.size() - 3);
    } else if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        QStringDecoder dec(bytes.startsWith("\xFF\xFE") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        s = dec(QByteArrayView(bytes).mid(2));
    } else {
        QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        s = dec(bytes);
        if (dec.hasError())
            s = QString::fromLatin1(bytes);
    }
    // One kind of line end; no characters the editor gives a meaning to
    // (U+FFFC stands for page links, images and PDFs in a page).
    s.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    s.remove(QChar::ObjectReplacementCharacter);
    s.remove(QChar(0));
    return s;
}

ImportedDoc fromMarkdown(const QString &text, const QString &fallbackTitle, const QString &baseDir,
                         const ImageImporter &images, const std::atomic<bool> *cancel, const Limits &limits)
{
    ImportedDoc doc;
    MarkdownConverter conv(&doc, limits, baseDir, images);
    conv.run(splitLines(text), cancel);
    // A leading "# " heading (after any front matter) is the title.
    const int first = conv.frontMatter ? 1 : 0;
    if (doc.blocks.size() > first && doc.blocks[first].kind == BlockKind::Text
        && doc.blocks[first].text.startsWith(QLatin1String("# "))) {
        doc.title = doc.blocks[first].text.mid(2).trimmed();
        doc.outputChars -= doc.blocks[first].text.size();
        doc.blocks.removeAt(first);
    }
    if (doc.title.isEmpty())
        doc.title = fallbackTitle;
    return doc;
}

ImportedDoc fromPlainText(const QString &text, const QString &title, const std::atomic<bool> *cancel,
                          const Limits &limits)
{
    ImportedDoc doc;
    doc.title = title;
    DocBuilder b(&doc, limits);
    const QVector<QStringView> lines = splitLines(text);
    for (int i = 0; i < lines.size() && !b.full(); ++i) {
        if ((i & 1023) == 0 && isCancelled(cancel)) {
            doc.error = QStringLiteral("cancelled");
            return doc;
        }
        QStringView l = lines[i];
        while (!l.isEmpty() && l.last().isSpace())
            l.chop(1);
        if (!l.isEmpty())
            b.text(l.toString());
    }
    return doc;
}

ImportedDoc importTextFile(const QString &path, const ImageImporter &images, const std::atomic<bool> *cancel,
                           const Limits &limits)
{
    ImportedDoc doc;
    const QFileInfo fi(path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        doc.error = f.errorString();
        return doc;
    }
    if (f.size() > limits.maxInputBytes) {
        doc.error = QStringLiteral("it is larger than %1 MB").arg(limits.maxInputBytes >> 20);
        return doc;
    }
    QString text = decodeText(f.readAll());
    f.close();
    if (fi.suffix().compare(QLatin1String("txt"), Qt::CaseInsensitive) == 0)
        return fromPlainText(text, fi.completeBaseName(), cancel, limits);
    return fromMarkdown(text, fi.completeBaseName(), fi.absolutePath(), images, cancel, limits);
}

} // namespace folia::import
