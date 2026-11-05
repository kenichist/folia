// Table-driven lexer for the C-like family (c, cpp, csharp, java, go, rust,
// swift, lua, python, javascript, typescript, sql). Language differences are
// expressed through Features plus a few small hooks; everything is a single
// left-to-right pass, linear in the line length, with no allocation.
//
// Carry state layout: low 4 bits = mode (below), upper 28 bits = payload.
// States are canonical: the same lexical situation always yields the same int.
#include "code/LangInternal.h"

namespace folia::code {

namespace {

enum : uint32_t {
    M_NORMAL = 0,
    M_BLOCK,    // inside /* */ ; payload = nesting depth (1 if not nested)
    M_LUASTR,   // inside [==[ ]==] ; payload = level
    M_LUACMT,   // inside --[==[ ]==] ; payload = level
    M_TRIPLE,   // inside """ / ''' ; payload = quote kind (1 "  2 ') | 4 if no escapes
    M_RAW,      // rust r#"..."# / swift #"""..."""# ; payload = hashes | triple<<8
    M_STR,      // multi-line "..." '...' `...` ; payload = quote kind (1 2 3) | 8 verbatim
    M_STRCONT,  // string continued by a trailing backslash ; payload = quote kind
    M_PPCONT,   // preprocessor line continued by a trailing backslash
    M_LCCONT,   // // comment continued by a trailing backslash
    M_DELIM,    // c++ R"delim( or sql $tag$ ; payload = len | hash << 5
    M_TPLTEXT,  // js template text ; payload = context stack
    M_TPLCODE,  // js code inside ${ } ; payload = context stack
};

constexpr uint32_t kMaxDepth = 1u << 20;
constexpr uint32_t kMaxStack = 1u << 21; // push allowed while stack < kMaxStack (payload < 2^22)

struct StrRes {
    int end;
    bool closed;
    bool contEsc;
};

inline uint32_t quoteKind(char16_t q) { return q == u'"' ? 1u : q == u'\'' ? 2u : 3u; }
inline char16_t quoteChar(uint32_t k) { return k == 1 ? u'"' : k == 2 ? u'\'' : u'`'; }

class CLex {
public:
    CLex(const Language &lang, QStringView line, Emit &sink)
        : L(lang), F(lang.f), s(line.utf16()), n(int(line.size())), em(sink)
    {
    }

    int run(int stateIn);

private:
    const Language &L;
    const Features &F;
    const char16_t *s;
    const int n;
    Emit &em;

    int i = 0;
    int out = 0;
    uint32_t stack = 1; // js template context stack with sentinel bit
    bool tplEol = false;
    bool inPre = false;
    bool atStart = true;
    bool prevValue = false;
    bool regexFailed = false;
    bool attrFailed = false;
    bool pendType = false;
    bool pendFunc = false;

    bool idStart(char16_t c) const
    {
        return isAsciiAlpha(c) || c == u'_' || c >= 0x80 || (c == u'$' && F.dollarIdent);
    }
    bool idPart(char16_t c) const
    {
        return isAlnum(c) || c == u'_' || c >= 0x80 || (c == u'$' && F.dollarIdent);
    }
    int identEnd(int j) const
    {
        while (j < n && idPart(s[j]))
            ++j;
        return j;
    }

    // ---- scanners (emit + set `out` when unterminated) ----
    StrRes scanStr(int j, char16_t q, bool esc, bool doubled) const;
    int finishString(int tokStart, int from, char16_t qc, bool verb);
    int openQuote(int tokStart, int q, bool verb);
    int tripleStr(int from, int tokStart, char16_t q, bool esc);
    int rawStr(int from, int tokStart, uint32_t hashes, bool triple, bool multi);
    int blockComment(int from, int tokStart, uint32_t depth);
    int luaLong(int from, int tokStart, uint32_t level, Token tok, uint32_t mode);
    int delimClose(int from, int tokStart, uint32_t len, uint32_t hash);
    bool cppRawOpen(int tokStart, int q);
    int tplRun(int tokStart, int j);
    int scanNumber(int j) const;
    bool tryRegex(int &e) const;
    bool rustChar(int q, int &end) const;
    bool luaOpen(int j, uint32_t &level, int &after) const;
    bool dollarOpen(int j, uint32_t &len, uint32_t &hash, int &after) const;

    // ---- token handlers ----
    void identifier(bool first);
    void preprocessor();
    Token capToken(int st0, int len) const;
    bool allIn(int st0, int len, const char *set) const;
    bool lineEndsWithColon() const;
};

// ---------------------------------------------------------------------------
StrRes CLex::scanStr(int j, char16_t q, bool esc, bool doubled) const
{
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\' && esc) {
            if (j + 1 >= n)
                return {n, false, true};
            if (F.luaLong && s[j + 1] == u'z' && j + 2 >= n) // lua: \z skips the line break
                return {n, false, true};
            j += 2;
            continue;
        }
        if (c == q) {
            if (doubled && j + 1 < n && s[j + 1] == q) {
                j += 2;
                continue;
            }
            return {j + 1, true, false};
        }
        ++j;
    }
    return {n, false, false};
}

int CLex::finishString(int tokStart, int from, char16_t qc, bool verb)
{
    const bool esc = !verb && !F.sqlStr;
    const bool dbl = (F.csStr && verb) || (F.sqlStr && qc != u'`');
    const StrRes r = scanStr(from, qc, esc, dbl);
    em.add(tokStart, r.end, qc == u'`' && F.btIdent ? F.btTok : Token::String);
    if (!r.closed) {
        bool multi;
        if (qc == u'"')
            multi = F.dqMulti || (F.csStr && verb);
        else if (qc == u'\'')
            multi = F.sqMulti;
        else
            multi = F.backtickRaw;
        if (multi)
            out = packState(M_STR, quoteKind(qc) | (verb ? 8u : 0u));
        else if (r.contEsc && F.strCont)
            out = packState(M_STRCONT, quoteKind(qc));
    }
    return r.end;
}

int CLex::tripleStr(int from, int tokStart, char16_t q, bool esc)
{
    int j = from;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\' && esc) {
            j += 2;
            continue;
        }
        if (c == q && j + 2 < n && s[j + 1] == q && s[j + 2] == q) {
            j += 3;
            em.add(tokStart, j, Token::String);
            return j;
        }
        ++j;
    }
    em.add(tokStart, n, Token::String);
    out = packState(M_TRIPLE, (q == u'"' ? 1u : 2u) | (esc ? 0u : 4u));
    return n;
}

int CLex::openQuote(int tokStart, int q, bool verb)
{
    const char16_t qc = s[q];
    if (F.triple && (qc == u'"' || (qc == u'\'' && F.tripleSq)) && q + 2 < n && s[q + 1] == qc && s[q + 2] == qc)
        return tripleStr(q + 3, tokStart, qc, !F.csStr && !verb);
    return finishString(tokStart, q + 1, qc, verb);
}

int CLex::rawStr(int from, int tokStart, uint32_t hashes, bool triple, bool multi)
{
    const int need = triple ? 3 : 1;
    int j = from;
    while (j < n) {
        if (s[j] == u'"') {
            int k = j + 1;
            int qc = 1;
            while (triple && qc < 3 && k < n && s[k] == u'"') {
                ++k;
                ++qc;
            }
            if (qc == need) {
                uint32_t h = 0;
                int m = k;
                while (m < n && s[m] == u'#' && h < hashes) {
                    ++m;
                    ++h;
                }
                if (h == hashes) {
                    em.add(tokStart, m, Token::String);
                    return m;
                }
            }
            j = k;
            continue;
        }
        ++j;
    }
    em.add(tokStart, n, Token::String);
    if (multi)
        out = packState(M_RAW, (hashes & 0xFFu) | (triple ? 0x100u : 0u));
    return n;
}

int CLex::blockComment(int from, int tokStart, uint32_t depth)
{
    int j = from;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'*' && j + 1 < n && s[j + 1] == u'/') {
            j += 2;
            if (--depth == 0) {
                em.add(tokStart, j, Token::Comment);
                return j;
            }
            continue;
        }
        if (F.nest && c == u'/' && j + 1 < n && s[j + 1] == u'*') {
            if (depth < kMaxDepth)
                ++depth;
            j += 2;
            continue;
        }
        ++j;
    }
    em.add(tokStart, n, Token::Comment);
    out = packState(M_BLOCK, F.nest ? depth : 1u);
    return n;
}

int CLex::luaLong(int from, int tokStart, uint32_t level, Token tok, uint32_t mode)
{
    int j = from;
    while (j < n) {
        if (s[j] == u']') {
            int k = j + 1;
            uint32_t c = 0;
            while (k < n && s[k] == u'=') {
                ++k;
                ++c;
            }
            if (c == level && k < n && s[k] == u']') {
                em.add(tokStart, k + 1, tok);
                return k + 1;
            }
            j = k;
            continue;
        }
        ++j;
    }
    em.add(tokStart, n, tok);
    out = packState(mode, level > kMaxDepth ? kMaxDepth : level);
    return n;
}

int CLex::delimClose(int from, int tokStart, uint32_t len, uint32_t hash)
{
    int j = from;
    if (F.cppRaw) { // )delim"
        while (j < n) {
            if (s[j] == u')' && j + 1 + int(len) < n && s[j + 1 + len] == u'"' &&
                hashUnits(s + j + 1, int(len)) == hash) {
                const int e = j + 2 + int(len);
                em.add(tokStart, e, Token::String);
                return e;
            }
            ++j;
        }
    } else { // $tag$
        while (j < n) {
            if (s[j] == u'$' && j + 1 + int(len) < n && s[j + 1 + len] == u'$' &&
                hashUnits(s + j + 1, int(len)) == hash) {
                const int e = j + 2 + int(len);
                em.add(tokStart, e, Token::String);
                return e;
            }
            ++j;
        }
    }
    em.add(tokStart, n, Token::String);
    out = packState(M_DELIM, len | (hash << 5));
    return n;
}

// s[q] == '"' preceded by an ident ending in R at tokStart. Parses R"delim(.
bool CLex::cppRawOpen(int tokStart, int q)
{
    int j = q + 1;
    while (j < n && s[j] != u'(' && j - (q + 1) <= 16) {
        const char16_t c = s[j];
        if (c <= u' ' || c == u')' || c == u'\\' || c == u'"' || c >= 0x7F)
            return false;
        ++j;
    }
    if (j >= n || s[j] != u'(' || j - (q + 1) > 16)
        return false;
    const int len = j - (q + 1);
    const uint32_t hash = hashUnits(s + q + 1, len);
    i = delimClose(j + 1, tokStart, uint32_t(len), hash);
    return true;
}

// Scans template text from j (just after a backtick or a closing brace).
int CLex::tplRun(int tokStart, int j)
{
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\') {
            j += 2;
            continue;
        }
        if (c == u'`') {
            ++j;
            em.add(tokStart, j, Token::String);
            return j;
        }
        if (c == u'$' && j + 1 < n && s[j + 1] == u'{') {
            em.add(tokStart, j, Token::String);
            em.add(j, j + 2, Token::Punctuation);
            if (stack < kMaxStack)
                stack = (stack << 1) | 1u;
            return j + 2;
        }
        ++j;
    }
    em.add(tokStart, n, Token::String);
    tplEol = true;
    return n;
}

// ---------------------------------------------------------------------------
int CLex::scanNumber(int j) const
{
    if (s[j] == u'0' && j + 1 < n) {
        const char16_t x = char16_t(s[j + 1] | 0x20);
        if (x == u'x' || x == u'b' || x == u'o') {
            const bool hex = x == u'x';
            j += 2;
            while (j < n) {
                const char16_t c = s[j];
                if (isAlnum(c) || c == u'_') {
                    if (hex && (c == u'p' || c == u'P') && j + 1 < n && (s[j + 1] == u'+' || s[j + 1] == u'-'))
                        ++j;
                    ++j;
                } else if (F.numApos && c == u'\'' && j + 1 < n && isAlnum(s[j + 1])) {
                    j += 2;
                } else {
                    break;
                }
            }
            return j;
        }
    }
    while (j < n && (isDigit(s[j]) || s[j] == u'_' ||
                     (F.numApos && s[j] == u'\'' && j + 1 < n && isDigit(s[j + 1]))))
        ++j;
    if (j < n && s[j] == u'.') {
        if (j + 1 < n && isDigit(s[j + 1])) {
            ++j;
            while (j < n && (isDigit(s[j]) || s[j] == u'_'))
                ++j;
        } else if (!(j + 1 < n && (s[j + 1] == u'.' || idStart(s[j + 1])))) {
            ++j;
        }
    }
    if (j < n && (s[j] == u'e' || s[j] == u'E')) {
        int k = j + 1;
        if (k < n && (s[k] == u'+' || s[k] == u'-'))
            ++k;
        if (k < n && isDigit(s[k])) {
            j = k;
            while (j < n && (isDigit(s[j]) || s[j] == u'_'))
                ++j;
        }
    }
    while (j < n && (isAlnum(s[j]) || s[j] == u'_'))
        ++j;
    return j;
}

bool CLex::tryRegex(int &e) const
{
    int j = i + 1;
    bool cls = false;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\') {
            j += 2;
            continue;
        }
        if (c == u'[')
            cls = true;
        else if (c == u']')
            cls = false;
        else if (c == u'/' && !cls) {
            ++j;
            while (j < n && isAsciiAlpha(s[j]))
                ++j;
            e = j;
            return true;
        }
        ++j;
    }
    return false;
}

bool CLex::rustChar(int q, int &end) const
{
    int j = q + 1;
    if (j >= n)
        return false;
    if (s[j] == u'\\') {
        const int lim = (j + 12 < n) ? j + 12 : n;
        for (int k = j + 2; k < lim; ++k)
            if (s[k] == u'\'') {
                end = k + 1;
                return true;
            }
        return false;
    }
    int k = j + 1;
    if (isHighSurrogate(s[j]) && k < n && isLowSurrogate(s[k]))
        ++k;
    if (s[j] != u'\'' && k < n && s[k] == u'\'') {
        end = k + 1;
        return true;
    }
    return false;
}

// s[j] == '[' : Lua long bracket opener "[" "="* "[".
bool CLex::luaOpen(int j, uint32_t &level, int &after) const
{
    int k = j + 1;
    uint32_t c = 0;
    while (k < n && s[k] == u'=') {
        ++k;
        ++c;
    }
    if (k < n && s[k] == u'[') {
        level = c;
        after = k + 1;
        return true;
    }
    return false;
}

// s[j] == '$' : SQL dollar quote opener $tag$ (tag may be empty).
bool CLex::dollarOpen(int j, uint32_t &len, uint32_t &hash, int &after) const
{
    int k = j + 1;
    if (k < n && (isAsciiAlpha(s[k]) || s[k] == u'_')) {
        while (k < n && (isAlnum(s[k]) || s[k] == u'_') && k - (j + 1) <= 32)
            ++k;
    }
    if (k < n && s[k] == u'$' && k - (j + 1) <= 31) {
        len = uint32_t(k - (j + 1));
        hash = hashUnits(s + j + 1, int(len));
        after = k + 1;
        return true;
    }
    return false;
}

bool CLex::allIn(int st0, int len, const char *set) const
{
    for (int k = 0; k < len; ++k) {
        const char16_t c = s[st0 + k];
        if (c >= 128 || !std::strchr(set, int(c)) || c == 0)
            return false;
    }
    return true;
}

Token CLex::capToken(int st0, int len) const
{
    if (len <= 0 || !isUpper(s[st0]))
        return Token::Plain;
    bool lower = false;
    for (int k = 1; k < len; ++k) {
        const char16_t c = s[st0 + k];
        if (isLower(c) || c >= 0x80) {
            lower = true;
            break;
        }
    }
    if (lower)
        return F.capType ? Token::Type : Token::Plain;
    if (len == 1)
        return F.capType ? Token::Type : Token::Plain;
    return F.capConst ? Token::Constant : Token::Plain;
}

bool CLex::lineEndsWithColon() const
{
    int k = n - 1;
    while (k >= 0 && isSpaceCh(s[k]))
        --k;
    return k >= 0 && s[k] == u':';
}

// ---------------------------------------------------------------------------
void CLex::identifier(bool first)
{
    const int st0 = i;
    ++i;
    while (i < n && idPart(s[i]))
        ++i;
    const int len = i - st0;

    // --- rust raw strings / raw identifiers ---
    if (F.rustRaw && i < n && (s[i] == u'"' || s[i] == u'#') &&
        ((len == 1 && s[st0] == u'r') || (len == 2 && (s[st0] == u'b' || s[st0] == u'c') && s[st0 + 1] == u'r'))) {
        int k = i;
        uint32_t h = 0;
        while (k < n && s[k] == u'#') {
            ++k;
            ++h;
        }
        if (k < n && s[k] == u'"') {
            i = rawStr(k + 1, st0, h > 255 ? 255 : h, false, true);
            prevValue = true;
            return;
        }
        if (h == 1 && len == 1 && k < n && idStart(s[k])) {
            i = identEnd(k);
            prevValue = true;
            return;
        }
    }

    // --- string prefixes (python r"", b"", f""; c++ L"" u8"" R"()"; rust b"" ) ---
    if (i < n && (s[i] == u'"' || s[i] == u'\'') && F.prefixes && len <= F.maxPrefix && allIn(st0, len, F.prefixes)) {
        if (F.cppRaw && s[i] == u'"' && s[i - 1] == u'R' && cppRawOpen(st0, i)) {
            prevValue = true;
            return;
        }
        i = openQuote(st0, i, false);
        prevValue = true;
        return;
    }

    const bool prevDot = st0 > 0 && s[st0 - 1] == u'.' && !(st0 > 1 && s[st0 - 2] == u'.');
    uint8_t info = prevDot ? 0 : L.kw.find(s + st0, len);

    if (info & kSoftNext) {
        int k = i;
        while (k < n && isSpaceCh(s[k]))
            ++k;
        const bool ok = k > i && k < n && (idStart(s[k]) || s[k] == u'"' || s[k] == u'\'' || s[k] == u'`');
        if (!ok)
            info = 0;
    } else if (info & kSoftLine) {
        if (!(first && lineEndsWithColon()))
            info = 0;
    }

    if (info) {
        const Token t = Token(info & kInfoToken);
        em.add(st0, i, t);
        pendType = (info & kNextType) != 0;
        pendFunc = (info & kNextFunc) != 0;
        if (t == Token::Keyword)
            prevValue = F.regex && ((len == 4 && equalsAscii(s + st0, 4, "this")) ||
                                    (len == 5 && equalsAscii(s + st0, 5, "super")));
        else
            prevValue = true;
        return;
    }

    Token t = Token::Plain;
    int end = i;
    if (pendFunc) {
        t = Token::Function;
    } else if (pendType) {
        t = Token::Type;
    } else if (i < n && s[i] == u'(') {
        t = Token::Function;
    } else if (F.rustMacro && i + 1 < n && s[i] == u'!' && (s[i + 1] == u'(' || s[i + 1] == u'[' || s[i + 1] == u'{')) {
        t = Token::Function;
        end = i + 1;
    } else if (F.capType || F.capConst) {
        t = capToken(st0, len);
    }
    pendType = pendFunc = false;
    em.add(st0, end, t);
    i = end;
    prevValue = true;
}

void CLex::preprocessor()
{
    int j = i + 1;
    while (j < n && (s[j] == u' ' || s[j] == u'\t'))
        ++j;
    int k = j;
    while (k < n && isAsciiAlpha(s[k]))
        ++k;
    inPre = true;
    if (k == j) {
        em.add(i, i + 1, Token::Preprocessor);
        ++i;
        return;
    }
    em.add(i, k, Token::Preprocessor);
    const int len = k - j;
    const bool incl = equalsAscii(s + j, len, "include") || equalsAscii(s + j, len, "import") ||
                      equalsAscii(s + j, len, "include_next");
    i = k;
    if (incl) {
        int m = i;
        while (m < n && (s[m] == u' ' || s[m] == u'\t'))
            ++m;
        if (m < n && s[m] == u'<') {
            int e = m + 1;
            while (e < n && s[e] != u'>')
                ++e;
            if (e < n) {
                em.add(m, e + 1, Token::String);
                i = e + 1;
            }
        }
    }
}

// ---------------------------------------------------------------------------
int CLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    const uint32_t mode = st & 15u;
    const uint32_t pl = st >> 4;

    // ---- resume a construct that was open at the end of the previous line ----
    switch (mode) {
    case M_BLOCK:
        if (F.blockC)
            i = blockComment(0, 0, F.nest ? (pl ? (pl > kMaxDepth ? kMaxDepth : pl) : 1u) : 1u);
        break;
    case M_LUASTR:
        if (F.luaLong)
            i = luaLong(0, 0, pl, Token::String, M_LUASTR);
        break;
    case M_LUACMT:
        if (F.luaLong)
            i = luaLong(0, 0, pl, Token::Comment, M_LUACMT);
        break;
    case M_TRIPLE:
        if (F.triple && (pl & 3u) >= 1 && (pl & 3u) <= 2)
            i = tripleStr(0, 0, (pl & 3u) == 1 ? u'"' : u'\'', (pl & 4u) == 0);
        break;
    case M_RAW:
        if (F.rustRaw || F.swiftHash)
            i = rawStr(0, 0, pl & 0xFFu, (pl & 0x100u) != 0, true);
        break;
    case M_STR: {
        const uint32_t k = pl & 7u;
        if (k >= 1 && k <= 3)
            i = finishString(0, 0, quoteChar(k), (pl & 8u) != 0);
        break;
    }
    case M_STRCONT: {
        const uint32_t k = pl & 7u;
        if (k >= 1 && k <= 3)
            i = finishString(0, 0, quoteChar(k), false);
        break;
    }
    case M_PPCONT:
        if (F.preproc)
            inPre = true;
        break;
    case M_LCCONT:
        if (F.lineCommentCont) {
            em.add(0, n, Token::Comment);
            if (n > 0 && s[n - 1] == u'\\')
                out = packState(M_LCCONT, 0);
            return out;
        }
        break;
    case M_DELIM:
        if (F.cppRaw || F.dollarQuote)
            i = delimClose(0, 0, pl & 31u, (pl >> 5) & 0x3FFFFFu);
        break;
    case M_TPLTEXT:
        if (F.templ) {
            stack = (pl && pl < (1u << 22)) ? pl : 1u;
            i = tplRun(0, 0);
        }
        break;
    case M_TPLCODE:
        if (F.templ)
            stack = (pl && pl < (1u << 22)) ? pl : 1u;
        break;
    default:
        break;
    }
    if (i > 0)
        atStart = false;

    // ---- main loop ----
    while (i < n) {
        const char16_t c = s[i];
        if (isSpaceCh(c)) {
            ++i;
            continue;
        }
        const bool wasValue = prevValue;
        prevValue = false;
        const bool first = atStart;
        atStart = false;

        if (idStart(c)) {
            identifier(first);
            continue;
        }
        pendType = pendFunc = false;

        if (isDigit(c) || (c == u'.' && i + 1 < n && isDigit(s[i + 1]))) {
            const int e = scanNumber(i);
            em.add(i, e, Token::Number);
            i = e;
            prevValue = true;
            continue;
        }

        switch (c) {
        case u'"':
            i = openQuote(i, i, false);
            prevValue = true;
            break;
        case u'\'':
            if (F.rustLifetime) {
                int e = 0;
                if (rustChar(i, e)) {
                    em.add(i, e, Token::String);
                    i = e;
                    prevValue = true;
                } else if (i + 1 < n && idStart(s[i + 1])) {
                    const int e2 = identEnd(i + 1);
                    em.add(i, e2, Token::Type);
                    i = e2;
                    prevValue = true;
                } else {
                    em.add(i, i + 1, Token::Punctuation);
                    ++i;
                }
            } else if (F.sq) {
                i = openQuote(i, i, false);
                prevValue = true;
            } else {
                ++i;
            }
            break;
        case u'`':
            if (F.templ) {
                i = tplRun(i, i + 1);
                prevValue = true;
            } else if (F.backtickRaw || F.btIdent) {
                i = finishString(i, i + 1, u'`', true);
                prevValue = true;
            } else {
                ++i;
            }
            break;
        case u'/':
            if (F.blockC && i + 1 < n && s[i + 1] == u'*') {
                i = blockComment(i + 2, i, 1);
                prevValue = wasValue; // a comment does not change what came before it
                atStart = first;
            } else if (F.lineSlash && i + 1 < n && s[i + 1] == u'/') {
                em.add(i, n, Token::Comment);
                if (F.lineCommentCont && n > 0 && s[n - 1] == u'\\')
                    out = packState(M_LCCONT, 0);
                i = n;
            } else if (F.regex && !wasValue && !regexFailed) {
                int e = 0;
                if (tryRegex(e)) {
                    em.add(i, e, Token::String);
                    i = e;
                    prevValue = true;
                } else {
                    regexFailed = true;
                    em.add(i, i + 1, Token::Operator);
                    ++i;
                }
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'#':
            if (F.preproc && first && !inPre) {
                preprocessor();
            } else if (F.swiftHash) {
                int k = i;
                uint32_t h = 0;
                while (k < n && s[k] == u'#') {
                    ++k;
                    ++h;
                }
                if (k < n && s[k] == u'"') {
                    if (k + 2 < n && s[k + 1] == u'"' && s[k + 2] == u'"')
                        i = rawStr(k + 3, i, h > 255 ? 255 : h, true, true);
                    else
                        i = rawStr(k + 1, i, h > 255 ? 255 : h, false, false);
                    prevValue = true;
                } else if (h == 1 && k < n && idStart(s[k])) {
                    const int e = identEnd(k);
                    em.add(i, e, Token::Preprocessor);
                    i = e;
                } else {
                    i = k;
                }
            } else if (F.lineHash) {
                em.add(i, n, Token::Comment);
                i = n;
            } else if (F.rustAttr && i + 1 < n && (s[i + 1] == u'[' || (s[i + 1] == u'!' && i + 2 < n && s[i + 2] == u'['))) {
                int j = s[i + 1] == u'[' ? i + 2 : i + 3;
                int depth = 1;
                while (j < n && depth > 0) {
                    const char16_t d = s[j];
                    if (d == u'[')
                        ++depth;
                    else if (d == u']')
                        --depth;
                    else if (d == u'"') {
                        ++j;
                        while (j < n && s[j] != u'"')
                            j += (s[j] == u'\\') ? 2 : 1;
                    }
                    ++j;
                }
                if (j > n)
                    j = n;
                em.add(i, j, Token::Attribute);
                i = j;
            } else if (F.hashIdent && i + 1 < n && idStart(s[i + 1])) {
                i = identEnd(i + 1);
                prevValue = true;
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'-':
            if (F.lineDash && i + 1 < n && s[i + 1] == u'-') {
                uint32_t level = 0;
                int after = 0;
                if (F.luaLong && i + 2 < n && s[i + 2] == u'[' && luaOpen(i + 2, level, after)) {
                    i = luaLong(after, i, level, Token::Comment, M_LUACMT);
                } else {
                    em.add(i, n, Token::Comment);
                    i = n;
                }
            } else if (F.regex && wasValue && i + 1 < n && s[i + 1] == u'-') { // postfix --
                em.add(i, i + 2, Token::Operator);
                i += 2;
                prevValue = true;
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'[': {
            uint32_t level = 0;
            int after = 0;
            if (F.luaLong && luaOpen(i, level, after)) {
                i = luaLong(after, i, level, Token::String, M_LUASTR);
                prevValue = true;
            } else if (F.cppAttr && !attrFailed && i + 1 < n && s[i + 1] == u'[') {
                int e = i + 2;
                while (e + 1 < n && !(s[e] == u']' && s[e + 1] == u']'))
                    ++e;
                if (e + 1 < n) {
                    em.add(i, e + 2, Token::Attribute);
                    i = e + 2;
                } else {
                    attrFailed = true; // no closing ]] on this line: never rescan (keeps the pass linear)
                    em.add(i, i + 1, Token::Punctuation);
                    ++i;
                }
            } else {
                em.add(i, i + 1, Token::Punctuation);
                ++i;
            }
            break;
        }
        case u']':
        case u')':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            prevValue = true;
            break;
        case u'{':
            if (F.templ && stack > 1 && stack < kMaxStack)
                stack <<= 1;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u'}':
            if (F.templ && stack > 1) {
                const bool top = (stack & 1u) != 0;
                stack >>= 1;
                em.add(i, i + 1, Token::Punctuation);
                ++i;
                if (top) {
                    i = tplRun(i, i);
                    prevValue = true;
                } else {
                    prevValue = true;
                }
            } else {
                em.add(i, i + 1, Token::Punctuation);
                ++i;
                prevValue = true;
            }
            break;
        case u'@': {
            if (F.csStr && i + 1 < n && s[i + 1] == u'"') {
                i = openQuote(i, i + 1, true);
                prevValue = true;
            } else if (F.csStr && i + 1 < n && idStart(s[i + 1])) {
                i = identEnd(i + 1);
                prevValue = true;
            } else if (F.pyDecor && first && i + 1 < n && idStart(s[i + 1])) {
                int e = i + 1;
                while (e < n && (idPart(s[e]) || s[e] == u'.'))
                    ++e;
                em.add(i, e, Token::Attribute);
                i = e;
            } else if (F.atAttr && i + 1 < n && idStart(s[i + 1])) {
                int e = i + 1;
                while (e < n && (idPart(s[e]) || s[e] == u'.'))
                    ++e;
                em.add(i, e, Token::Attribute);
                i = e;
            } else if (F.sqlStr) {
                int e = i + 1;
                if (e < n && s[e] == u'@')
                    ++e;
                const int e2 = identEnd(e);
                if (e2 > e) {
                    em.add(i, e2, Token::Variable);
                    i = e2;
                    prevValue = true;
                } else {
                    em.add(i, i + 1, Token::Operator);
                    ++i;
                }
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        }
        case u'$': {
            if (F.csStr) {
                int k = i;
                bool verb = false;
                while (k < n && k - i < 4 && (s[k] == u'$' || s[k] == u'@')) {
                    verb |= (s[k] == u'@');
                    ++k;
                }
                if (k < n && s[k] == u'"') {
                    i = openQuote(i, k, verb);
                    prevValue = true;
                } else {
                    ++i;
                }
            } else if (F.dollarQuote) {
                uint32_t len = 0, hash = 0;
                int after = 0;
                if (dollarOpen(i, len, hash, after)) {
                    i = delimClose(after, i, len, hash);
                    prevValue = true;
                } else if (i + 1 < n && (isDigit(s[i + 1]) || idStart(s[i + 1]))) {
                    const int e = identEnd(i + 1);
                    em.add(i, e, Token::Variable);
                    i = e;
                    prevValue = true;
                } else {
                    ++i;
                }
            } else if (F.dollarVar && i + 1 < n && idPart(s[i + 1])) {
                const int e = identEnd(i + 1);
                em.add(i, e, Token::Variable);
                i = e;
                prevValue = true;
            } else {
                ++i;
            }
            break;
        }
        case u':':
            if (F.sqlStr && i + 1 < n && idStart(s[i + 1]) && (i == 0 || s[i - 1] != u':') && s[i + 1] != u':') {
                const int e = identEnd(i + 1);
                em.add(i, e, Token::Variable);
                i = e;
                prevValue = true;
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'+':
            if (F.regex && wasValue && i + 1 < n && s[i + 1] == u'+') { // postfix ++ keeps the value
                em.add(i, i + 2, Token::Operator);
                i += 2;
                prevValue = true;
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'!':
        case u'*':
        case u'%':
        case u'=':
        case u'<':
        case u'>':
        case u'&':
        case u'|':
        case u'^':
        case u'~':
        case u'?':
            em.add(i, i + 1, Token::Operator);
            ++i;
            break;
        case u'(':
        case u',':
        case u';':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u'.':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        default:
            ++i;
            break;
        }
    }

    if (out == 0) {
        if (tplEol)
            out = packState(M_TPLTEXT, stack);
        else if (stack > 1)
            out = packState(M_TPLCODE, stack);
        else if (inPre && n > 0 && s[n - 1] == u'\\')
            out = packState(M_PPCONT, 0);
    }
    return out;
}

} // namespace

int lexCLike(const Language &L, QStringView line, int state, Emit &em)
{
    CLex lx(L, line, em);
    return lx.run(state);
}

} // namespace folia::code
