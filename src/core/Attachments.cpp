#include "core/Attachments.h"

namespace folia::attachments {

bool isHash(QStringView s)
{
    if (s.size() != kHashLength)
        return false;
    for (QChar c : s) {
        const char16_t u = c.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f')))
            return false;
    }
    return true;
}

bool parseImageMarkdown(QStringView t, QString *hash, QString *alt)
{
    static const QLatin1String kMid("](attachment:");
    if (!mayBeImageMarkdown(t))
        return false;
    // The hash is the fixed-size tail; everything between "![" and "](" is alt.
    const qsizetype hashAt = t.size() - 1 - kHashLength;
    const qsizetype midAt = hashAt - kMid.size();
    if (midAt < 2 || t.mid(midAt, kMid.size()) != kMid)
        return false;
    const QStringView h = t.mid(hashAt, kHashLength);
    if (!isHash(h))
        return false;
    QString a;
    const QStringView raw = t.mid(2, midAt - 2);
    a.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QChar c = raw[i];
        if (c == QLatin1Char('\\') && i + 1 < raw.size()) {
            a += raw[++i];
            continue;
        }
        if (c == QLatin1Char('[') || c == QLatin1Char(']'))
            return false; // unescaped bracket: not our form
        a += c;
    }
    if (hash)
        *hash = h.toString();
    if (alt)
        *alt = a;
    return true;
}

QString imageMarkdown(const QString &hash, const QString &alt)
{
    QString out;
    out.reserve(alt.size() + kHashLength + 18);
    out += QLatin1String("![");
    for (QChar c : alt) {
        if (c == QLatin1Char('[') || c == QLatin1Char(']') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        if (c == QLatin1Char('\n') || c == QChar::ObjectReplacementCharacter)
            continue;
        out += c;
    }
    out += QLatin1String("](attachment:");
    out += hash;
    out += QLatin1Char(')');
    return out;
}

} // namespace folia::attachments
