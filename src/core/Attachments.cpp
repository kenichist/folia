#include "core/Attachments.h"

#include <cstring>

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

namespace {

// `<prefix>label](attachment:<hash>)`, prefix "![" or "[".
bool parseRef(QStringView t, qsizetype prefix, QString *hash, QString *alt)
{
    static const QLatin1String kMid("](attachment:");
    // The hash is the fixed-size tail; everything between the prefix and "](" is the label.
    const qsizetype hashAt = t.size() - 1 - kHashLength;
    const qsizetype midAt = hashAt - kMid.size();
    if (midAt < prefix || t.mid(midAt, kMid.size()) != kMid)
        return false;
    const QStringView h = t.mid(hashAt, kHashLength);
    if (!isHash(h))
        return false;
    QString a;
    const QStringView raw = t.mid(prefix, midAt - prefix);
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

QString makeRef(QLatin1String prefix, const QString &hash, const QString &alt)
{
    QString out;
    out.reserve(alt.size() + kHashLength + 18);
    out += prefix;
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

template <typename T>
void put(QByteArray &b, T v)
{
    for (size_t i = 0; i < sizeof(T); ++i) // little-endian whatever the host
        b.append(char((v >> (8 * i)) & 0xff));
}

template <typename T>
T get(const char *p)
{
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        v |= T(T(uchar(p[i])) << (8 * i));
    return v;
}

quint32 floatBits(float f)
{
    quint32 u;
    static_assert(sizeof(u) == sizeof(f));
    memcpy(&u, &f, sizeof(u));
    return u;
}

float bitsFloat(quint32 u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

constexpr int kRunBytesV1 = 4 + 4 + 4 + 2;
constexpr int kRunBytesV2 = kRunBytesV1 + 2 + 4;

} // namespace

bool parseImageMarkdown(QStringView t, QString *hash, QString *alt)
{
    return mayBeImageMarkdown(t) && parseRef(t, 2, hash, alt);
}

bool parseFileMarkdown(QStringView t, QString *hash, QString *name)
{
    return mayBeFileMarkdown(t) && parseRef(t, 1, hash, name);
}

bool parseAnyMarkdown(QStringView t, QString *hash)
{
    return parseImageMarkdown(t, hash, nullptr) || parseFileMarkdown(t, hash, nullptr);
}

QString imageMarkdown(const QString &hash, const QString &alt)
{
    return makeRef(QLatin1String("!["), hash, alt);
}

QString fileMarkdown(const QString &hash, const QString &name)
{
    return makeRef(QLatin1String("["), hash, name);
}

QByteArray encodePageGeom(const QVector<PdfPageGeom> &pages)
{
    QByteArray b;
    if (pages.isEmpty())
        return b;
    b.append(char(2));
    for (qsizetype i = 0; i < pages.size();) {
        qsizetype j = i + 1;
        while (j < pages.size() && pages[j] == pages[i])
            ++j;
        put<quint32>(b, quint32(j - i));
        put<quint32>(b, floatBits(pages[i].w));
        put<quint32>(b, floatBits(pages[i].h));
        put<quint16>(b, quint16(pages[i].rotation));
        put<quint16>(b, pages[i].flags);
        put<quint32>(b, pages[i].imagePixels);
        i = j;
    }
    return b;
}

namespace {

// Calls fn(count, geom) per run; false on a malformed blob.
template <typename Fn>
bool forEachRun(const QByteArray &blob, Fn fn)
{
    if (blob.isEmpty())
        return false;
    const int version = blob[0];
    const int run = version == 1 ? kRunBytesV1 : version == 2 ? kRunBytesV2 : 0;
    if (run == 0 || blob.size() < 1 + run || (blob.size() - 1) % run != 0)
        return false;
    for (qsizetype at = 1; at + run <= blob.size(); at += run) {
        const char *p = blob.constData() + at;
        const quint32 n = get<quint32>(p);
        PdfPageGeom g;
        g.w = bitsFloat(get<quint32>(p + 4));
        g.h = bitsFloat(get<quint32>(p + 8));
        g.rotation = get<quint16>(p + 12);
        if (version >= 2) {
            g.flags = get<quint16>(p + 14);
            g.imagePixels = get<quint32>(p + 16);
        }
        if (n == 0 || n > (1u << 24) || !(g.w > 0) || !(g.h > 0))
            return false;
        if (!fn(n, g))
            return true;
    }
    return true;
}

} // namespace

QVector<PdfPageGeom> decodePageGeom(const QByteArray &blob)
{
    QVector<PdfPageGeom> out;
    const bool ok = forEachRun(blob, [&](quint32 n, const PdfPageGeom &g) {
        for (quint32 k = 0; k < n; ++k)
            out.push_back(g);
        return true;
    });
    return ok ? out : QVector<PdfPageGeom>();
}

PdfPageGeom firstPageGeom(const QByteArray &blob)
{
    PdfPageGeom first;
    forEachRun(blob, [&](quint32, const PdfPageGeom &g) {
        first = g;
        return false;
    });
    return first;
}

} // namespace folia::attachments
