// Word (.docx) import (docs/ATTACHMENTS.md §8). Runs on the import worker.
//
// A .docx is a zip of XML parts. The relationships, styles and numbering
// parts are read first (small), then word/document.xml is streamed through
// QXmlStreamReader straight from the inflater (ZipReader), so memory follows
// the output, not the document. The page gets what the editor renders:
// Title / Heading 1–3 → # .. ###, lists → "- " / "N. ", bold and italic
// runs, external hyperlinks, tables as | rows |, quotes, and images
// (stored as attachments after the text pass, one job each, streamed from
// the archive to disk without decoding). Equations become their text;
// tracked deletions, comments, field codes and page breaks are left out.
#pragma once

#include "import/ImportedDoc.h"

class QIODevice;

namespace folia::import {

struct DocxLimits {
    qint64 maxDocumentXml = 256ll << 20; // word/document.xml, inflated
    qint64 maxSmallPart = 16ll << 20;    // styles, numbering, core properties
    qint64 maxRels = 4ll << 20;
    qint64 maxImageBytes = 256ll << 20;  // one image, inflated
    int maxTableColumns = 64;
    int maxCellChars = 2048;
};

// Stores one embedded image read from `data` (a stream inside the archive)
// and returns its attachment line, or an empty string to leave it out.
// `name` is the part's file name ("image1.png").
using DocxImageImporter = std::function<QString(QIODevice *data, const QString &name, const QString &alt)>;

ImportedDoc importDocx(const QString &path, const DocxImageImporter &images, const std::atomic<bool> *cancel = nullptr,
                       const Limits &limits = {}, const DocxLimits &docx = {});

} // namespace folia::import
