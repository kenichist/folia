#include "editor/Links.h"

#include <QRegularExpression>
#include <QTextFragment>

namespace folia::links {

namespace {

bool balancedParens(const QString &s)
{
    int depth = 0;
    for (QChar c : s) {
        if (c == QLatin1Char('('))
            ++depth;
        else if (c == QLatin1Char(')') && --depth < 0)
            return false;
    }
    return depth == 0;
}

} // namespace

// The label may contain escaped brackets/backslashes; the url may contain
// balanced parentheses but no whitespace.
bool parseLink(const QString &s, int i, QString *label, QString *href, int *end)
{
    if (i < 0 || i >= s.size() || s[i] != QLatin1Char('['))
        return false;
    const int n = int(s.size());
    int j = i + 1;
    QString l;
    while (j < n) {
        const QChar ch = s[j];
        if (ch == QLatin1Char('\\') && j + 1 < n
            && (s[j + 1] == QLatin1Char('[') || s[j + 1] == QLatin1Char(']') || s[j + 1] == QLatin1Char('\\'))) {
            l += s[j + 1];
            j += 2;
            continue;
        }
        if (ch == QLatin1Char('['))
            return false;
        if (ch == QLatin1Char(']'))
            break;
        l += ch;
        ++j;
    }
    if (j >= n || l.isEmpty() || j + 1 >= n || s[j + 1] != QLatin1Char('('))
        return false;
    int k = j + 2, depth = 0;
    while (k < n) {
        const QChar ch = s[k];
        if (ch.isSpace())
            return false;
        if (ch == QLatin1Char('('))
            ++depth;
        else if (ch == QLatin1Char(')')) {
            if (depth == 0)
                break;
            --depth;
        }
        ++k;
    }
    if (k >= n || k == j + 2)
        return false;
    *label = l;
    *href = s.mid(j + 2, k - j - 2);
    *end = k + 1;
    return true;
}

QString parseMarkdown(const QString &s, QVector<Span> *spans)
{
    if (!s.contains(QLatin1String("](")))
        return s;
    const int n = int(s.size());
    QString out;
    out.reserve(n);
    bool any = false;
    int i = 0;
    while (i < n) {
        const QChar c = s[i];
        if (c == QLatin1Char('\\') && i + 1 < n) {
            out += c;
            out += s[i + 1];
            i += 2;
            continue;
        }
        if (c == QLatin1Char('`')) {
            // Same rule as the editor's inline-code styling: a run of N
            // backticks is closed only by a run of exactly N.
            int run = 1;
            while (i + run < n && s[i + run] == QLatin1Char('`'))
                ++run;
            const QString fence(run, QLatin1Char('`'));
            int e = int(s.indexOf(fence, i + run));
            while (e >= 0 && e + run < n && s[e + run] == QLatin1Char('`'))
                e = int(s.indexOf(fence, e + run + 1));
            const int stop = e >= 0 ? e + run : i + run;
            out += QStringView(s).mid(i, stop - i);
            i = stop;
            continue;
        }
        if (c == QLatin1Char('$') && i + 1 < n && s[i + 1] != QLatin1Char('$')) {
            int e = i + 1;
            while (e < n && !(s[e] == QLatin1Char('$') && s[e - 1] != QLatin1Char('\\')))
                ++e;
            if (e < n && e > i + 1) {
                out += QStringView(s).mid(i, e + 1 - i);
                i = e + 1;
                continue;
            }
        }
        if (c == QLatin1Char('[')) {
            QString label, href;
            int end = 0;
            if (parseLink(s, i, &label, &href, &end)) {
                if (spans)
                    spans->push_back({int(out.size()), int(label.size()), href});
                out += label;
                i = end;
                any = true;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return any ? out : s;
}

QString toMarkdown(const QString &label, const QString &href)
{
    QString l;
    l.reserve(label.size() + 4);
    for (int i = 0; i < label.size(); ++i) {
        const QChar c = label[i];
        if (c == QLatin1Char('[') || c == QLatin1Char(']')) {
            l += QLatin1Char('\\');
            l += c;
        } else if (c == QLatin1Char('\\')) {
            const bool special = i + 1 >= label.size() || label[i + 1] == QLatin1Char('[')
                || label[i + 1] == QLatin1Char(']') || label[i + 1] == QLatin1Char('\\');
            l += special ? QStringLiteral("\\\\") : QStringLiteral("\\");
        } else {
            l += c;
        }
    }
    QString h = href;
    static const QRegularExpression ws(QStringLiteral("\\s"));
    if (h.contains(ws)) {
        h.replace(QLatin1Char(' '), QLatin1String("%20"));
        h.replace(ws, QString());
    }
    if (!balancedParens(h)) {
        h.replace(QLatin1Char('('), QLatin1String("%28"));
        h.replace(QLatin1Char(')'), QLatin1String("%29"));
    }
    return QLatin1Char('[') + l + QStringLiteral("](") + h + QLatin1Char(')');
}

QTextCharFormat anchorFormat(const QString &href)
{
    QTextCharFormat f;
    f.setAnchor(true);
    f.setAnchorHref(href);
    return f;
}

bool blockHasLinks(const QTextBlock &b)
{
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid() && isLink(f.charFormat()))
            return true;
    }
    return false;
}

QString blockSource(const QTextBlock &b, int from, int to)
{
    const QString text = b.text();
    const int len = int(text.size());
    from = qBound(0, from, len);
    to = to < 0 ? len : qBound(from, to, len);
    if (!blockHasLinks(b))
        return text.mid(from, to - from);
    QString out;
    QString runHref, runLabel;
    auto flush = [&] {
        if (!runHref.isEmpty())
            out += toMarkdown(runLabel, runHref);
        runHref.clear();
        runLabel.clear();
    };
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid())
            continue;
        const int fs = f.position() - b.position();
        const int s = qMax(fs, from), e = qMin(fs + f.length(), to);
        if (e <= s)
            continue;
        const QString piece = text.mid(s, e - s);
        const QTextCharFormat cf = f.charFormat();
        const QString href = isLink(cf) && !cf.isImageFormat() ? cf.anchorHref() : QString();
        if (href.isEmpty()) {
            flush();
            out += piece;
        } else if (href == runHref) {
            runLabel += piece;
        } else {
            flush();
            runHref = href;
            runLabel = piece;
        }
    }
    flush();
    return out;
}

int bareUrlLength(QStringView text, int i)
{
    const int n = int(text.size());
    if (i < 0 || i >= n)
        return 0;
    if (i > 0) {
        const QChar p = text[i - 1];
        if (p.isLetterOrNumber() || p == QLatin1Char('/') || p == QLatin1Char('.') || p == QLatin1Char('@')
            || p == QLatin1Char(':') || p == QLatin1Char('-') || p == QLatin1Char('_'))
            return 0;
    }
    static const char *const kPrefixes[] = {"https://", "http://", "www.", "mailto:", "ftp://", "file://"};
    int pre = 0;
    for (const char *prefix : kPrefixes) {
        const QLatin1StringView pv(prefix);
        if (text.mid(i).startsWith(pv, Qt::CaseInsensitive)) {
            pre = int(pv.size());
            break;
        }
    }
    if (!pre)
        return 0;
    int j = i + pre;
    while (j < n) {
        const QChar ch = text[j];
        if (ch.isSpace() || ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('"')
            || ch == QLatin1Char('`') || ch == QChar::ObjectReplacementCharacter)
            break;
        ++j;
    }
    // Trailing punctuation belongs to the sentence, not the address.
    while (j > i + pre) {
        const QChar ch = text[j - 1];
        if (QStringView(u".,;:!?'*_~").contains(ch)) {
            --j;
            continue;
        }
        if (ch == QLatin1Char(')') || ch == QLatin1Char(']')) {
            const QChar open = ch == QLatin1Char(')') ? QLatin1Char('(') : QLatin1Char('[');
            const QStringView u = text.mid(i, j - i);
            if (u.count(ch) > u.count(open)) {
                --j;
                continue;
            }
        }
        break;
    }
    return j - (i + pre) >= 1 ? j - i : 0;
}

QString bareUrlHref(QStringView url)
{
    if (url.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
        return QStringLiteral("https://") + url.toString();
    return url.toString();
}

QString normalizeUrl(const QString &input)
{
    const QString t = input.trimmed();
    if (t.isEmpty())
        return QString();
    static const QRegularExpression withScheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    static const QRegularExpression opaque(QStringLiteral("^(mailto|tel|sms|file|data):"), QRegularExpression::CaseInsensitiveOption);
    if (withScheme.match(t).hasMatch() || opaque.match(t).hasMatch())
        return t;
    if (t.startsWith(QLatin1Char('/')) || t.startsWith(QLatin1Char('#')))
        return t;
    if (t.contains(QLatin1Char('@')) && !t.contains(QLatin1Char('/')) && !t.contains(QLatin1Char(' ')))
        return QStringLiteral("mailto:") + t;
    return QStringLiteral("https://") + t;
}

bool looksLikeUrl(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || t.contains(QRegularExpression(QStringLiteral("\\s"))))
        return false;
    if (bareUrlLength(t, 0) == t.size())
        return true;
    static const QRegularExpression domain(
        QStringLiteral("^[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}(:[0-9]+)?(/\\S*)?$"));
    return domain.match(t).hasMatch();
}

} // namespace folia::links
