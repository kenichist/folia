// Web links in page text.
//
//  * Named links ("Text" + "Link", like Google Docs) are stored as Markdown
//    `[label](url)` in a paragraph's source. In the editor the label is
//    ordinary text carrying an anchor char format (QTextCharFormat::
//    isAnchor/anchorHref), so the caret moves through it like any word and
//    only the label is shown. PageSession converts both ways at load/save.
//  * Bare URLs (https://…, www.…, mailto:…) stay plain source text and are
//    recognized at layout time.
#pragma once

#include <QString>
#include <QStringView>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QVector>

namespace folia::links {

struct Span {
    int start = 0;   // in the label-only (display) text
    int length = 0;
    QString href;
};

// Splits one stored line into display text (labels only) and link spans.
// Inline code spans, inline math and backslash-escaped brackets are left
// untouched. Returns `line` unchanged (and no spans) when there is no link.
QString parseMarkdown(const QString &line, QVector<Span> *spans);
// Cheap pre-check: false when `line` cannot contain `[label](url)`.
bool mayContainLink(QStringView line);
// One `[label](url)` starting at line[i] == '['; `end` is one past ')'.
bool parseLink(const QString &line, int i, QString *label, QString *href, int *end);

// Markdown source for [from, to) of a block (positions relative to the
// block): anchored runs become `[label](url)`, everything else is copied.
QString blockSource(const QTextBlock &b, int from = 0, int to = -1);
bool blockHasLinks(const QTextBlock &b);

QString toMarkdown(const QString &label, const QString &href);

// The char format used for a named link's label.
QTextCharFormat anchorFormat(const QString &href);
inline bool isLink(const QTextCharFormat &f) { return f.isAnchor() && !f.anchorHref().isEmpty(); }

// Length of a bare URL starting at text[i], or 0.
int bareUrlLength(QStringView text, int i);
// The address a bare URL opens ("www.x.org" -> "https://www.x.org").
QString bareUrlHref(QStringView url);

// User input from the link editor -> an openable address ("" if empty).
QString normalizeUrl(const QString &input);
// A single token that is clearly an address (used when pasting onto a selection).
bool looksLikeUrl(const QString &text);

} // namespace folia::links
