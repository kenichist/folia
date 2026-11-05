#include "math/MathAst.h"

#include <QHash>

namespace folia {
namespace math {
namespace detail {

namespace {

struct SymDef {
    const char *name;
    char32_t cp;
    Cls cls;
    quint8 flags;
};

constexpr Cls O = Cls::Ord, OP = Cls::Op, B = Cls::Bin, R = Cls::Rel, OPN = Cls::Open,
              CL = Cls::Close, P = Cls::Punct, IN = Cls::Inner;
constexpr quint8 L = kSymLetter, BIG = kSymBigOp, INT = kSymIntegral;

const SymDef kSymbols[] = {
    // Greek lowercase (italic by default)
    {"alpha", 0x3B1, O, L}, {"beta", 0x3B2, O, L}, {"gamma", 0x3B3, O, L},
    {"delta", 0x3B4, O, L}, {"epsilon", 0x3F5, O, L}, {"varepsilon", 0x3B5, O, L},
    {"zeta", 0x3B6, O, L}, {"eta", 0x3B7, O, L}, {"theta", 0x3B8, O, L},
    {"vartheta", 0x3D1, O, L}, {"iota", 0x3B9, O, L}, {"kappa", 0x3BA, O, L},
    {"varkappa", 0x3F0, O, L}, {"lambda", 0x3BB, O, L}, {"mu", 0x3BC, O, L},
    {"nu", 0x3BD, O, L}, {"xi", 0x3BE, O, L}, {"omicron", 0x3BF, O, L},
    {"pi", 0x3C0, O, L}, {"varpi", 0x3D6, O, L}, {"rho", 0x3C1, O, L},
    {"varrho", 0x3F1, O, L}, {"sigma", 0x3C3, O, L}, {"varsigma", 0x3C2, O, L},
    {"tau", 0x3C4, O, L}, {"upsilon", 0x3C5, O, L}, {"phi", 0x3D5, O, L},
    {"varphi", 0x3C6, O, L}, {"chi", 0x3C7, O, L}, {"psi", 0x3C8, O, L},
    {"omega", 0x3C9, O, L},
    // Greek uppercase (upright by default)
    {"Alpha", 0x391, O, L}, {"Beta", 0x392, O, L}, {"Gamma", 0x393, O, L},
    {"Delta", 0x394, O, L}, {"Epsilon", 0x395, O, L}, {"Zeta", 0x396, O, L},
    {"Eta", 0x397, O, L}, {"Theta", 0x398, O, L}, {"Iota", 0x399, O, L},
    {"Kappa", 0x39A, O, L}, {"Lambda", 0x39B, O, L}, {"Mu", 0x39C, O, L},
    {"Nu", 0x39D, O, L}, {"Xi", 0x39E, O, L}, {"Omicron", 0x39F, O, L},
    {"Pi", 0x3A0, O, L}, {"Rho", 0x3A1, O, L}, {"Sigma", 0x3A3, O, L},
    {"Tau", 0x3A4, O, L}, {"Upsilon", 0x3A5, O, L}, {"Phi", 0x3A6, O, L},
    {"Chi", 0x3A7, O, L}, {"Psi", 0x3A8, O, L}, {"Omega", 0x3A9, O, L},
    // misc ordinary
    {"infty", 0x221E, O, 0}, {"partial", 0x2202, O, L}, {"nabla", 0x2207, O, 0},
    {"forall", 0x2200, O, 0}, {"exists", 0x2203, O, 0}, {"nexists", 0x2204, O, 0},
    {"emptyset", 0x2205, O, 0}, {"varnothing", 0x2300, O, 0}, {"neg", 0xAC, O, 0},
    {"lnot", 0xAC, O, 0}, {"top", 0x22A4, O, 0}, {"bot", 0x22A5, O, 0},
    {"angle", 0x2220, O, 0}, {"measuredangle", 0x2221, O, 0}, {"triangle", 0x25B3, O, 0},
    {"hbar", 0x210F, O, 0}, {"hslash", 0x210F, O, 0}, {"ell", 0x2113, O, 0},
    {"wp", 0x2118, O, 0}, {"Re", 0x211C, O, 0}, {"Im", 0x2111, O, 0},
    {"aleph", 0x2135, O, 0}, {"beth", 0x2136, O, 0}, {"gimel", 0x2137, O, 0},
    {"prime", 0x2032, O, 0}, {"backprime", 0x2035, O, 0}, {"flat", 0x266D, O, 0},
    {"natural", 0x266E, O, 0}, {"sharp", 0x266F, O, 0}, {"clubsuit", 0x2663, O, 0},
    {"diamondsuit", 0x2662, O, 0}, {"heartsuit", 0x2661, O, 0}, {"spadesuit", 0x2660, O, 0},
    {"checkmark", 0x2713, O, 0}, {"S", 0xA7, O, 0}, {"P", 0xB6, O, 0},
    {"degree", 0xB0, O, 0}, {"imath", 0x131, O, L}, {"jmath", 0x237, O, L},
    {"Box", 0x25A1, O, 0}, {"square", 0x25A1, O, 0}, {"blacksquare", 0x25A0, O, 0},
    {"lozenge", 0x25CA, O, 0}, {"surd", 0x221A, O, 0}, {"mho", 0x2127, O, 0},
    {"complement", 0x2201, O, 0}, {"vdots", 0x22EE, O, 0}, {"dag", 0x2020, O, 0},
    {"ddag", 0x2021, O, 0}, {"backslash", '\\', O, 0}, {"vert", '|', O, 0},
    {"Vert", 0x2016, O, 0}, {"|", 0x2016, O, 0}, {"#", '#', O, 0}, {"$", '$', O, 0},
    {"%", '%', O, 0}, {"&", '&', O, 0}, {"_", '_', O, 0}, {"infin", 0x221E, O, 0},
    {"ldots", 0x2026, IN, 0}, {"dots", 0x2026, IN, 0}, {"dotsc", 0x2026, IN, 0},
    {"dotso", 0x2026, IN, 0}, {"cdots", 0x22EF, IN, 0}, {"dotsb", 0x22EF, IN, 0},
    {"dotsm", 0x22EF, IN, 0}, {"dotsi", 0x22EF, IN, 0}, {"ddots", 0x22F1, IN, 0},
    {"cdotp", 0xB7, P, 0}, {"ldotp", '.', P, 0}, {"colon", ':', P, 0},
    // delimiters
    {"{", '{', OPN, 0}, {"}", '}', CL, 0}, {"lbrace", '{', OPN, 0}, {"rbrace", '}', CL, 0},
    {"lbrack", '[', OPN, 0}, {"rbrack", ']', CL, 0}, {"langle", 0x27E8, OPN, 0},
    {"rangle", 0x27E9, CL, 0}, {"lfloor", 0x230A, OPN, 0}, {"rfloor", 0x230B, CL, 0},
    {"lceil", 0x2308, OPN, 0}, {"rceil", 0x2309, CL, 0}, {"lvert", '|', OPN, 0},
    {"rvert", '|', CL, 0}, {"lVert", 0x2016, OPN, 0}, {"rVert", 0x2016, CL, 0},
    {"llbracket", 0x27E6, OPN, 0}, {"rrbracket", 0x27E7, CL, 0},
    // binary operators
    {"pm", 0xB1, B, 0}, {"mp", 0x2213, B, 0}, {"times", 0xD7, B, 0}, {"div", 0xF7, B, 0},
    {"cdot", 0x22C5, B, 0}, {"ast", 0x2217, B, 0}, {"star", 0x22C6, B, 0},
    {"circ", 0x2218, B, 0}, {"bullet", 0x2219, B, 0}, {"oplus", 0x2295, B, 0},
    {"ominus", 0x2296, B, 0}, {"otimes", 0x2297, B, 0}, {"oslash", 0x2298, B, 0},
    {"odot", 0x2299, B, 0}, {"cup", 0x222A, B, 0}, {"cap", 0x2229, B, 0},
    {"sqcup", 0x2294, B, 0}, {"sqcap", 0x2293, B, 0}, {"vee", 0x2228, B, 0},
    {"lor", 0x2228, B, 0}, {"wedge", 0x2227, B, 0}, {"land", 0x2227, B, 0},
    {"setminus", 0x2216, B, 0}, {"smallsetminus", 0x2216, B, 0}, {"wr", 0x2240, B, 0},
    {"diamond", 0x22C4, B, 0}, {"bigtriangleup", 0x25B3, B, 0},
    {"bigtriangledown", 0x25BD, B, 0}, {"triangleleft", 0x25C1, B, 0},
    {"triangleright", 0x25B7, B, 0}, {"uplus", 0x228E, B, 0}, {"amalg", 0x2A3F, B, 0},
    {"dagger", 0x2020, B, 0}, {"ddagger", 0x2021, B, 0}, {"ltimes", 0x22C9, B, 0},
    {"rtimes", 0x22CA, B, 0}, {"boxplus", 0x229E, B, 0}, {"boxminus", 0x229F, B, 0},
    {"boxtimes", 0x22A0, B, 0}, {"boxdot", 0x22A1, B, 0}, {"bigcirc", 0x25EF, B, 0},
    // relations
    {"leq", 0x2264, R, 0}, {"le", 0x2264, R, 0}, {"geq", 0x2265, R, 0}, {"ge", 0x2265, R, 0},
    {"neq", 0x2260, R, 0}, {"ne", 0x2260, R, 0}, {"equiv", 0x2261, R, 0},
    {"approx", 0x2248, R, 0}, {"approxeq", 0x224A, R, 0}, {"sim", 0x223C, R, 0},
    {"simeq", 0x2243, R, 0}, {"cong", 0x2245, R, 0}, {"propto", 0x221D, R, 0},
    {"ll", 0x226A, R, 0}, {"gg", 0x226B, R, 0}, {"prec", 0x227A, R, 0},
    {"succ", 0x227B, R, 0}, {"preceq", 0x2AAF, R, 0}, {"succeq", 0x2AB0, R, 0},
    {"subset", 0x2282, R, 0}, {"supset", 0x2283, R, 0}, {"subseteq", 0x2286, R, 0},
    {"supseteq", 0x2287, R, 0}, {"subsetneq", 0x228A, R, 0}, {"supsetneq", 0x228B, R, 0},
    {"nsubseteq", 0x2288, R, 0}, {"nsupseteq", 0x2289, R, 0}, {"sqsubset", 0x228F, R, 0},
    {"sqsupset", 0x2290, R, 0}, {"sqsubseteq", 0x2291, R, 0}, {"sqsupseteq", 0x2292, R, 0},
    {"in", 0x2208, R, 0}, {"notin", 0x2209, R, 0}, {"ni", 0x220B, R, 0},
    {"owns", 0x220B, R, 0}, {"mid", 0x2223, R, 0}, {"nmid", 0x2224, R, 0},
    {"parallel", 0x2225, R, 0}, {"nparallel", 0x2226, R, 0}, {"perp", 0x27C2, R, 0},
    {"vdash", 0x22A2, R, 0}, {"dashv", 0x22A3, R, 0}, {"models", 0x22A8, R, 0},
    {"vDash", 0x22A8, R, 0}, {"Vdash", 0x22A9, R, 0}, {"asymp", 0x224D, R, 0},
    {"doteq", 0x2250, R, 0}, {"bowtie", 0x22C8, R, 0}, {"smile", 0x2323, R, 0},
    {"frown", 0x2322, R, 0}, {"coloneqq", 0x2254, R, 0}, {"eqqcolon", 0x2255, R, 0},
    {"triangleq", 0x225C, R, 0}, {"lesssim", 0x2272, R, 0}, {"gtrsim", 0x2273, R, 0},
    {"leqslant", 0x2A7D, R, 0}, {"geqslant", 0x2A7E, R, 0}, {"nleq", 0x2270, R, 0},
    {"ngeq", 0x2271, R, 0}, {"nless", 0x226E, R, 0}, {"ngtr", 0x226F, R, 0},
    {"nsim", 0x2241, R, 0}, {"ncong", 0x2247, R, 0}, {"lessgtr", 0x2276, R, 0},
    {"gtrless", 0x2277, R, 0}, {"leqq", 0x2266, R, 0}, {"geqq", 0x2267, R, 0},
    {"lll", 0x22D8, R, 0}, {"ggg", 0x22D9, R, 0}, {"thicksim", 0x223C, R, 0},
    {"thickapprox", 0x2248, R, 0}, {"backsim", 0x223D, R, 0}, {"eqsim", 0x2242, R, 0},
    {"Join", 0x2A1D, R, 0},
    // arrows (relations)
    {"to", 0x2192, R, 0}, {"rightarrow", 0x2192, R, 0}, {"gets", 0x2190, R, 0},
    {"leftarrow", 0x2190, R, 0}, {"leftrightarrow", 0x2194, R, 0},
    {"Rightarrow", 0x21D2, R, 0}, {"Leftarrow", 0x21D0, R, 0},
    {"Leftrightarrow", 0x21D4, R, 0}, {"mapsto", 0x21A6, R, 0},
    {"longmapsto", 0x27FC, R, 0}, {"longrightarrow", 0x27F6, R, 0},
    {"longleftarrow", 0x27F5, R, 0}, {"longleftrightarrow", 0x27F7, R, 0},
    {"Longrightarrow", 0x27F9, R, 0}, {"Longleftarrow", 0x27F8, R, 0},
    {"Longleftrightarrow", 0x27FA, R, 0}, {"implies", 0x27F9, R, 0},
    {"impliedby", 0x27F8, R, 0}, {"iff", 0x27FA, R, 0}, {"uparrow", 0x2191, R, 0},
    {"downarrow", 0x2193, R, 0}, {"updownarrow", 0x2195, R, 0}, {"Uparrow", 0x21D1, R, 0},
    {"Downarrow", 0x21D3, R, 0}, {"Updownarrow", 0x21D5, R, 0}, {"nearrow", 0x2197, R, 0},
    {"searrow", 0x2198, R, 0}, {"swarrow", 0x2199, R, 0}, {"nwarrow", 0x2196, R, 0},
    {"hookrightarrow", 0x21AA, R, 0}, {"hookleftarrow", 0x21A9, R, 0},
    {"rightharpoonup", 0x21C0, R, 0}, {"rightharpoondown", 0x21C1, R, 0},
    {"leftharpoonup", 0x21BC, R, 0}, {"leftharpoondown", 0x21BD, R, 0},
    {"rightleftharpoons", 0x21CC, R, 0}, {"leftrightharpoons", 0x21CB, R, 0},
    {"leadsto", 0x21DD, R, 0}, {"rightsquigarrow", 0x21DD, R, 0},
    {"twoheadrightarrow", 0x21A0, R, 0}, {"twoheadleftarrow", 0x219E, R, 0},
    {"nrightarrow", 0x219B, R, 0}, {"nleftarrow", 0x219A, R, 0},
    {"nRightarrow", 0x21CF, R, 0}, {"nLeftarrow", 0x21CD, R, 0},
    {"nLeftrightarrow", 0x21CE, R, 0}, {"circlearrowleft", 0x21BA, R, 0},
    {"circlearrowright", 0x21BB, R, 0}, {"rightleftarrows", 0x21C4, R, 0},
    {"leftrightarrows", 0x21C6, R, 0}, {"upharpoonright", 0x21BE, R, 0},
    // big operators
    {"sum", 0x2211, OP, BIG}, {"prod", 0x220F, OP, BIG}, {"coprod", 0x2210, OP, BIG},
    {"bigcup", 0x22C3, OP, BIG}, {"bigcap", 0x22C2, OP, BIG}, {"bigvee", 0x22C1, OP, BIG},
    {"bigwedge", 0x22C0, OP, BIG}, {"bigoplus", 0x2A01, OP, BIG},
    {"bigotimes", 0x2A02, OP, BIG}, {"bigodot", 0x2A00, OP, BIG},
    {"biguplus", 0x2A04, OP, BIG}, {"bigsqcup", 0x2A06, OP, BIG},
    {"int", 0x222B, OP, INT}, {"intop", 0x222B, OP, INT}, {"iint", 0x222C, OP, INT},
    {"iiint", 0x222D, OP, INT}, {"iiiint", 0x2A0C, OP, INT}, {"oint", 0x222E, OP, INT},
    {"oiint", 0x222F, OP, INT}, {"oiiint", 0x2230, OP, INT}, {"smallint", 0x222B, OP, 0},
};

const FunctionInfo kFunctions[] = {
    {"arccos", false}, {"arcsin", false}, {"arctan", false}, {"arg", false},
    {"cos", false}, {"cosh", false}, {"cot", false}, {"coth", false}, {"csc", false},
    {"deg", false}, {"dim", false}, {"exp", false}, {"hom", false}, {"ker", false},
    {"lg", false}, {"ln", false}, {"log", false}, {"sec", false}, {"sin", false},
    {"sinh", false}, {"tan", false}, {"tanh", false}, {"sgn", false}, {"tr", false},
    {"det", true}, {"gcd", true}, {"inf", true}, {"lim", true}, {"max", true},
    {"min", true}, {"Pr", true}, {"sup", true}, {"argmax", true}, {"argmin", true},
};

struct Tables {
    QHash<QString, SymbolInfo> symbols;
    QHash<char32_t, SymbolInfo> byChar;
    QHash<QString, FunctionInfo> functions;
    Tables()
    {
        for (const SymDef &d : kSymbols) {
            SymbolInfo si{d.cp, d.cls, d.flags};
            symbols.insert(QString::fromLatin1(d.name), si);
            if (!byChar.contains(d.cp) && d.cp > 0x7F)
                byChar.insert(d.cp, si);
        }
        // liminf / limsup / argmax print with a thin space or as one word.
        for (const FunctionInfo &f : kFunctions)
            functions.insert(QString::fromLatin1(f.text), f);
        functions.insert(QStringLiteral("liminf"), FunctionInfo{"lim inf", true});
        functions.insert(QStringLiteral("limsup"), FunctionInfo{"lim sup", true});
        functions.insert(QStringLiteral("injlim"), FunctionInfo{"inj lim", true});
        functions.insert(QStringLiteral("projlim"), FunctionInfo{"proj lim", true});
    }
};

const Tables &tables()
{
    static const Tables t; // thread-safe magic static
    return t;
}

} // namespace

const SymbolInfo *lookupSymbol(QStringView name)
{
    const auto &t = tables().symbols;
    auto it = t.constFind(name.toString());
    return it == t.cend() ? nullptr : &it.value();
}

const FunctionInfo *lookupFunction(QStringView name)
{
    const auto &t = tables().functions;
    auto it = t.constFind(name.toString());
    return it == t.cend() ? nullptr : &it.value();
}

SymbolInfo classifyChar(char32_t c)
{
    switch (c) {
    case '+': return {'+', Cls::Bin, 0};
    case '-': return {0x2212, Cls::Bin, 0};
    case '*': return {0x2217, Cls::Bin, 0};
    case '=': return {'=', Cls::Rel, 0};
    case '<': return {'<', Cls::Rel, 0};
    case '>': return {'>', Cls::Rel, 0};
    case ':': return {':', Cls::Rel, 0};
    case ',': return {',', Cls::Punct, 0};
    case ';': return {';', Cls::Punct, 0};
    case '!': return {'!', Cls::Close, 0};
    case '?': return {'?', Cls::Close, 0};
    case '(': return {'(', Cls::Open, 0};
    case '[': return {'[', Cls::Open, 0};
    case ')': return {')', Cls::Close, 0};
    case ']': return {']', Cls::Close, 0};
    default: break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return {c, Cls::Ord, kSymLetter};
    if ((c >= 0x391 && c <= 0x3A9) || (c >= 0x3B1 && c <= 0x3C9) || c == 0x3D1 || c == 0x3D5
        || c == 0x3D6 || c == 0x3F0 || c == 0x3F1 || c == 0x3F5)
        return {c, Cls::Ord, kSymLetter};
    if (c == 0x2212)
        return {c, Cls::Bin, 0};
    const auto &t = tables().byChar;
    auto it = t.constFind(c);
    if (it != t.cend())
        return it.value();
    return {c, Cls::Ord, 0};
}

bool isDelimiterChar(char32_t c)
{
    switch (c) {
    case '(': case ')': case '[': case ']': case '{': case '}': case '|': case '/':
    case '\\': case 0x2016: case 0x27E8: case 0x27E9: case 0x230A: case 0x230B:
    case 0x2308: case 0x2309: case 0x27E6: case 0x27E7: case 0x2191: case 0x2193:
    case 0x2195: case 0x21D1: case 0x21D3: case 0x21D5:
        return true;
    default:
        return false;
    }
}

char32_t negated(char32_t c)
{
    switch (c) {
    case '=': return 0x2260;
    case '<': return 0x226E;
    case '>': return 0x226F;
    case 0x2208: return 0x2209;
    case 0x220B: return 0x220C;
    case 0x2282: return 0x2284;
    case 0x2283: return 0x2285;
    case 0x2286: return 0x2288;
    case 0x2287: return 0x2289;
    case 0x2261: return 0x2262;
    case 0x2264: return 0x2270;
    case 0x2265: return 0x2271;
    case 0x223C: return 0x2241;
    case 0x2248: return 0x2249;
    case 0x2245: return 0x2247;
    case 0x2243: return 0x2244;
    case 0x2223: return 0x2224;
    case 0x2225: return 0x2226;
    case 0x227A: return 0x2280;
    case 0x227B: return 0x2281;
    case 0x2192: return 0x219B;
    case 0x2190: return 0x219A;
    case 0x21D2: return 0x21CF;
    case 0x21D0: return 0x21CD;
    case 0x21D4: return 0x21CE;
    case 0x2203: return 0x2204;
    case 0x22A2: return 0x22AC;
    case 0x22A8: return 0x22AD;
    default: return 0;
    }
}

char32_t mapAlpha(char32_t c, Alpha a)
{
    const bool upper = c >= 'A' && c <= 'Z';
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    if (upper || lower) {
        const int i = upper ? int(c - 'A') : int(c - 'a');
        auto latin = [&](char32_t baseUpper) -> char32_t {
            return baseUpper + (upper ? 0 : 26) + i;
        };
        switch (a) {
        case Alpha::Default:
        case Alpha::It:
            if (c == 'h')
                return 0x210E;
            return latin(0x1D434);
        case Alpha::Rm:
            return c;
        case Alpha::Bf:
            return latin(0x1D400);
        case Alpha::BfIt:
            return latin(0x1D468);
        case Alpha::Sf:
            return latin(0x1D5A0);
        case Alpha::Tt:
            return latin(0x1D670);
        case Alpha::Cal:
            switch (c) {
            case 'B': return 0x212C;
            case 'E': return 0x2130;
            case 'F': return 0x2131;
            case 'H': return 0x210B;
            case 'I': return 0x2110;
            case 'L': return 0x2112;
            case 'M': return 0x2133;
            case 'R': return 0x211B;
            case 'e': return 0x212F;
            case 'g': return 0x210A;
            case 'o': return 0x2134;
            default: return latin(0x1D49C);
            }
        case Alpha::Frak:
            switch (c) {
            case 'C': return 0x212D;
            case 'H': return 0x210C;
            case 'I': return 0x2111;
            case 'R': return 0x211C;
            case 'Z': return 0x2128;
            default: return latin(0x1D504);
            }
        case Alpha::Bb:
            switch (c) {
            case 'C': return 0x2102;
            case 'H': return 0x210D;
            case 'N': return 0x2115;
            case 'P': return 0x2119;
            case 'Q': return 0x211A;
            case 'R': return 0x211D;
            case 'Z': return 0x2124;
            default: return latin(0x1D538);
            }
        }
        return c;
    }
    if (digit) {
        const int d = int(c - '0');
        switch (a) {
        case Alpha::Bf: case Alpha::BfIt: return 0x1D7CE + d;
        case Alpha::Bb: return 0x1D7D8 + d;
        case Alpha::Sf: return 0x1D7E2 + d;
        case Alpha::Tt: return 0x1D7F6 + d;
        default: return c;
        }
    }
    // Greek: index into the 58-slot math Greek block.
    int gi = -1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        gi = int(c - 0x391);
    else if (c == 0x3F4)
        gi = 17;
    else if (c == 0x2207)
        gi = 25;
    else if (c >= 0x3B1 && c <= 0x3C9)
        gi = 26 + int(c - 0x3B1);
    else if (c == 0x2202)
        gi = 51;
    else if (c == 0x3F5)
        gi = 52;
    else if (c == 0x3D1)
        gi = 53;
    else if (c == 0x3F0)
        gi = 54;
    else if (c == 0x3D5)
        gi = 55;
    else if (c == 0x3F1)
        gi = 56;
    else if (c == 0x3D6)
        gi = 57;
    if (gi >= 0) {
        const bool isUpper = gi < 25;
        const bool isNabla = gi == 25;
        switch (a) {
        case Alpha::Default:
            // TeX: lowercase Greek and \partial italic, uppercase upright.
            if (isUpper || isNabla)
                return c;
            return 0x1D6E2 + gi;
        case Alpha::Rm: return c;
        case Alpha::It: return isNabla ? c : 0x1D6E2 + gi;
        case Alpha::Bf: return 0x1D6A8 + gi;
        case Alpha::BfIt: return 0x1D71C + gi;
        case Alpha::Sf: return 0x1D756 + gi; // sans-serif bold (only sans Greek)
        default:
            if (isUpper || isNabla)
                return c;
            return 0x1D6E2 + gi;
        }
    }
    if (c == 0x131 && (a == Alpha::Default || a == Alpha::It))
        return 0x1D6A4;
    if (c == 0x237 && (a == Alpha::Default || a == Alpha::It))
        return 0x1D6A5;
    return c;
}

} // namespace detail
} // namespace math
} // namespace folia
