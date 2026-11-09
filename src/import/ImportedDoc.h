// A document converted into a page (docs/ATTACHMENTS.md §8): what importing a
// Markdown, text or Word file produces, built on the import worker and saved
// as one new page. The text uses only what the editor renders (headings
// #..###, list markers, "> " quotes, **bold**, *italic*, `code`, $math$,
// [text](url)), so an imported page is not rewritten when it is opened.
#pragma once

#include "core/Attachments.h"
#include "core/Types.h"

#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace folia::import {

struct Limits {
    qint64 maxInputBytes = 16ll << 20;  // a Markdown or text file
    qint64 maxOutputChars = 10ll << 20; // text kept in the page (the 10 MiB stress note's size)
    int maxImages = 500;
};

struct ImportedDoc {
    QString title;
    QVector<BlockRecord> blocks;         // in order; ids are assigned when the page is saved
    QVector<AttachmentInfo> attachments; // files the blocks reference (pinned until saved)
    QStringList notes;                   // what was left out; shown as a quote at the top
    QString error;                       // set: nothing was imported
    bool truncated = false;
    qint64 outputChars = 0;
};

// Imports one local image (import worker) and returns its attachment line
// (`![alt](attachment:<hash>)`), or an empty string when it cannot.
using ImageImporter = std::function<QString(const QString &path, const QString &alt)>;

// Appends blocks while keeping to the output limit.
class DocBuilder {
public:
    DocBuilder(ImportedDoc *doc, const Limits &limits) : m_doc(doc), m_limits(limits) {}

    bool full() const { return m_doc->truncated; }
    void text(const QString &line) { add(BlockKind::Text, {}, line); }
    void code(const QString &lang, const QString &body) { add(BlockKind::Code, lang, body); }
    // Display math; `single`: written as one $$…$$ line.
    void math(const QString &tex, bool single)
    {
        add(BlockKind::Math, single ? QStringLiteral("single") : QString(), tex);
    }

private:
    void add(BlockKind kind, const QString &lang, const QString &text)
    {
        if (m_doc->truncated)
            return;
        if (m_doc->outputChars + text.size() > m_limits.maxOutputChars) {
            m_doc->truncated = true;
            m_doc->notes << QStringLiteral("The rest was left out: Folia keeps at most %1 MB of text per imported page.")
                                .arg(m_limits.maxOutputChars >> 20);
            return;
        }
        BlockRecord r;
        r.kind = kind;
        r.lang = lang;
        r.text = text;
        m_doc->outputChars += text.size();
        m_doc->blocks.push_back(std::move(r));
    }

    ImportedDoc *m_doc;
    Limits m_limits;
};

} // namespace folia::import
