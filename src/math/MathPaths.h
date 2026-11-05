// Internal: synthetic outlines (see MathPaths.cpp).
#pragma once

#include <QPainterPath>

namespace folia {
namespace math {
namespace detail {

// Vertical delimiter / radical of total height H spanning y in [-H, 0].
// Returns an empty path for unsupported characters. *advance = width.
QPainterPath syntheticVertical(char32_t cp, qreal H, qreal em, qreal *advance);
// Horizontal brace/paren/bracket (U+23DC..U+23DF, U+23B4/5) of width W.
QPainterPath syntheticHorizontal(char32_t cp, qreal W, qreal em);
// Negation slash centred at (cx, cy).
QPainterPath notSlash(qreal cx, qreal cy, qreal halfHeight, qreal em);

} // namespace detail
} // namespace math
} // namespace folia
