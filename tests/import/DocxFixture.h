// Builds .docx files for tests: a minimal zip writer (stored or deflated
// entries) and Word-style parts (rels, styles, numbering, core properties,
// a PNG and an EMF) around a given document body.
#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QTemporaryDir>

namespace docxfixture {

// ------------------------------------------------- a minimal zip writer ----
inline quint32 crc32(const QByteArray &d)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (char ch : d)
        c = table[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void le16(QByteArray &o, quint16 v) { o.append(char(v & 0xff)).append(char(v >> 8)); }
inline void le32(QByteArray &o, quint32 v) { le16(o, quint16(v & 0xffff)); le16(o, quint16(v >> 16)); }

// Entries stored, or deflated (qCompress's zlib stream without its 2-byte
// header and 4-byte checksum is raw deflate).
inline QByteArray makeZip(const QList<QPair<QString, QByteArray>> &entries, bool deflate = true)
{
    QByteArray out, central;
    for (const auto &e : entries) {
        const QByteArray name = e.first.toUtf8();
        QByteArray data = e.second;
        quint16 method = 0;
        if (deflate && !data.isEmpty()) {
            const QByteArray z = qCompress(data, 9);
            data = z.mid(4 + 2, z.size() - 4 - 2 - 4);
            method = 8;
        }
        const quint32 offset = quint32(out.size());
        const quint32 crc = crc32(e.second);
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, method); le16(out, 0); le16(out, 0x21);
        le32(out, crc); le32(out, quint32(data.size())); le32(out, quint32(e.second.size()));
        le16(out, quint16(name.size())); le16(out, 0);
        out += name;
        out += data;
        le32(central, 0x02014b50); le16(central, 20); le16(central, 20); le16(central, 0); le16(central, method);
        le16(central, 0); le16(central, 0x21); le32(central, crc); le32(central, quint32(data.size()));
        le32(central, quint32(e.second.size())); le16(central, quint16(name.size())); le16(central, 0); le16(central, 0);
        le16(central, 0); le16(central, 0); le32(central, 0); le32(central, offset);
        central += name;
    }
    const quint32 cdOffset = quint32(out.size());
    out += central;
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, quint16(entries.size()));
    le16(out, quint16(entries.size())); le32(out, quint32(central.size())); le32(out, cdOffset); le16(out, 0);
    return out;
}

inline const char *kW = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                 "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                 "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                 "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                 "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
                 "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" "
                 "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"";

inline QByteArray documentXml(const QByteArray &body)
{
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><w:document ") + kW
        + "><w:body>" + body + "<w:sectPr><w:pgSz w:w=\"12240\"/></w:sectPr></w:body></w:document>";
}

inline QList<QPair<QString, QByteArray>> docxParts(const QByteArray &body)
{
    const QByteArray rels =
        "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "</Relationships>";
    const QByteArray docRels =
        "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdS\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rIdN\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "<Relationship Id=\"rIdL\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"https://example.com/x\" TargetMode=\"External\"/>"
        "<Relationship Id=\"rIdI\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image1.png\"/>"
        "<Relationship Id=\"rIdE\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image2.emf\"/>"
        "</Relationships>";
    const QByteArray styles = QByteArray("<?xml version=\"1.0\"?><w:styles ") + kW + ">"
        "<w:style w:type=\"paragraph\" w:styleId=\"Titel\"><w:name w:val=\"Title\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"berschrift1\"><w:name w:val=\"heading 1\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"H4\"><w:name w:val=\"heading 4\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"MyHeading\"><w:name w:val=\"My heading\"/><w:basedOn w:val=\"berschrift1\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Zitat\"><w:name w:val=\"Quote\"/></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"Strong\"><w:name w:val=\"Strong\"/><w:rPr><w:b/></w:rPr></w:style>"
        "</w:styles>";
    const QByteArray numbering = QByteArray("<?xml version=\"1.0\"?><w:numbering ") + kW + ">"
        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
        "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
        "</w:numbering>";
    const QByteArray core = "<?xml version=\"1.0\"?><cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Core title</dc:title></cp:coreProperties>";
    QImage img(8, 6, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray png;
    QBuffer pb(&png);
    pb.open(QIODevice::WriteOnly);
    img.save(&pb, "PNG");
    return {{QStringLiteral("[Content_Types].xml"), "<Types/>"},
            {QStringLiteral("_rels/.rels"), rels},
            {QStringLiteral("word/_rels/document.xml.rels"), docRels},
            {QStringLiteral("word/styles.xml"), styles},
            {QStringLiteral("word/numbering.xml"), numbering},
            {QStringLiteral("docProps/core.xml"), core},
            {QStringLiteral("word/media/image1.png"), png},
            {QStringLiteral("word/media/image2.emf"), QByteArray(64, '\x01')},
            {QStringLiteral("word/document.xml"), documentXml(body)}};
}

inline QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString p = dir.filePath(name);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        qFatal("cannot write %s", qPrintable(p));
    return p;
}

inline QByteArray para(const QByteArray &style, const QByteArray &runs, const QByteArray &extraPPr = {})
{
    QByteArray ppr = style.isEmpty() ? QByteArray() : "<w:pStyle w:val=\"" + style + "\"/>";
    ppr += extraPPr;
    return "<w:p>" + (ppr.isEmpty() ? QByteArray() : "<w:pPr>" + ppr + "<w:rPr><w:b/></w:rPr></w:pPr>") + runs + "</w:p>";
}
inline QByteArray run(const QByteArray &text, const QByteArray &rpr = {})
{
    return "<w:r>" + (rpr.isEmpty() ? QByteArray() : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t xml:space=\"preserve\">"
        + text + "</w:t></w:r>";
}
inline QByteArray listPara(int numId, int ilvl, const QByteArray &text)
{
    return para({}, run(text), "<w:numPr><w:ilvl w:val=\"" + QByteArray::number(ilvl) + "\"/><w:numId w:val=\""
                                   + QByteArray::number(numId) + "\"/></w:numPr>");
}
inline QByteArray image(const QByteArray &rid, const QByteArray &descr)
{
    return "<w:r><w:drawing><wp:inline><wp:docPr id=\"1\" name=\"Picture 1\" descr=\"" + descr
        + "\"/><a:graphic><a:graphicData><a:blip r:embed=\"" + rid + "\"/></a:graphicData></a:graphic></wp:inline>"
          "</w:drawing></w:r>";
}

} // namespace docxfixture
