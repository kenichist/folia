// Recursive-descent parser for the declared TeX subset (docs/MATH_SUBSET.md).
// Recursion depth is bounded by Ctx::maxDepth (checked in parseList and
// parseAtom, the only two recursion entry points), node count by
// Limits::maxNodes; the deadline/cancel flag is polled once per atom.
#include "math/MathAst.h"
#include "math/MathFont.h"

#include <cmath>

namespace folia {
namespace math {
namespace detail {

namespace {

enum Stop : unsigned {
    StopBrace = 1, StopAmp = 2, StopRow = 4, StopEnd = 8, StopRight = 16, StopBracket = 32
};

struct Tok {
    enum T { End, Char, Cmd, LBrace, RBrace, Sup, Sub, Amp, Prime, Tilde, Hash, Dollar } t = End;
    char32_t ch = 0;
    int start = 0, end = 0;
    QStringView name;
    bool is(const char *n) const { return t == Cmd && name == QLatin1String(n); }
};

inline bool isLetter(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

struct EnvSpec {
    const char *name;
    EnvKind kind;
};
const EnvSpec kEnvs[] = {
    {"matrix", EnvKind::Matrix}, {"pmatrix", EnvKind::PMatrix}, {"bmatrix", EnvKind::BMatrix},
    {"Bmatrix", EnvKind::BBMatrix}, {"vmatrix", EnvKind::VMatrix}, {"Vmatrix", EnvKind::VVMatrix},
    {"smallmatrix", EnvKind::SmallMatrix}, {"cases", EnvKind::Cases},
    {"dcases", EnvKind::DCases}, {"rcases", EnvKind::RCases}, {"aligned", EnvKind::Aligned},
    {"align", EnvKind::Aligned}, {"align*", EnvKind::Aligned}, {"split", EnvKind::Aligned},
    {"alignat", EnvKind::Aligned}, {"alignat*", EnvKind::Aligned}, {"alignedat", EnvKind::Aligned},
    {"gathered", EnvKind::Gathered}, {"gather", EnvKind::Gathered},
    {"gather*", EnvKind::Gathered}, {"equation", EnvKind::Gathered},
    {"equation*", EnvKind::Gathered}, {"array", EnvKind::Array}, {"darray", EnvKind::Array},
};

struct AccentSpec {
    const char *name;
    char32_t cp;
    bool wide;
};
const AccentSpec kAccents[] = {
    {"hat", 0x0302, false}, {"widehat", 0x0302, true}, {"check", 0x030C, false},
    {"widecheck", 0x030C, true}, {"tilde", 0x0303, false}, {"widetilde", 0x0303, true},
    {"acute", 0x0301, false}, {"grave", 0x0300, false}, {"dot", 0x0307, false},
    {"ddot", 0x0308, false}, {"dddot", 0x20DB, false}, {"breve", 0x0306, false},
    {"bar", 0x0304, false}, {"vec", 0x20D7, false}, {"mathring", 0x030A, false},
    {"overrightarrow", 0x2192, true}, {"overleftarrow", 0x2190, true},
    {"overleftrightarrow", 0x2194, true},
};

class Parser {
public:
    Parser(const QString &s, Ast &ast, Ctx &ctx)
        : d_(s.constData()), n_(int(s.size())), ast_(ast), ctx_(ctx)
    {
    }

    int parseTop()
    {
        EnvData env;
        env.kind = EnvKind::Gathered;
        if (!parseRows(env, 0, false, QString(), 0))
            return -1;
        if (env.rows.size() == 1 && env.rows[0].size() == 1)
            return env.rows[0][0];
        bool anyAmp = false;
        for (const auto &r : env.rows)
            anyAmp = anyAmp || r.size() > 1;
        env.kind = anyAmp ? EnvKind::Aligned : EnvKind::Gathered;
        return makeEnvNode(std::move(env), 0);
    }

    int nodeCount() const { return count_; }

private:
    const QChar *d_;
    int n_;
    int pos_ = 0;
    Ast &ast_;
    Ctx &ctx_;
    Alpha alpha_ = Alpha::Default;
    int count_ = 0;
    int cachePos_ = -1;
    Tok cache_;

    // ------------------------------------------------------------- lexing --
    Tok peek()
    {
        if (cachePos_ == pos_)
            return cache_;
        Tok t;
        int p = pos_;
        while (p < n_) {
            const QChar c = d_[p];
            if (c.isSpace()) {
                ++p;
                continue;
            }
            if (c == QLatin1Char('%')) {
                while (p < n_ && d_[p] != QLatin1Char('\n'))
                    ++p;
                continue;
            }
            break;
        }
        t.start = p;
        if (p >= n_) {
            t.t = Tok::End;
            t.end = p;
        } else {
            const QChar c = d_[p];
            switch (c.unicode()) {
            case '\\': {
                t.t = Tok::Cmd;
                if (p + 1 >= n_) {
                    t.end = p + 1;
                    t.name = QStringView();
                } else if (isLetter(d_[p + 1])) {
                    int q = p + 1;
                    while (q < n_ && isLetter(d_[q]))
                        ++q;
                    t.name = QStringView(d_ + p + 1, q - p - 1);
                    t.end = q;
                    // allow a trailing '*' for starred forms (\operatorname*)
                } else {
                    t.name = QStringView(d_ + p + 1, 1);
                    t.end = p + 2;
                }
                break;
            }
            case '{': t.t = Tok::LBrace; t.end = p + 1; break;
            case '}': t.t = Tok::RBrace; t.end = p + 1; break;
            case '^': t.t = Tok::Sup; t.end = p + 1; break;
            case '_': t.t = Tok::Sub; t.end = p + 1; break;
            case '&': t.t = Tok::Amp; t.end = p + 1; break;
            case '\'': t.t = Tok::Prime; t.end = p + 1; break;
            case '~': t.t = Tok::Tilde; t.end = p + 1; break;
            case '#': t.t = Tok::Hash; t.end = p + 1; break;
            case '$': t.t = Tok::Dollar; t.end = p + 1; break;
            default:
                t.t = Tok::Char;
                if (c.isHighSurrogate() && p + 1 < n_ && d_[p + 1].isLowSurrogate()) {
                    t.ch = QChar::surrogateToUcs4(c, d_[p + 1]);
                    t.end = p + 2;
                } else {
                    t.ch = c.unicode();
                    t.end = p + 1;
                }
            }
        }
        cachePos_ = pos_;
        cache_ = t;
        return t;
    }
    void consume(const Tok &t) { pos_ = t.end; }
    bool consumeStar()
    {
        if (pos_ < n_ && d_[pos_] == QLatin1Char('*')) {
            ++pos_;
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------- nodes --
    int newNode(NK k, int src)
    {
        if (ctx_.failed)
            return -1;
        if (++count_ > ctx_.lim.maxNodes) {
            ctx_.fail(QStringLiteral("Formula too complex (more than %1 nodes)").arg(ctx_.lim.maxNodes), src);
            return -1;
        }
        ast_.nodes.emplace_back();
        Node &n = ast_.nodes.back();
        n.kind = k;
        n.src = src;
        return int(ast_.nodes.size()) - 1;
    }
    Node &node(int i) { return ast_.nodes[size_t(i)]; }
    int wrapList(std::vector<int> &&items, int src)
    {
        const int n = newNode(NK::List, src);
        if (n < 0)
            return -1;
        node(n).list = int(ast_.lists.size());
        ast_.lists.push_back(std::move(items));
        return n;
    }
    int makeEnvNode(EnvData &&env, int src)
    {
        const int n = newNode(NK::Env, src);
        if (n < 0)
            return -1;
        node(n).list = int(ast_.envs.size());
        node(n).cls = Cls::Inner;
        ast_.envs.push_back(std::move(env));
        return n;
    }
    int spaceNode(float value, quint8 flags, int src)
    {
        const int n = newNode(NK::Space, src);
        if (n < 0)
            return -1;
        node(n).value = value;
        node(n).flags = flags;
        node(n).cls = Cls::None;
        return n;
    }
    int charNode(char32_t cp, Cls cls, int src, Alpha alpha = Alpha::Default)
    {
        const int n = newNode(NK::Char, src);
        if (n < 0)
            return -1;
        node(n).cp = cp;
        node(n).cls = cls;
        node(n).alpha = alpha;
        return n;
    }
    int textNode(const QString &text, int variant, Cls cls, int src)
    {
        const int n = newNode(NK::Text, src);
        if (n < 0)
            return -1;
        node(n).text = text;
        node(n).variant = variant;
        node(n).cls = cls;
        return n;
    }

    bool fail(const QString &msg, int off) { return ctx_.fail(msg, off); }

    // ------------------------------------------------------------- lists --
    int parseList(unsigned stops, int depth)
    {
        const int src = peek().start;
        if (depth > ctx_.maxDepth) {
            fail(QStringLiteral("Formula nested too deeply (limit %1)").arg(ctx_.maxDepth), src);
            return -1;
        }
        std::vector<int> items;
        std::vector<int> numer;
        int infix = -1; // 0 over, 1 atop, 2 choose
        int infixSrc = 0;
        for (;;) {
            if (!ctx_.check())
                return -1;
            const Tok t = peek();
            bool stop = false;
            switch (t.t) {
            case Tok::End:
                stop = true;
                break;
            case Tok::RBrace:
                if (stops & StopBrace) {
                    stop = true;
                    break;
                }
                fail(QStringLiteral("Unexpected '}' (no matching '{')"), t.start);
                return -1;
            case Tok::Amp:
                if (stops & StopAmp) {
                    stop = true;
                    break;
                }
                fail(QStringLiteral("Misplaced '&' (only allowed in matrix-like environments)"), t.start);
                return -1;
            case Tok::Sup:
            case Tok::Sub:
                consume(t);
                if (!attachScript(items, t, depth))
                    return -1;
                continue;
            case Tok::Prime:
                if (!attachPrimes(items, t))
                    return -1;
                continue;
            case Tok::Hash:
                fail(QStringLiteral("Unexpected '#' (macro parameters are not supported)"), t.start);
                return -1;
            case Tok::Dollar:
                fail(QStringLiteral("Unexpected '$' inside a formula"), t.start);
                return -1;
            case Tok::Char:
                if (t.ch == ']' && (stops & StopBracket)) {
                    stop = true;
                    break;
                }
                break;
            case Tok::Cmd:
                if (t.is("\\") || t.is("cr") || t.is("newline")) {
                    if (stops & StopRow) {
                        stop = true;
                        break;
                    }
                    fail(QStringLiteral("Line break '\\\\' is not allowed here"), t.start);
                    return -1;
                }
                if (t.is("right") || t.is("middle")) {
                    if (stops & StopRight) {
                        stop = true;
                        break;
                    }
                    fail(QStringLiteral("\\%1 without matching \\left").arg(t.name.toString()), t.start);
                    return -1;
                }
                if (t.is("end")) {
                    if (stops & StopEnd) {
                        stop = true;
                        break;
                    }
                    fail(QStringLiteral("\\end without matching \\begin"), t.start);
                    return -1;
                }
                if (t.is("over") || t.is("atop") || t.is("choose")) {
                    if (infix >= 0) {
                        fail(QStringLiteral("Ambiguous \\%1: use \\frac or add braces").arg(t.name.toString()), t.start);
                        return -1;
                    }
                    consume(t);
                    infix = t.is("over") ? 0 : t.is("atop") ? 1 : 2;
                    infixSrc = t.start;
                    numer.swap(items);
                    continue;
                }
                if (t.is("limits") || t.is("nolimits") || t.is("displaylimits")) {
                    consume(t);
                    if (items.empty() || node(items.back()).kind != NK::Op) {
                        fail(QStringLiteral("\\%1 must follow a math operator").arg(t.name.toString()), t.start);
                        return -1;
                    }
                    node(items.back()).limits = t.is("limits") ? 1 : t.is("nolimits") ? 0 : -1;
                    continue;
                }
                break;
            default:
                break;
            }
            if (stop)
                break;
            if (!parseAtom(items, depth, false))
                return -1;
        }
        if (infix >= 0) {
            const int num = wrapList(std::move(numer), infixSrc);
            const int den = wrapList(std::move(items), infixSrc);
            const int f = newNode(NK::Frac, infixSrc);
            if (f < 0 || num < 0 || den < 0)
                return -1;
            node(f).a = num;
            node(f).b = den;
            node(f).flags = infix == 0 ? 0 : infix == 1 ? kFracNoRule : (kFracNoRule | kFracParens);
            items.clear();
            items.push_back(f);
        }
        return wrapList(std::move(items), src);
    }

    static bool takesScripts(const Node &n) { return n.kind != NK::Space && n.kind != NK::Style; }

    bool attachScript(std::vector<int> &items, const Tok &t, int depth)
    {
        const bool sup = t.t == Tok::Sup;
        int base;
        if (items.empty() || !takesScripts(node(items.back()))) {
            base = newNode(NK::Empty, t.start);
            if (base < 0)
                return false;
            items.push_back(base);
        } else {
            base = items.back();
        }
        if (sup && node(base).sup >= 0)
            return fail(QStringLiteral("Double superscript (use braces, e.g. x^{ab})"), t.start);
        if (!sup && node(base).sub >= 0)
            return fail(QStringLiteral("Double subscript (use braces, e.g. x_{ab})"), t.start);
        const int arg = parseArg(t, depth, sup ? "superscript '^'" : "subscript '_'");
        if (arg < 0)
            return false;
        if (sup)
            node(base).sup = arg;
        else
            node(base).sub = arg;
        return true;
    }

    bool attachPrimes(std::vector<int> &items, const Tok &first)
    {
        int count = 0;
        for (Tok t = peek(); t.t == Tok::Prime; t = peek()) {
            consume(t);
            ++count;
        }
        int base;
        if (items.empty() || !takesScripts(node(items.back()))) {
            base = newNode(NK::Empty, first.start);
            if (base < 0)
                return false;
            items.push_back(base);
        } else {
            base = items.back();
        }
        if (node(base).sup >= 0)
            return fail(QStringLiteral("Double superscript (prime after superscript)"), first.start);
        node(base).primes = quint8(qMin(int(node(base).primes) + count, 16));
        return true;
    }

    // Required argument: {group} or a single token.
    int parseArg(const Tok &cmd, int depth, const char *what = nullptr)
    {
        const Tok t = peek();
        auto missing = [&]() {
            if (what)
                fail(QStringLiteral("Missing argument for %1").arg(QLatin1String(what)), cmd.start);
            else
                fail(QStringLiteral("Missing argument for \\%1").arg(cmd.name.toString()), cmd.start);
            return -1;
        };
        switch (t.t) {
        case Tok::LBrace:
            return parseGroup(depth);
        case Tok::End: case Tok::RBrace: case Tok::Amp: case Tok::Sup: case Tok::Sub:
        case Tok::Hash: case Tok::Dollar: case Tok::Prime:
            return missing();
        case Tok::Cmd:
            if (t.is("\\") || t.is("right") || t.is("middle") || t.is("end") || t.is("cr")
                || t.is("over") || t.is("limits") || t.is("nolimits"))
                return missing();
            break;
        default:
            break;
        }
        std::vector<int> tmp;
        if (!parseAtom(tmp, depth + 1, true))
            return -1;
        if (tmp.size() == 1)
            return tmp[0];
        return wrapList(std::move(tmp), t.start);
    }

    int parseGroup(int depth)
    {
        const Tok open = peek();
        consume(open);
        const Alpha saved = alpha_;
        const int list = parseList(StopBrace, depth + 1);
        alpha_ = saved;
        if (list < 0)
            return -1;
        const Tok close = peek();
        if (close.t != Tok::RBrace) {
            fail(QStringLiteral("Missing closing brace '}'"), open.start);
            return -1;
        }
        consume(close);
        return list;
    }

    // Raw {name} after \begin / \end / \color...
    bool readBraced(QString &out, int &start, bool lettersOnly)
    {
        const Tok open = peek();
        if (open.t != Tok::LBrace)
            return false;
        int p = open.end;
        start = p;
        int depth = 1;
        while (p < n_) {
            const QChar c = d_[p];
            if (c == QLatin1Char('{'))
                ++depth;
            else if (c == QLatin1Char('}') && --depth == 0)
                break;
            if (lettersOnly && !(isLetter(c) || c == QLatin1Char('*') || c.isSpace()))
                return false;
            out += c;
            ++p;
        }
        if (p >= n_)
            return false;
        pos_ = p + 1;
        out = out.trimmed();
        return true;
    }

    bool parseDimension(float &em, quint8 &flags, int errOff)
    {
        // number [unit]; units: em ex mu pt px mm cm in (pt relative to 10pt)
        int p = pos_;
        while (p < n_ && d_[p].isSpace())
            ++p;
        const int start = p;
        if (p < n_ && (d_[p] == QLatin1Char('-') || d_[p] == QLatin1Char('+')))
            ++p;
        while (p < n_ && (d_[p].isDigit() || d_[p] == QLatin1Char('.')))
            ++p;
        bool ok = false;
        double v = QStringView(d_ + start, p - start).toDouble(&ok);
        if (!ok)
            return fail(QStringLiteral("Expected a dimension (e.g. 1em, 3mu, 5pt)"), errOff);
        while (p < n_ && d_[p].isSpace())
            ++p;
        const QStringView unit = QStringView(d_ + p, qMin(2, n_ - p));
        flags = 0;
        if (unit == QLatin1String("em")) v *= 1;
        else if (unit == QLatin1String("ex")) flags = kSpaceEx;
        else if (unit == QLatin1String("mu")) flags = kSpaceMu;
        else if (unit == QLatin1String("pt")) v /= 10.0;
        else if (unit == QLatin1String("px")) v /= 14.0;
        else if (unit == QLatin1String("mm")) v *= 72.27 / 25.4 / 10.0;
        else if (unit == QLatin1String("cm")) v *= 72.27 / 2.54 / 10.0;
        else if (unit == QLatin1String("in")) v *= 7.227;
        else
            return fail(QStringLiteral("Unknown unit (use em, ex, mu, pt, px, mm, cm, in)"), p);
        p += 2;
        const double limit = (flags & kSpaceMu) ? 900.0 : 50.0;
        if (!std::isfinite(v) || std::fabs(v) > limit)
            return fail(QStringLiteral("Dimension out of range"), start);
        em = float(v);
        pos_ = p;
        return true;
    }

    // ------------------------------------------------------------- atoms --
    bool parseAtom(std::vector<int> &out, int depth, bool single)
    {
        const Tok t = peek();
        if (depth > ctx_.maxDepth)
            return fail(QStringLiteral("Formula nested too deeply (limit %1)").arg(ctx_.maxDepth), t.start);
        if (!ctx_.check())
            return false;
        switch (t.t) {
        case Tok::LBrace: {
            const int g = parseGroup(depth);
            if (g < 0)
                return false;
            out.push_back(g);
            return true;
        }
        case Tok::Tilde: {
            consume(t);
            const int s = spaceNode(0.333f, 0, t.start);
            if (s < 0)
                return false;
            out.push_back(s);
            return true;
        }
        case Tok::Char: {
            consume(t);
            const SymbolInfo si = classifyChar(t.ch);
            int n;
            if (si.flags & (kSymBigOp | kSymIntegral)) {
                n = opNode(si, t.start);
            } else {
                const bool alphaApplies = (si.flags & kSymLetter) || (t.ch >= '0' && t.ch <= '9');
                n = charNode(si.cp, si.cls, t.start, alphaApplies ? alpha_ : Alpha::Default);
            }
            if (n < 0)
                return false;
            out.push_back(n);
            return true;
        }
        case Tok::Cmd:
            consume(t);
            return parseCommand(t, out, depth);
        case Tok::RBrace:
            return fail(QStringLiteral("Unexpected '}' (no matching '{')"), t.start);
        default:
            return fail(QStringLiteral("Unexpected token"), t.start);
        }
        Q_UNUSED(single);
    }

    int opNode(const SymbolInfo &si, int src)
    {
        const int n = newNode(NK::Op, src);
        if (n < 0)
            return -1;
        node(n).cp = si.cp;
        node(n).cls = Cls::Op;
        node(n).limitsDefault = (si.flags & kSymBigOp) != 0;
        node(n).flags = (si.flags & kSymIntegral) ? kOpIntegral : 0;
        return n;
    }

    bool push(std::vector<int> &out, int n)
    {
        if (n < 0)
            return false;
        out.push_back(n);
        return true;
    }

    bool parseDelimiter(const Tok &cmd, char32_t &out)
    {
        const Tok t = peek();
        if (t.t == Tok::Char) {
            char32_t c = t.ch;
            if (c == '.') {
                out = 0;
                consume(t);
                return true;
            }
            if (c == '<')
                c = 0x27E8;
            else if (c == '>')
                c = 0x27E9;
            if (isDelimiterChar(c)) {
                out = c;
                consume(t);
                return true;
            }
        } else if (t.t == Tok::Cmd) {
            const SymbolInfo *si = lookupSymbol(t.name);
            if (si) {
                char32_t c = si->cp;
                if (c == 0x2223)
                    c = '|';
                else if (c == 0x2225)
                    c = 0x2016;
                if (isDelimiterChar(c)) {
                    out = c;
                    consume(t);
                    return true;
                }
            }
        }
        return fail(QStringLiteral("Missing or unsupported delimiter after \\%1").arg(cmd.name.toString()),
                    t.t == Tok::End ? cmd.start : t.start);
    }

    bool parseCommand(const Tok &t, std::vector<int> &out, int depth)
    {
        const QStringView name = t.name;
        if (name.isEmpty())
            return fail(QStringLiteral("Lone backslash at end of formula"), t.start);
        auto is = [&](const char *s) { return name == QLatin1String(s); };

        // --- spacing
        struct Sp { const char *n; float v; quint8 f; };
        static const Sp spaces[] = {
            {",", 3, kSpaceMu}, {":", 4, kSpaceMu}, {">", 4, kSpaceMu}, {";", 5, kSpaceMu},
            {"!", -3, kSpaceMu}, {" ", 0.333f, 0}, {"\n", 0.333f, 0}, {"\t", 0.333f, 0},
            {"quad", 1, 0}, {"qquad", 2, 0}, {"enspace", 0.5f, 0}, {"enskip", 0.5f, 0},
            {"thinspace", 3, kSpaceMu}, {"medspace", 4, kSpaceMu}, {"thickspace", 5, kSpaceMu},
            {"negthinspace", -3, kSpaceMu}, {"negmedspace", -4, kSpaceMu},
            {"negthickspace", -5, kSpaceMu}, {"nobreakspace", 0.333f, 0},
        };
        for (const Sp &s : spaces) {
            if (is(s.n))
                return push(out, spaceNode(s.v, s.f, t.start));
        }
        if (is("hspace") || is("kern") || is("mkern") || is("mskip") || is("hskip")) {
            consumeStar();
            const Tok open = peek();
            const bool braced = open.t == Tok::LBrace;
            if (braced)
                pos_ = open.end;
            float v = 0;
            quint8 f = 0;
            if (!parseDimension(v, f, t.start))
                return false;
            if (braced) {
                const Tok close = peek();
                if (close.t != Tok::RBrace)
                    return fail(QStringLiteral("Missing closing brace '}'"), open.start);
                consume(close);
            }
            cachePos_ = -1;
            return push(out, spaceNode(v, f, t.start));
        }

        // --- style
        if (is("displaystyle") || is("textstyle") || is("scriptstyle") || is("scriptscriptstyle")) {
            const int n = newNode(NK::Style, t.start);
            if (n < 0)
                return false;
            node(n).style = is("displaystyle") ? 0 : is("textstyle") ? 1 : is("scriptstyle") ? 2 : 3;
            node(n).cls = Cls::None;
            out.push_back(n);
            return true;
        }

        // --- alphabets
        struct Al { const char *n; Alpha a; };
        static const Al alphas[] = {
            {"mathrm", Alpha::Rm}, {"mathup", Alpha::Rm}, {"mathbf", Alpha::Bf},
            {"mathit", Alpha::It}, {"mathsf", Alpha::Sf}, {"mathtt", Alpha::Tt},
            {"mathbb", Alpha::Bb}, {"mathcal", Alpha::Cal}, {"mathscr", Alpha::Cal},
            {"mathfrak", Alpha::Frak}, {"boldsymbol", Alpha::BfIt}, {"bm", Alpha::BfIt},
            {"mathbfit", Alpha::BfIt}, {"mathnormal", Alpha::Default},
        };
        for (const Al &a : alphas) {
            if (is(a.n)) {
                const Alpha saved = alpha_;
                alpha_ = a.a;
                const int arg = parseArg(t, depth);
                alpha_ = saved;
                return push(out, arg);
            }
        }
        static const Al switches[] = {
            {"rm", Alpha::Rm}, {"bf", Alpha::Bf}, {"it", Alpha::It}, {"sf", Alpha::Sf},
            {"tt", Alpha::Tt}, {"cal", Alpha::Cal}, {"mit", Alpha::Default},
        };
        for (const Al &a : switches) {
            if (is(a.n)) {
                alpha_ = a.a;
                return true;
            }
        }

        // --- text
        if (is("text") || is("textrm") || is("textnormal") || is("textup") || is("mbox")
            || is("textbf") || is("textit") || is("emph") || is("texttt") || is("textsf")
            || is("hbox")) {
            int variant = fontVariant(RoleText);
            if (is("textbf")) variant = fontVariant(RoleText, false, true);
            else if (is("textit") || is("emph")) variant = fontVariant(RoleText, true, false);
            else if (is("texttt")) variant = fontVariant(RoleMono);
            else if (is("textsf")) variant = fontVariant(RoleSans);
            return push(out, parseText(t, variant));
        }

        // --- fractions
        if (is("frac") || is("dfrac") || is("tfrac") || is("cfrac") || is("binom")
            || is("dbinom") || is("tbinom")) {
            const int num = parseArg(t, depth);
            if (num < 0)
                return false;
            const int den = parseArg(t, depth);
            if (den < 0)
                return false;
            const int f = newNode(NK::Frac, t.start);
            if (f < 0)
                return false;
            node(f).a = num;
            node(f).b = den;
            if (is("binom") || is("dbinom") || is("tbinom"))
                node(f).flags = kFracNoRule | kFracParens;
            if (is("dfrac") || is("cfrac") || is("dbinom"))
                node(f).style = 0;
            else if (is("tfrac") || is("tbinom"))
                node(f).style = 1;
            out.push_back(f);
            return true;
        }

        if (is("sqrt")) {
            int index = -1;
            const Tok nx = peek();
            if (nx.t == Tok::Char && nx.ch == '[') {
                consume(nx);
                const Alpha saved = alpha_;
                index = parseList(StopBracket, depth + 1);
                alpha_ = saved;
                if (index < 0)
                    return false;
                const Tok close = peek();
                if (close.t != Tok::Char || close.ch != ']')
                    return fail(QStringLiteral("Missing ']' for \\sqrt index"), nx.start);
                consume(close);
            }
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            const int s = newNode(NK::Sqrt, t.start);
            if (s < 0)
                return false;
            node(s).a = body;
            node(s).b = index;
            out.push_back(s);
            return true;
        }

        if (is("left"))
            return push(out, parseLeftRight(t, depth));

        // \big \Bigl \biggr \Biggm ...
        {
            static const struct { const char *n; float size; } bigs[] = {
                {"big", 1.2f}, {"Big", 1.8f}, {"bigg", 2.4f}, {"Bigg", 3.0f}};
            for (const auto &b : bigs) {
                const QLatin1String base(b.n);
                if (name.startsWith(base) && name.size() <= base.size() + 1) {
                    const QStringView suffix = name.mid(base.size());
                    Cls cls = Cls::Ord;
                    if (suffix == QLatin1String("l")) cls = Cls::Open;
                    else if (suffix == QLatin1String("r")) cls = Cls::Close;
                    else if (suffix == QLatin1String("m")) cls = Cls::Rel;
                    else if (!suffix.isEmpty()) continue;
                    char32_t d = 0;
                    if (!parseDelimiter(t, d))
                        return false;
                    const int n = newNode(NK::BigDelim, t.start);
                    if (n < 0)
                        return false;
                    node(n).cp = d;
                    node(n).value = b.size;
                    node(n).cls = cls;
                    out.push_back(n);
                    return true;
                }
            }
        }

        if (is("begin"))
            return push(out, parseEnv(t, depth));

        for (const AccentSpec &a : kAccents) {
            if (is(a.name)) {
                const int body = parseArg(t, depth);
                if (body < 0)
                    return false;
                const int n = newNode(NK::Accent, t.start);
                if (n < 0)
                    return false;
                node(n).a = body;
                node(n).cp = a.cp;
                node(n).flags = a.wide ? kAccentWide : 0;
                out.push_back(n);
                return true;
            }
        }

        if (is("overline") || is("underline")) {
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            const int n = newNode(is("overline") ? NK::Overline : NK::Underline, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            out.push_back(n);
            return true;
        }

        if (is("overbrace") || is("underbrace") || is("overparen") || is("underparen")
            || is("overbracket") || is("underbracket")) {
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            const int n = newNode(NK::Brace, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            node(n).cls = Cls::Op;
            node(n).limits = 1;
            const bool under = name.startsWith(QLatin1String("under"));
            node(n).flags = under ? kUnder : 0;
            if (name.endsWith(QLatin1String("brace")))
                node(n).cp = under ? 0x23DF : 0x23DE;
            else if (name.endsWith(QLatin1String("paren")))
                node(n).cp = under ? 0x23DD : 0x23DC;
            else
                node(n).cp = under ? 0x23B5 : 0x23B4;
            out.push_back(n);
            return true;
        }

        if (is("overset") || is("underset") || is("stackrel")) {
            const int annot = parseArg(t, depth);
            if (annot < 0)
                return false;
            const int base = parseArg(t, depth);
            if (base < 0)
                return false;
            const int n = newNode(NK::Stack, t.start);
            if (n < 0)
                return false;
            node(n).a = base;
            if (is("underset"))
                node(n).c = annot;
            else
                node(n).b = annot;
            node(n).cls = is("stackrel") ? Cls::Rel : atomClass(base);
            out.push_back(n);
            return true;
        }

        if (is("phantom") || is("hphantom") || is("vphantom") || is("smash")) {
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            const int n = newNode(NK::Phantom, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            node(n).flags = is("phantom") ? (kPhantomH | kPhantomV)
                            : is("hphantom") ? kPhantomH
                            : is("vphantom") ? kPhantomV : kSmash;
            out.push_back(n);
            return true;
        }

        if (is("boxed") || is("fbox")) {
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            const int n = newNode(NK::Boxed, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            out.push_back(n);
            return true;
        }

        if (is("not")) {
            const int body = parseArg(t, depth);
            if (body < 0)
                return false;
            Node &b = node(body);
            if (b.kind == NK::Char && negated(b.cp)) {
                b.cp = negated(b.cp);
                out.push_back(body);
                return true;
            }
            const int n = newNode(NK::Not, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            node(n).cls = atomClass(body);
            out.push_back(n);
            return true;
        }

        if (is("operatorname") || is("mathop") || is("operatornamewithlimits")) {
            const bool star = consumeStar();
            const bool isOpName = !is("mathop");
            const Alpha saved = alpha_;
            if (isOpName)
                alpha_ = Alpha::Rm;
            const int body = parseArg(t, depth);
            alpha_ = saved;
            if (body < 0)
                return false;
            const int n = newNode(NK::Op, t.start);
            if (n < 0)
                return false;
            node(n).a = body;
            node(n).cls = Cls::Op;
            node(n).limitsDefault = !isOpName || star || is("operatornamewithlimits");
            out.push_back(n);
            return true;
        }

        {
            static const struct { const char *n; Cls c; } classes[] = {
                {"mathord", Cls::Ord}, {"mathbin", Cls::Bin}, {"mathrel", Cls::Rel},
                {"mathopen", Cls::Open}, {"mathclose", Cls::Close},
                {"mathpunct", Cls::Punct}, {"mathinner", Cls::Inner}};
            for (const auto &c : classes) {
                if (is(c.n)) {
                    const int body = parseArg(t, depth);
                    if (body < 0)
                        return false;
                    std::vector<int> one{body};
                    const int n = wrapList(std::move(one), t.start);
                    if (n < 0)
                        return false;
                    node(n).cls = c.c;
                    out.push_back(n);
                    return true;
                }
            }
        }

        if (is("color")) {
            QString dummy;
            int s = 0;
            if (!readBraced(dummy, s, false))
                return fail(QStringLiteral("Missing argument for \\color"), t.start);
            cachePos_ = -1;
            return true; // colour is ignored (single-colour display list)
        }
        if (is("textcolor") || is("colorbox")) {
            QString dummy;
            int s = 0;
            if (!readBraced(dummy, s, false))
                return fail(QStringLiteral("Missing argument for \\%1").arg(name.toString()), t.start);
            cachePos_ = -1;
            return push(out, parseArg(t, depth));
        }

        if (is("pmod") || is("pod") || is("mod")) {
            const int arg = parseArg(t, depth);
            if (arg < 0)
                return false;
            const bool paren = !is("mod");
            if (!push(out, spaceNode(is("mod") ? 12 : 8, kSpaceMu, t.start)))
                return false;
            if (paren && !push(out, charNode('(', Cls::Open, t.start)))
                return false;
            if (!is("pod")) {
                if (!push(out, textNode(QStringLiteral("mod"), fontVariant(RoleMath), Cls::Ord, t.start)))
                    return false;
                if (!push(out, spaceNode(6, kSpaceMu, t.start)))
                    return false;
            }
            if (!push(out, arg))
                return false;
            if (paren && !push(out, charNode(')', Cls::Close, t.start)))
                return false;
            return true;
        }
        if (is("bmod"))
            return push(out, textNode(QStringLiteral("mod"), fontVariant(RoleMath), Cls::Bin, t.start));

        if (const FunctionInfo *fi = lookupFunction(name)) {
            const int n = newNode(NK::Op, t.start);
            if (n < 0)
                return false;
            node(n).text = QString::fromLatin1(fi->text);
            node(n).variant = fontVariant(RoleMath);
            node(n).cls = Cls::Op;
            node(n).limitsDefault = fi->limits;
            out.push_back(n);
            return true;
        }

        if (const SymbolInfo *si = lookupSymbol(name)) {
            if (si->flags & (kSymBigOp | kSymIntegral))
                return push(out, opNode(*si, t.start));
            return push(out, charNode(si->cp, si->cls, t.start,
                                      (si->flags & kSymLetter) ? alpha_ : Alpha::Default));
        }

        return fail(QStringLiteral("Unknown command \\%1").arg(name.toString()), t.start);
    }

    Cls atomClass(int n)
    {
        const Node &x = node(n);
        if (x.kind == NK::List && x.cls == Cls::Ord) {
            const auto &l = ast_.lists[size_t(x.list)];
            if (l.size() == 1)
                return node(l[0]).cls == Cls::None ? Cls::Ord : node(l[0]).cls;
        }
        return x.cls == Cls::None ? Cls::Ord : x.cls;
    }

    int parseText(const Tok &cmd, int variant)
    {
        const Tok open = peek();
        QString out;
        if (open.t == Tok::Char) {
            consume(open);
            out = QString::fromUcs4(&open.ch, 1);
        } else if (open.t != Tok::LBrace) {
            fail(QStringLiteral("Missing argument for \\%1").arg(cmd.name.toString()), cmd.start);
            return -1;
        } else {
            int p = open.end;
            int depth = 1;
            bool lastSpace = false;
            while (p < n_) {
                const QChar c = d_[p];
                if (c == QLatin1Char('\\')) {
                    if (p + 1 >= n_)
                        break;
                    const QChar e = d_[p + 1];
                    if (QStringLiteral("{}$%&#_ ").contains(e)) {
                        out += e;
                        lastSpace = e == QLatin1Char(' ');
                        p += 2;
                        continue;
                    }
                    if (e == QLatin1Char('\\')) {
                        out += QLatin1Char(' ');
                        p += 2;
                        continue;
                    }
                    fail(QStringLiteral("Commands inside \\%1{...} are not supported").arg(cmd.name.toString()), p);
                    return -1;
                }
                if (c == QLatin1Char('{')) {
                    ++depth;
                    ++p;
                    continue;
                }
                if (c == QLatin1Char('}')) {
                    if (--depth == 0)
                        break;
                    ++p;
                    continue;
                }
                if (c == QLatin1Char('$')) {
                    fail(QStringLiteral("Math inside \\%1{...} is not supported").arg(cmd.name.toString()), p);
                    return -1;
                }
                if (c.isSpace() || c == QLatin1Char('~')) {
                    if (!lastSpace)
                        out += QLatin1Char(' ');
                    lastSpace = true;
                    ++p;
                    continue;
                }
                lastSpace = false;
                out += c;
                ++p;
            }
            if (p >= n_ || depth != 0) {
                fail(QStringLiteral("Missing closing brace '}'"), open.start);
                return -1;
            }
            pos_ = p + 1;
            cachePos_ = -1;
        }
        if (out.size() > 2000) {
            fail(QStringLiteral("\\%1 argument too long (limit 2000 characters)").arg(cmd.name.toString()), cmd.start);
            return -1;
        }
        return textNode(out, variant, Cls::Ord, cmd.start);
    }

    int parseLeftRight(const Tok &left, int depth)
    {
        LeftRightData lr;
        char32_t d = 0;
        if (!parseDelimiter(left, d))
            return -1;
        lr.delims.push_back(d);
        for (;;) {
            const Alpha saved = alpha_;
            const int seg = parseList(StopRight, depth + 1);
            alpha_ = saved;
            if (seg < 0)
                return -1;
            lr.segments.push_back(seg);
            const Tok e = peek();
            if (e.is("right") || e.is("middle")) {
                consume(e);
                if (!parseDelimiter(e, d))
                    return -1;
                lr.delims.push_back(d);
                if (e.is("right"))
                    break;
                continue;
            }
            fail(QStringLiteral("\\left without matching \\right"), left.start);
            return -1;
        }
        const int n = newNode(NK::LeftRight, left.start);
        if (n < 0)
            return -1;
        node(n).list = int(ast_.lrs.size());
        node(n).cls = Cls::Inner;
        ast_.lrs.push_back(std::move(lr));
        return n;
    }

    int parseEnv(const Tok &begin, int depth)
    {
        QString name;
        int nameStart = begin.end;
        if (!readBraced(name, nameStart, true) || name.isEmpty()) {
            fail(QStringLiteral("Expected {environment name} after \\begin"), begin.start);
            return -1;
        }
        cachePos_ = -1;
        const EnvSpec *spec = nullptr;
        for (const EnvSpec &e : kEnvs) {
            if (name == QLatin1String(e.name)) {
                spec = &e;
                break;
            }
        }
        if (!spec) {
            fail(QStringLiteral("Unknown environment '%1'").arg(name), nameStart);
            return -1;
        }
        EnvData env;
        env.kind = spec->kind;
        env.src = begin.start;
        if (name.startsWith(QLatin1String("alignat"))) {
            QString dummy; // column-pair count is implied by the rows
            int s = 0;
            readBraced(dummy, s, false);
            cachePos_ = -1;
        }
        if (env.kind == EnvKind::Array) {
            QString spec;
            int specStart = 0;
            if (!readBraced(spec, specStart, false)) {
                fail(QStringLiteral("Missing column specification for array (e.g. {cc})"), begin.start);
                return -1;
            }
            cachePos_ = -1;
            env.vlines.push_back(0);
            for (int i = 0; i < spec.size(); ++i) {
                const QChar c = spec[i];
                if (c == QLatin1Char('l') || c == QLatin1Char('c') || c == QLatin1Char('r')) {
                    env.align.push_back(char(c.unicode()));
                    env.vlines.push_back(0);
                } else if (c == QLatin1Char('|')) {
                    env.vlines.back()++;
                } else if (!c.isSpace()) {
                    fail(QStringLiteral("Unsupported array column type '%1' (use l, c, r, |)").arg(c), specStart + i);
                    return -1;
                }
            }
            if (env.align.empty()) {
                fail(QStringLiteral("Empty array column specification"), specStart);
                return -1;
            }
        }
        if (!parseRows(env, depth + 1, true, name, begin.start))
            return -1;
        return makeEnvNode(std::move(env), begin.start);
    }

    bool parseRows(EnvData &env, int depth, bool isEnv, const QString &name, int beginOff)
    {
        env.hlines.push_back(0);
        std::vector<int> row;
        for (;;) {
            if (row.empty()) {
                for (Tok h = peek(); h.is("hline"); h = peek()) {
                    consume(h);
                    env.hlines.back()++;
                }
            }
            const Alpha saved = alpha_;
            const int cell = parseList(StopAmp | StopRow | (isEnv ? StopEnd : 0u), depth);
            alpha_ = saved;
            if (cell < 0)
                return false;
            row.push_back(cell);
            const Tok t = peek();
            if (t.t == Tok::Amp) {
                consume(t);
                continue;
            }
            if (t.is("\\") || t.is("cr") || t.is("newline")) {
                consume(t);
                float gap = 0;
                const Tok br = peek();
                if (br.t == Tok::Char && br.ch == '[') {
                    pos_ = br.end;
                    quint8 f = 0;
                    if (!parseDimension(gap, f, br.start))
                        return false;
                    cachePos_ = -1;
                    const Tok close = peek();
                    if (close.t != Tok::Char || close.ch != ']')
                        return fail(QStringLiteral("Missing ']' after row spacing"), br.start);
                    consume(close);
                    if (f & kSpaceMu)
                        gap /= 18.f;
                    else if (f & kSpaceEx)
                        gap *= 0.45f;
                }
                env.rows.push_back(std::move(row));
                row.clear();
                env.rowGapEm.push_back(gap);
                env.hlines.push_back(0);
                continue;
            }
            if (isEnv && t.is("end")) {
                consume(t);
                QString endName;
                int s = 0;
                if (!readBraced(endName, s, true))
                    return fail(QStringLiteral("Expected {%1} after \\end").arg(name), t.start);
                cachePos_ = -1;
                if (endName != name)
                    return fail(QStringLiteral("\\end{%1} does not match \\begin{%2}").arg(endName, name), t.start);
                break;
            }
            if (t.t == Tok::End) {
                if (isEnv)
                    return fail(QStringLiteral("Missing \\end{%1}").arg(name), beginOff);
                break;
            }
            return fail(QStringLiteral("Unexpected token"), t.start);
        }
        const bool lastEmpty = row.size() == 1
                               && ast_.lists[size_t(node(row[0]).list)].empty()
                               && !env.rows.empty();
        if (!lastEmpty) {
            env.rows.push_back(std::move(row));
            env.rowGapEm.push_back(0);
            env.hlines.push_back(0);
        }
        return true;
    }
};

} // namespace

int parse(const QString &src, Ast &ast, Ctx &ctx)
{
    Parser p(src, ast, ctx);
    const int root = p.parseTop();
    if (ctx.failed)
        return -1;
    ast.root = root;
    return root;
}

} // namespace detail
} // namespace math
} // namespace folia
