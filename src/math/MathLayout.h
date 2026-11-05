// Internal: TeX-style layout of a parsed Ast into a MathDisplayList.
#pragma once

#include "math/MathAst.h"
#include "math/MathFont.h"

namespace folia {
namespace math {
namespace detail {

// Returns false (ctx.failed set) on deadline/cancel/size errors.
bool layout(const FontData &fd, const Ast &ast, Ctx &ctx, qreal basePx, bool display,
            MathDisplayList &out);

} // namespace detail
} // namespace math
} // namespace folia
