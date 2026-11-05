// Lucide icons (https://lucide.dev), drawn natively.
//
// The SVG sources of the icons Folia uses are embedded verbatim and parsed
// on first use into QPainterPaths (24×24 viewBox, round 2-unit stroke, no fill), so
// no SVG module or image files are needed and icons stay crisp at any size
// and pixel ratio. Add an icon by pasting its element markup into the table
// in LucideIcons.cpp.
#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>
#include <QString>
#include <QStringList>

class QPainter;

namespace folia::icons {

bool has(const QString &name);
QStringList names();

// Draws `name` centered in `rect` (scaled to its shorter side) in `color`.
// `strokeWidth` is in icon units (Lucide's default is 2). Returns false for
// an unknown name. Icons are parsed on first use and blitted from a small
// byte-capped pixmap cache (vectors are stroked only for scaled painters).
bool paint(QPainter *p, const QString &name, const QRectF &rect, const QColor &color,
           qreal strokeWidth = 2.0);

// Drops cached icon pixmaps (memory pressure).
void clearCache();

// A resolution-independent QIcon (disabled mode is drawn faded).
QIcon icon(const QString &name, const QColor &color);

} // namespace folia::icons
