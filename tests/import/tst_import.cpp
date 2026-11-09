// Document import (docs/ATTACHMENTS.md §8): Markdown and text conversion,
// decoding, limits and cancellation.

#include "import/MarkdownImport.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace folia;
using namespace folia::import;

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
};

QTEST_GUILESS_MAIN(ImportTests)
#include "tst_import.moc"
