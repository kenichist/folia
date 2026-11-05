// Internal: TeX-subset AST, symbol tables and parser.
#pragma once

#include "math/MathRenderer.h"

#include <QElapsedTimer>
#include <QString>
#include <QStringView>
#include <atomic>
#include <vector>

namespace folia {
namespace math {
namespace detail {

enum class Cls : quint8 { Ord, Op, Bin, Rel, Open, Close, Punct, Inner, None };

// Math alphabets (\mathXX); Default = TeX conventions (italic letters).
enum class Alpha : quint8 { Default, Rm, It, Bf, BfIt, Sf, Tt, Bb, Cal, Frak };

enum class NK : quint8 {
    Char,      // cp, alpha
    Text,      // text (already mapped), variant
    List,      // list -> ast.lists
    Frac,      // a=num b=den; flags: kFracNoRule, kFracParens; style: forced style (-1 none)
    Sqrt,      // a=body b=index(-1)
    LeftRight, // list -> ast.lrs
    BigDelim,  // cp, value = size in em
    Accent,    // a=body cp=accent flags: kAccentWide
    Overline,  // a=body
    Underline, // a=body
    Brace,     // a=body cp=brace char (over if 0x23DE.. top), flags: kUnder
    Stack,     // a=base b=over c=under (overset/underset/stackrel)
    Space,     // value (em or mu per flags)
    Style,     // style: 0 D,1 T,2 S,3 SS
    Env,       // list -> ast.envs
    Phantom,   // a=body flags: kPhantomH / kPhantomV / kSmash
    Op,        // cp (big op glyph) or a=body (text/list); limits / limitsDefault
    Not,       // a=body (overlay slash)
    Boxed,     // a=body
    Empty,     // empty nucleus (for "{}^2" / "^2")
};

enum : quint8 {
    kFracNoRule = 1, kFracParens = 2,
    kAccentWide = 1,
    kUnder = 1,
    kPhantomH = 1, kPhantomV = 2, kSmash = 4,
    kSpaceMu = 1, kSpaceEx = 2,
    kOpIntegral = 1,
};

struct Node {
    NK kind = NK::Empty;
    Cls cls = Cls::Ord;
    qint8 limits = -1;        // Op: -1 default, 0 \nolimits, 1 \limits
    bool limitsDefault = false;
    Alpha alpha = Alpha::Default;
    quint8 primes = 0;
    quint8 flags = 0;
    qint8 style = -1;
    int src = 0;
    char32_t cp = 0;
    int a = -1, b = -1, c = -1;
    int sub = -1, sup = -1;
    int list = -1;
    float value = 0;
    int variant = 0;
    QString text;
};

enum class EnvKind : quint8 {
    Matrix, PMatrix, BMatrix, BBMatrix, VMatrix, VVMatrix, SmallMatrix,
    Cases, DCases, RCases, Aligned, Gathered, Array
};

struct EnvData {
    EnvKind kind = EnvKind::Matrix;
    std::vector<std::vector<int>> rows; // cell list-node indices
    std::vector<char> align;            // per column: 'l','c','r' (array)
    std::vector<int> vlines;            // array: number of '|' before column j (size cols+1)
    std::vector<int> hlines;            // \hline count before row i (size rows+1)
    std::vector<float> rowGapEm;        // extra gap after row i (\\[dim])
    int src = 0;
};

struct LeftRightData {
    std::vector<int> segments;          // list nodes
    std::vector<char32_t> delims;       // segments.size()+1, 0 = null delimiter
};

struct Ast {
    std::vector<Node> nodes;
    std::vector<std::vector<int>> lists;
    std::vector<EnvData> envs;
    std::vector<LeftRightData> lrs;
    int root = -1;
};

// Shared render context: limits, deadline, cancellation and first error.
struct Ctx {
    Limits lim;
    int maxDepth = 48;
    QElapsedTimer timer;
    const std::atomic<bool> *cancel = nullptr;
    bool failed = false;
    QString error;
    int offset = -1;
    // Time spent on first-ever glyph measurements (font fallback lookups)
    // is a one-time cost of the process, not of this formula: it is credited
    // against the deadline up to kMaxColdCreditNs.
    qint64 coldNanos = 0;
    static constexpr qint64 kMaxColdCreditNs = 250 * 1000 * 1000;

    bool fail(const QString &msg, int off)
    {
        if (!failed) {
            failed = true;
            error = msg;
            offset = off;
        }
        return false;
    }
    bool check()
    {
        if (failed)
            return false;
        if (cancel && cancel->load(std::memory_order_relaxed))
            return fail(QStringLiteral("cancelled"), -1);
        const qint64 spent = timer.nsecsElapsed() - qMin(coldNanos, kMaxColdCreditNs);
        if (spent > qint64(lim.maxMillis) * 1000000)
            return fail(QStringLiteral("render time limit exceeded (%1 ms)").arg(lim.maxMillis), -1);
        return true;
    }
};

// Parses `src`; on failure ctx.failed is set and the returned root is -1.
int parse(const QString &src, Ast &ast, Ctx &ctx);

// ---- symbol tables (MathSymbols.cpp) --------------------------------------
enum : quint8 { kSymLetter = 1, kSymBigOp = 2, kSymIntegral = 4 };
struct SymbolInfo {
    char32_t cp = 0;
    Cls cls = Cls::Ord;
    quint8 flags = 0;
};
const SymbolInfo *lookupSymbol(QStringView name);
// Class of a literal character typed in math mode.
SymbolInfo classifyChar(char32_t c);
struct FunctionInfo {
    const char *text;
    bool limits;
};
const FunctionInfo *lookupFunction(QStringView name);
// Map a code point through a math alphabet (Unicode Mathematical
// Alphanumeric Symbols). Returns the input when the alphabet has no form.
char32_t mapAlpha(char32_t c, Alpha a);
// Delimiter code point for a symbol code point / literal, 0 if not a delimiter.
bool isDelimiterChar(char32_t c);
// Precomposed negation (\not=) or 0.
char32_t negated(char32_t c);

} // namespace detail
} // namespace math
} // namespace folia
