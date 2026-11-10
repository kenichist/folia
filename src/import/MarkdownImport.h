// Markdown and plain-text import (docs/ATTACHMENTS.md §8). Runs on the
// import worker. Markdown maps almost one to one onto a page: fenced code
// becomes code blocks, $$ blocks display math, a soft-wrapped paragraph one
// line, headings deeper than ### become ###, local images become image
// attachments and remote ones links (nothing is fetched).
#pragma once

#include "import/ImportedDoc.h"

namespace folia::import {

// BOM (UTF-8, UTF-16 LE/BE) first, else UTF-8 when it decodes cleanly, else
// Latin-1. Line ends become '\n'.
QString decodeText(const QByteArray &bytes);

// `fallbackTitle` is used when the text does not start with a "# " heading
// (which then becomes the title). Relative image paths resolve against
// `baseDir`; `images` may be empty (images stay links).
ImportedDoc fromMarkdown(const QString &text, const QString &fallbackTitle, const QString &baseDir,
                         const ImageImporter &images, const std::atomic<bool> *cancel = nullptr,
                         const Limits &limits = {});

// Plain text: one line per paragraph, nothing interpreted; blank lines go.
ImportedDoc fromPlainText(const QString &text, const QString &title, const std::atomic<bool> *cancel = nullptr,
                          const Limits &limits = {});

// Reads `path` (refusing files over limits.maxInputBytes), decodes it and
// converts it: .txt as plain text, anything else as Markdown.
ImportedDoc importTextFile(const QString &path, const ImageImporter &images, const std::atomic<bool> *cancel = nullptr,
                           const Limits &limits = {});

} // namespace folia::import
