// Which uploaded files become new pages (docs/ATTACHMENTS.md §8), by suffix
// only: no file is opened to decide.
#pragma once

#include <QFileInfo>
#include <QString>

namespace folia::import {

inline bool isMarkdownFile(const QString &path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("md") || s == QLatin1String("markdown") || s == QLatin1String("txt");
}

inline bool isWordFile(const QString &path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("docx"), Qt::CaseInsensitive) == 0;
}

// Converted into a new page (PDFs and images are attached to a page instead).
inline bool isDocumentFile(const QString &path)
{
    return isMarkdownFile(path) || isWordFile(path);
}

} // namespace folia::import
