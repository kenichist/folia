// Document import (docs/ATTACHMENTS.md §8): Markdown and text conversion,
// decoding, limits and cancellation.

#include "import/DocxImport.h"
#include "import/MarkdownImport.h"
#include "import/ZipReader.h"

#include "DocxFixture.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

using namespace folia;
using namespace folia::import;
using namespace docxfixture;

namespace {

QStringList texts(const ImportedDoc &d)
{
    QStringList out;
    for (const BlockRecord &b : d.blocks) {
        switch (b.kind) {
        case BlockKind::Code: out << QStringLiteral("CODE[%1]:%2").arg(b.lang, b.text); break;
        case BlockKind::Math: out << QStringLiteral("MATH[%1]:%2").arg(b.lang, b.text); break;
        default: out << b.text; break;
        }
    }
    return out;
}

QString hashFor(const QString &path)
{
    return QString(64, QLatin1Char(path.endsWith(QLatin1String("cat.png")) ? 'a' : 'b'));
}

} // namespace

class ImportTests : public QObject {
    Q_OBJECT
private slots:
    void markdownMapsOntoPageLines()
    {
        const QString md = QStringLiteral(
            "---\ntags: [a]\n---\n"
            "# My Title\n\n"
            "Some *soft*\nwrapped text.\n\n"
            "## Sub\n#### Deep ####\n"
            "Setext\n======\n"
            "- item one\n  continued\n"
            "1) first\n"
            "> quote line\n> more\n\n"
            "```py extra\nprint(1)\n\nx = 2\n```\n"
            "$$\nx^2\n$$\n"
            "$$y$$\n"
            "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
            "    indented code\n"
            "#tag line\n"
            "***\n"
            "Line with two spaces  \nnext\n"
            "~~~\nunclosed\n");
        const ImportedDoc d = fromMarkdown(md, QStringLiteral("file"), QString(), {});
        QVERIFY(d.error.isEmpty());
        QCOMPARE(d.title, QStringLiteral("My Title"));
        const QStringList expected = {
            QStringLiteral("CODE[yaml]:tags: [a]"),
            QStringLiteral("Some *soft* wrapped text."),
            QStringLiteral("## Sub"),
            QStringLiteral("### Deep"),
            QStringLiteral("# Setext"),
            QStringLiteral("- item one continued"),
            QStringLiteral("1. first"),
            QStringLiteral("> quote line more"),
            QStringLiteral("CODE[py]:print(1)\n\nx = 2"),
            QStringLiteral("MATH[]:x^2"),
            QStringLiteral("MATH[single]:y"),
            QStringLiteral("| a | b |"),
            QStringLiteral("|---|---|"),
            QStringLiteral("| 1 | 2 |"),
            QStringLiteral("CODE[]:indented code"),
            QStringLiteral("#tag line"),
            QStringLiteral("---"),
            QStringLiteral("Line with two spaces"),
            QStringLiteral("next"),
            QStringLiteral("CODE[]:unclosed"),
        };
        QCOMPARE(texts(d), expected);
        QVERIFY(d.notes.isEmpty());
    }

    void titleFallsBackToTheFileName()
    {
        const ImportedDoc d = fromMarkdown(QStringLiteral("Just text\n# Later heading\n"), QStringLiteral("notes"),
                                           QString(), {});
        QCOMPARE(d.title, QStringLiteral("notes"));
        QCOMPARE(texts(d), QStringList({QStringLiteral("Just text"), QStringLiteral("# Later heading")}));
    }

    void imagesBecomeImageLines()
    {
        QStringList asked;
        ImageImporter images = [&](const QString &path, const QString &alt) -> QString {
            asked << path;
            if (path.endsWith(QLatin1String("missing.png")))
                return {};
            return attachments::imageMarkdown(hashFor(path), alt);
        };
        const QString md = QStringLiteral(
            "![Cat](img/cat.png)\n\n"
            "Before ![dog](dog%20pic.png \"title\") after\n\n"
            "See ![x](https://example.com/a.png) here\n\n"
            "Gone ![m](missing.png)\n\n"
            "![](data:image/png;base64,AAAA)\n\n"
            "- ![Cat](img/cat.png)\n");
        const ImportedDoc d = fromMarkdown(md, QStringLiteral("t"), QStringLiteral("/base"), images);
        const QString cat = attachments::imageMarkdown(hashFor(QStringLiteral("cat.png")), QStringLiteral("Cat"));
        const QString dog = attachments::imageMarkdown(hashFor(QStringLiteral("dog pic.png")), QStringLiteral("dog"));
        QCOMPARE(texts(d), QStringList({cat, QStringLiteral("Before"), dog, QStringLiteral("after"),
                                        QStringLiteral("See [x](https://example.com/a.png) here"),
                                        QStringLiteral("Gone [m](missing.png)"), cat}));
        // Relative to the document, percent-decoded; remote and data: never asked.
        QCOMPARE(asked.value(0), QStringLiteral("/base/img/cat.png"));
        QCOMPARE(asked.value(1), QStringLiteral("/base/dog pic.png"));
        QVERIFY(!asked.join(QLatin1Char(' ')).contains(QLatin1String("example.com")));
        QCOMPARE(d.notes.size(), 1); // the data: image
        // Without an importer, images stay links.
        const ImportedDoc plain = fromMarkdown(QStringLiteral("![Cat](img/cat.png)"), QStringLiteral("t"), QString(), {});
        QCOMPARE(texts(plain), QStringList({QStringLiteral("[Cat](img/cat.png)")}));
    }

    void decodingAndLineEnds()
    {
        QCOMPARE(decodeText(QByteArray("a\r\nb\rc\n")), QStringLiteral("a\nb\nc\n"));
        QCOMPARE(decodeText(QByteArray("\xEF\xBB\xBF" "caf\xC3\xA9")), QStringLiteral("café"));
        QCOMPARE(decodeText(QByteArray("caf\xE9")), QStringLiteral("café")); // not UTF-8: Latin-1
        QByteArray le("\xFF\xFE", 2);
        for (QChar c : QStringLiteral("hié"))
            le.append(char(c.unicode() & 0xff)).append(char(c.unicode() >> 8));
        QCOMPARE(decodeText(le), QStringLiteral("hié"));
        // Characters the editor gives a meaning to are dropped.
        QCOMPARE(decodeText(QStringLiteral("a￼b").toUtf8()), QStringLiteral("ab"));
    }

    void plainTextKeepsLines()
    {
        const ImportedDoc d = fromPlainText(QStringLiteral("one  \n\n  two\n```\n"), QStringLiteral("t"));
        QCOMPARE(texts(d), QStringList({QStringLiteral("one"), QStringLiteral("  two"), QStringLiteral("```")}));
    }

    void limitsAndCancellation()
    {
        Limits small;
        small.maxOutputChars = 20;
        const ImportedDoc d = fromMarkdown(QStringLiteral("aaaaaaaaaa\n\nbbbbbbbbbb\n\ncccccccccc\n"),
                                           QStringLiteral("t"), QString(), {}, nullptr, small);
        QVERIFY(d.truncated);
        QCOMPARE(d.blocks.size(), 2);
        QCOMPARE(d.notes.size(), 1);

        std::atomic<bool> cancel{true};
        QVERIFY(!fromMarkdown(QStringLiteral("x\n"), QStringLiteral("t"), QString(), {}, &cancel).error.isEmpty());

        QTemporaryDir dir;
        const QString big = dir.filePath(QStringLiteral("big.md"));
        {
            QFile f(big);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(2048, 'x'));
        }
        Limits tiny;
        tiny.maxInputBytes = 1024;
        const ImportedDoc refused = importTextFile(big, {}, nullptr, tiny);
        QVERIFY(!refused.error.isEmpty());
        QVERIFY(refused.blocks.isEmpty());
        const ImportedDoc ok = importTextFile(big, {});
        QVERIFY(ok.error.isEmpty());
        QCOMPARE(ok.title, QStringLiteral("big"));
        QCOMPARE(ok.blocks.size(), 1);
    }

    // ------------------------------------------------------------ Word ----
    void zipReaderStreamsEntries()
    {
        QTemporaryDir dir;
        const QByteArray big(300000, 'z');
        for (bool deflate : {false, true}) {
            const QString p = writeFile(dir, deflate ? QStringLiteral("d.zip") : QStringLiteral("s.zip"),
                                        makeZip({{QStringLiteral("a/One.txt"), "hello"}, {QStringLiteral("big"), big}}, deflate));
            ZipReader z;
            QString err;
            QVERIFY2(z.open(p, &err), qPrintable(err));
            QCOMPARE(z.entryCount(), 2);
            QCOMPARE(z.find(QStringLiteral("A/one.TXT")), 0); // part names are case-insensitive
            QCOMPARE(z.readAll(0, 1024), QByteArray("hello"));
            QCOMPARE(z.readAll(z.find(QStringLiteral("big")), 1 << 20), big);
            // Past the limit: an error, counted on what was really inflated.
            QVERIFY(z.readAll(1, 1000, &err).isEmpty());
            QVERIFY2(err.contains(QLatin1String("larger")), qPrintable(err));
        }
    }

    void wordMapsOntoPageLines()
    {
        const QByteArray field = "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> HYPERLINK "
                                 "\"https://f.example\" </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
            + run("field link") + "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
        const QByteArray body =
            para("Titel", run("The Title")) + para("berschrift1", run("Intro")) + para("H4", run("Deep"))
            + para("MyHeading", run("Inherited"))
            + para({}, run("Plain ") + run("bold", "<w:b/>") + run(" and ") + run("ital", "<w:i/>") + run(" text ")
                           + run("both", "<w:b/><w:i/>"))
            + para({}, run("mid") + run("word", "<w:i/>") + run("italic"))
            + para({}, run("Strong style", "<w:rStyle w:val=\"Strong\"/>"))
            + listPara(1, 0, "apple") + listPara(1, 1, "pip") + listPara(2, 0, "three") + listPara(2, 0, "four")
            + para({}, "<w:hyperlink r:id=\"rIdL\">" + run("a link") + "</w:hyperlink>" + run(" and ") + field)
            + para({}, run("kept") + "<w:del><w:r><w:delText>gone</w:delText></w:r></w:del><w:ins>" + run(" added") + "</w:ins>")
            + para({}, "<mc:AlternateContent><mc:Choice Requires=\"wps\">" + run("choice") + "</mc:Choice><mc:Fallback>"
                           + run("fallback") + "</mc:Fallback></mc:AlternateContent>")
            + para("Zitat", run("A quote")) + para({}, run("costs $5 and $10, a * b * c"))
            + para({}, run("line one") + "<w:r><w:br/></w:r>" + run("# not a heading") + "<w:r><w:br w:type=\"page\"/></w:r>")
            + para({}, run("before ") + image("rIdI", "Red box") + run(" after")) + para({}, image("rIdE", ""))
            + para({}, image("rIdI", "Again"))
            + "<w:tbl><w:tr><w:tc>" + para({}, run("h1")) + "</w:tc><w:tc>" + para({}, run("h|2"))
            + "</w:tc></w:tr><w:tr><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr>" + para({}, run("wide"))
            + "</w:tc></w:tr></w:tbl>"
            + para({}, run("outer") + "<w:r><w:drawing><wp:anchor><a:graphic><a:graphicData><wps:wsp><wps:txbx><w:txbxContent>"
                           + para({}, run("in a box")) + "</w:txbxContent></wps:txbx></wps:wsp></a:graphicData></a:graphic>"
                                                         "</wp:anchor></w:drawing></w:r>")
            + para({}, "<m:oMath><m:r><m:t>x=1</m:t></m:r></m:oMath>") + para({}, {});
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("Report.docx"), makeZip(docxParts(body)));
        int calls = 0;
        QString pngHash;
        DocxImageImporter images = [&](QIODevice *data, const QString &name, const QString &alt) -> QString {
            ++calls;
            const QByteArray bytes = data->readAll();
            if (QImage::fromData(bytes).isNull())
                return {};
            pngHash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
            Q_UNUSED(name);
            return attachments::imageMarkdown(pngHash, alt);
        };
        const ImportedDoc d = importDocx(path, images);
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        QCOMPARE(d.title, QStringLiteral("The Title"));
        QCOMPARE(calls, 1); // the PNG once (the EMF is not a picture Folia stores; the repeat is reused)
        const QString img = attachments::imageMarkdown(pngHash, QStringLiteral("Red box"));
        const QStringList expected = {
            QStringLiteral("# Intro"),
            QStringLiteral("### Deep"),
            QStringLiteral("# Inherited"),
            QStringLiteral("Plain **bold** and *ital* text **both**"),
            QStringLiteral("midworditalic"),
            QStringLiteral("**Strong style**"),
            QStringLiteral("- apple"),
            QStringLiteral("  - pip"),
            QStringLiteral("3. three"),
            QStringLiteral("4. four"),
            QStringLiteral("[a link](https://example.com/x) and [field link](https://f.example)"),
            QStringLiteral("kept added"),
            QStringLiteral("choice"),
            QStringLiteral("> A quote"),
            QStringLiteral("costs \\$5 and \\$10, a \\* b \\* c"),
            QStringLiteral("line one"),
            QStringLiteral("\\# not a heading"),
            QStringLiteral("before"),
            img,
            QStringLiteral("after"),
            img,
            QStringLiteral("| h1 | h\\|2 |"),
            QStringLiteral("|---|---|"),
            QStringLiteral("| wide |  |"),
            QStringLiteral("outer"),
            QStringLiteral("in a box"),
            QStringLiteral("x=1"),
        };
        QCOMPARE(texts(d), expected);
        QCOMPARE(d.notes.size(), 1); // the EMF
        // Without a Title paragraph, the document's own title is used.
        const QString plain = writeFile(dir, QStringLiteral("plain.docx"), makeZip(docxParts(para({}, run("x")))));
        QCOMPARE(importDocx(plain, images).title, QStringLiteral("Core title"));
    }

    // Files written without styles or numbering (TextEdit's "Word" format):
    // large bold lines are headings, typed bullets and numbers are lists.
    void wordWithoutStyles()
    {
        auto parts = docxParts(
            para({}, run("Big title", "<w:b/><w:sz w:val=\"48\"/>"))
            + para({}, run("Section", "<w:b/><w:sz w:val=\"36\"/>"))
            + para({}, run("Bold body text at body size", "<w:b/><w:sz w:val=\"24\"/>"))
            + para({}, "<w:r><w:t></w:t><w:tab/><w:t>\xE2\x80\xA2</w:t><w:tab/></w:r>" + run("first"))
            + para({}, "<w:r><w:tab/><w:t>\xE2\x97\xA6</w:t><w:tab/></w:r>" + run("nested"))
            + para({}, "<w:r><w:tab/><w:t>2</w:t><w:tab/></w:r>" + run("second step"))
            + para({}, run("a") + "<w:r><w:tab/></w:r>" + run("b")));
        parts.removeAt(5); // no core properties: the title comes from the text
        QTemporaryDir dir;
        const ImportedDoc d = importDocx(writeFile(dir, QStringLiteral("t.docx"), makeZip(parts)), {});
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
        QCOMPARE(d.title, QStringLiteral("Big title"));
        QCOMPARE(texts(d), QStringList({QStringLiteral("## Section"), QStringLiteral("**Bold body text at body size**"),
                                        QStringLiteral("- first"), QStringLiteral("  - nested"),
                                        QStringLiteral("2. second step"), QStringLiteral("a b")}));
    }

    void wordRefusesHostileFiles()
    {
        QTemporaryDir dir;
        QString err;
        // An old .doc (or an encrypted .docx: also a compound file).
        QByteArray cfb("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
        cfb.append(QByteArray(512, '\0'));
        err = importDocx(writeFile(dir, QStringLiteral("old.docx"), cfb), {}).error;
        QVERIFY2(err.contains(QLatin1String(".doc")), qPrintable(err));
        // A truncated archive.
        const QByteArray zip = makeZip(docxParts(para({}, run("x"))));
        err = importDocx(writeFile(dir, QStringLiteral("cut.docx"), zip.left(zip.size() / 2)), {}).error;
        QVERIFY(!err.isEmpty());
        // A DTD (entity expansion tricks are never read).
        auto parts = docxParts({});
        parts.last().second = "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;\">]>"
                              "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                              "<w:body><w:p><w:r><w:t>&lol2;</w:t></w:r></w:p></w:body></w:document>";
        err = importDocx(writeFile(dir, QStringLiteral("dtd.docx"), makeZip(parts)), {}).error;
        QVERIFY2(err.contains(QLatin1String("DTD")), qPrintable(err));
        // A zip bomb: the limit counts inflated bytes.
        QByteArray huge;
        for (int i = 0; i < 40000; ++i)
            huge += para({}, run("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        const QString bomb = writeFile(dir, QStringLiteral("bomb.docx"), makeZip(docxParts(huge)));
        QVERIFY(QFileInfo(bomb).size() < (1 << 20));
        DocxLimits small;
        small.maxDocumentXml = 1 << 20;
        err = importDocx(bomb, {}, nullptr, {}, small).error;
        QVERIFY2(err.contains(QLatin1String("larger")), qPrintable(err));
        // Too many parts.
        QList<QPair<QString, QByteArray>> many;
        for (int i = 0; i <= ZipReader::kMaxEntries; ++i)
            many.push_back({QStringLiteral("p%1").arg(i), QByteArray()});
        err = importDocx(writeFile(dir, QStringLiteral("many.docx"), makeZip(many, false)), {}).error;
        QVERIFY2(err.contains(QLatin1String("more than")), qPrintable(err));
        // A zip that is not a Word document.
        err = importDocx(writeFile(dir, QStringLiteral("other.docx"), makeZip({{QStringLiteral("a.txt"), "x"}})), {}).error;
        QVERIFY2(err.contains(QLatin1String("not a Word")), qPrintable(err));
        // Cancelled.
        std::atomic<bool> cancel{true};
        QCOMPARE(importDocx(bomb, {}, &cancel).error, QStringLiteral("cancelled"));
    }
};

QTEST_MAIN(ImportTests)
#include "tst_import.moc"
