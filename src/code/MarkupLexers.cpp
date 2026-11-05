// css and html/xml lexers.
//
// CSS carry state: mode 0 normal / 1 inside a comment; payload =
//   ((blockStack - 1) << 2) | preludeIsGroupingAtRule << 1 | inValue
// where blockStack is a sentinel-prefixed bit stack (1 = grouping at-rule
// block such as @media, 0 = declaration block).
//
// HTML carry state: see the H_* enum. <script> and <style> bodies are handed to
// the javascript / css lexers; their carry state lives in the payload.
#include "code/LangInternal.h"

namespace folia::code {

namespace {

// ===========================================================================
// CSS
// ===========================================================================
class CssLex {
public:
    CssLex(QStringView line, Emit &sink) : s(line.utf16()), n(int(line.size())), em(sink) {}
    int run(int stateIn);

private:
    const char16_t *s;
    const int n;
    Emit &em;
    int i = 0;
    uint32_t stack = 1;
    bool inValue = false;
    bool preRule = false;
    bool inAt = false;
    bool inBracket = false;
    bool inComment = false;
    int paren = 0;

    bool ctxDecl() const { return stack > 1 && (stack & 1u) == 0; }
    static bool identChar(char16_t c) { return isAlnum(c) || c == u'-' || c == u'_' || c >= 0x80 || c == u'\\'; }
    bool identStartAt(int j) const
    {
        if (j >= n)
            return false;
        const char16_t c = s[j];
        if (isAsciiAlpha(c) || c == u'_' || c >= 0x80 || c == u'\\')
            return true;
        if (c == u'-' && j + 1 < n)
            return isAsciiAlpha(s[j + 1]) || s[j + 1] == u'_' || s[j + 1] == u'-' || s[j + 1] >= 0x80;
        return false;
    }
    int identEnd(int j) const
    {
        while (j < n && identChar(s[j])) {
            if (s[j] == u'\\')
                ++j;
            ++j;
        }
        return j > n ? n : j;
    }
    bool numberAt(int j) const
    {
        if (j >= n)
            return false;
        const char16_t c = s[j];
        if (isDigit(c))
            return true;
        if (c == u'.')
            return j + 1 < n && isDigit(s[j + 1]);
        if ((c == u'+' || c == u'-') && j + 1 < n)
            return isDigit(s[j + 1]) || (s[j + 1] == u'.' && j + 2 < n && isDigit(s[j + 2]));
        return false;
    }
    int scanNumber(int j) const
    {
        if (j < n && (s[j] == u'+' || s[j] == u'-'))
            ++j;
        while (j < n && isDigit(s[j]))
            ++j;
        if (j + 1 < n && s[j] == u'.' && isDigit(s[j + 1])) {
            ++j;
            while (j < n && isDigit(s[j]))
                ++j;
        }
        if (j < n && (s[j] == u'e' || s[j] == u'E')) {
            int k = j + 1;
            if (k < n && (s[k] == u'+' || s[k] == u'-'))
                ++k;
            if (k < n && isDigit(s[k])) {
                j = k;
                while (j < n && isDigit(s[j]))
                    ++j;
            }
        }
        while (j < n && (isAsciiAlpha(s[j]) || s[j] == u'%'))
            ++j;
        return j;
    }
    int skipWs(int j) const
    {
        while (j < n && isSpaceCh(s[j]))
            ++j;
        return j;
    }
    void comment(int from, int tokStart)
    {
        int j = from;
        while (j + 1 < n && !(s[j] == u'*' && s[j + 1] == u'/'))
            ++j;
        if (j + 1 < n) {
            em.add(tokStart, j + 2, Token::Comment);
            i = j + 2;
        } else {
            em.add(tokStart, n, Token::Comment);
            inComment = true;
            i = n;
        }
    }
    void value();
    void declStart();
    void selector();
};

void CssLex::value()
{
    const char16_t c = s[i];
    if (numberAt(i)) {
        const int e = scanNumber(i);
        em.add(i, e, Token::Number);
        i = e;
        return;
    }
    if (c == u'#') {
        int e = i + 1;
        while (e < n && isAlnum(s[e]))
            ++e;
        em.add(i, e, Token::Number);
        i = e > i ? e : i + 1;
        return;
    }
    if (c == u'!') {
        int e = skipWs(i + 1);
        e = identEnd(e);
        em.add(i, e, Token::Keyword);
        i = e;
        return;
    }
    if (identStartAt(i)) {
        const int e = identEnd(i);
        if (e < n && s[e] == u'(') {
            em.add(i, e, Token::Function);
            i = e;
            return;
        }
        em.add(i, e, (e - i >= 2 && s[i] == u'-' && s[i + 1] == u'-') ? Token::Variable : Token::Constant);
        i = e;
        return;
    }
    switch (c) {
    case u'(':
    case u')':
    case u',':
    case u':':
        em.add(i, i + 1, Token::Punctuation);
        break;
    case u'/':
    case u'*':
    case u'+':
    case u'-':
    case u'=':
    case u'>':
    case u'<':
        em.add(i, i + 1, Token::Operator);
        break;
    default:
        break;
    }
    ++i;
}

void CssLex::declStart()
{
    if (identStartAt(i)) {
        const int e = identEnd(i);
        const int k = skipWs(e);
        if (k < n && s[k] == u':' && !(k + 1 < n && s[k + 1] == u':')) {
            const bool custom = e - i >= 2 && s[i] == u'-' && s[i + 1] == u'-';
            em.add(i, e, custom ? Token::Variable : Token::Attribute);
            em.add(k, k + 1, Token::Punctuation);
            i = k + 1;
            inValue = true;
            return;
        }
        em.add(i, e, Token::Tag);
        i = e;
        return;
    }
    selector();
}

void CssLex::selector()
{
    const char16_t c = s[i];
    if (numberAt(i) && c != u'+' && c != u'-') {
        const int e = scanNumber(i);
        em.add(i, e, Token::Number);
        i = e;
        return;
    }
    switch (c) {
    case u'@': {
        int e = i + 1;
        while (e < n && identChar(s[e]))
            ++e;
        em.add(i, e, Token::Keyword);
        const int len = e - (i + 1);
        const char16_t *w = s + i + 1;
        if (equalsAsciiCI(w, len, "media") || equalsAsciiCI(w, len, "supports") || equalsAsciiCI(w, len, "layer") ||
            equalsAsciiCI(w, len, "container") || equalsAsciiCI(w, len, "document") ||
            equalsAsciiCI(w, len, "scope") || equalsAsciiCI(w, len, "starting-style") ||
            equalsAsciiCI(w, len, "-moz-document"))
            preRule = true;
        inAt = true;
        i = e;
        return;
    }
    case u'.':
        if (identStartAt(i + 1)) {
            const int e = identEnd(i + 1);
            em.add(i, e, Token::Type);
            i = e;
            return;
        }
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u'#':
        if (i + 1 < n && identChar(s[i + 1])) {
            const int e = identEnd(i + 1);
            em.add(i, e, Token::Constant);
            i = e;
            return;
        }
        ++i;
        return;
    case u':': {
        int j = i;
        while (j < n && s[j] == u':')
            ++j;
        const int e = identEnd(j);
        if (e > j) {
            em.add(i, e, Token::Builtin);
            i = e;
        } else {
            em.add(i, j, Token::Punctuation);
            i = j;
        }
        return;
    }
    case u'[':
        inBracket = true;
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u']':
        inBracket = false;
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u'(':
        ++paren;
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u')':
        if (paren > 0)
            --paren;
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u',':
        em.add(i, i + 1, Token::Punctuation);
        ++i;
        return;
    case u'>':
    case u'+':
    case u'~':
    case u'*':
    case u'&':
    case u'|':
    case u'^':
    case u'$':
    case u'=':
    case u'!':
        em.add(i, i + 1, Token::Operator);
        ++i;
        return;
    default:
        break;
    }
    if (identStartAt(i)) {
        const int e = identEnd(i);
        if (inBracket) {
            em.add(i, e, Token::Attribute);
        } else if (inAt) {
            const int k = skipWs(e);
            const int len = e - i;
            if (k < n && s[k] == u':' && paren > 0)
                em.add(i, e, Token::Attribute);
            else if (equalsAsciiCI(s + i, len, "and") || equalsAsciiCI(s + i, len, "or") ||
                     equalsAsciiCI(s + i, len, "not") || equalsAsciiCI(s + i, len, "only"))
                em.add(i, e, Token::Keyword);
            else if (e < n && s[e] == u'(')
                em.add(i, e, Token::Function);
        } else {
            em.add(i, e, Token::Tag);
        }
        i = e;
        return;
    }
    ++i;
}

int CssLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    const uint32_t mode = st & 15u;
    const uint32_t pl = st >> 4;
    stack = (pl >> 2) + 1;
    if (stack >= (1u << 20))
        stack = 1;
    inValue = (pl & 1u) != 0;
    preRule = (pl & 2u) != 0;

    if (mode == 1)
        comment(0, 0);

    while (i < n) {
        const char16_t c = s[i];
        if (isSpaceCh(c)) {
            ++i;
            continue;
        }
        if (c == u'/' && i + 1 < n && s[i + 1] == u'*') {
            comment(i + 2, i);
            continue;
        }
        switch (c) {
        case u'{': {
            const bool grouping = !ctxDecl() && preRule;
            if (stack < (1u << 19))
                stack = (stack << 1) | (grouping ? 1u : 0u);
            inValue = preRule = inAt = false;
            inBracket = false;
            paren = 0;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        }
        case u'}':
            if (stack > 1)
                stack >>= 1;
            inValue = preRule = inAt = false;
            inBracket = false;
            paren = 0;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u';':
            inValue = preRule = inAt = false;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u'"':
        case u'\'': {
            int j = i + 1;
            while (j < n && s[j] != c)
                j += (s[j] == u'\\') ? 2 : 1;
            const int e = j < n ? j + 1 : n;
            em.add(i, e, Token::String);
            i = e;
            break;
        }
        default:
            if (ctxDecl()) {
                if (inValue) {
                    // url( unquoted ) : treat the argument as a string
                    if (identStartAt(i)) {
                        const int e = identEnd(i);
                        if (e - i == 3 && equalsAsciiCI(s + i, 3, "url") && e < n && s[e] == u'(') {
                            em.add(i, e, Token::Function);
                            em.add(e, e + 1, Token::Punctuation);
                            int j = skipWs(e + 1);
                            i = e + 1;
                            if (j < n && s[j] != u'"' && s[j] != u'\'') {
                                int k = j;
                                while (k < n && s[k] != u')')
                                    ++k;
                                em.add(j, k, Token::String);
                                i = k;
                            }
                            break;
                        }
                    }
                    value();
                } else {
                    declStart();
                }
            } else {
                selector();
            }
            break;
        }
    }

    const uint32_t payload = ((stack - 1) << 2) | (preRule ? 2u : 0u) | (inValue ? 1u : 0u);
    return packState(inComment ? 1u : 0u, payload);
}

// ===========================================================================
// HTML / XML
// ===========================================================================
enum : uint32_t {
    H_TEXT = 0,
    H_COMMENT,  // <!-- ... -->
    H_TAG,      // inside a tag, between attributes ; payload = kind
    H_DQ,       // inside a double-quoted attribute value ; payload = kind
    H_SQ,       // inside a single-quoted attribute value ; payload = kind
    H_CDATA,    // <![CDATA[ ... ]]>
    H_DECL,     // <!DOCTYPE ...> (payload 0) or <? ... ?> (payload 1)
    H_SCRIPT,   // <script> body ; payload = javascript carry state
    H_STYLE,    // <style> body  ; payload = css carry state
};

class HtmlLex {
public:
    HtmlLex(QStringView line, Emit &sink) : s(line.utf16()), n(int(line.size())), em(sink) {}
    int run(int stateIn);

private:
    const char16_t *s;
    const int n;
    Emit &em;
    int i = 0;
    int out = 0;
    int bodyKind = 0;

    int find(int from, const char16_t a, const char16_t b = 0, const char16_t c = 0) const
    {
        for (int j = from; j < n; ++j) {
            if (s[j] != a)
                continue;
            if (b && !(j + 1 < n && s[j + 1] == b))
                continue;
            if (c && !(j + 2 < n && s[j + 2] == c))
                continue;
            return j;
        }
        return -1;
    }
    static bool nameChar(char16_t c) { return isAlnum(c) || c == u':' || c == u'_' || c == u'-' || c == u'.' || c >= 0x80; }
    int attrs(int j, int kind);
    int body(int from, int kind, uint32_t inner);
    int quoted(int from, char16_t q, int kind);
};

int HtmlLex::attrs(int j, int kind)
{
    bool afterEq = false;
    while (j < n) {
        const char16_t c = s[j];
        if (isSpaceCh(c)) {
            ++j;
            continue;
        }
        if (c == u'>') {
            em.add(j, j + 1, Token::Punctuation);
            bodyKind = kind;
            return j + 1;
        }
        if (c == u'/' && j + 1 < n && s[j + 1] == u'>') {
            em.add(j, j + 2, Token::Punctuation);
            return j + 2;
        }
        if (c == u'"' || c == u'\'') {
            j = quoted(j + 1, c, kind);
            if (out)
                return n; // value continues on the next line
            afterEq = false;
            continue;
        }
        if (c == u'=') {
            em.add(j, j + 1, Token::Operator);
            ++j;
            afterEq = true;
            continue;
        }
        if (c == u'<')
            return j;
        int e = j;
        while (e < n) {
            const char16_t d = s[e];
            if (isSpaceCh(d) || d == u'=' || d == u'>' || d == u'"' || d == u'\'' || d == u'<' ||
                (d == u'/' && e + 1 < n && s[e + 1] == u'>'))
                break;
            ++e;
        }
        if (e == j) {
            ++j;
            continue;
        }
        em.add(j, e, afterEq ? Token::String : Token::Attribute);
        afterEq = false;
        j = e;
    }
    out = packState(H_TAG, uint32_t(kind));
    return n;
}

// Scans a quoted attribute value whose opening quote is at from-1 (or the
// continuation from column 0). Emits the String span. Returns the index after
// the closing quote; if unterminated sets `out` and returns n.
int HtmlLex::quoted(int from, char16_t q, int kind)
{
    const int tokStart = from > 0 ? from - 1 : 0;
    int e = from;
    while (e < n && s[e] != q)
        ++e;
    if (e < n) {
        em.add(tokStart, e + 1, Token::String);
        return e + 1;
    }
    em.add(tokStart, n, Token::String);
    out = packState(q == u'"' ? H_DQ : H_SQ, uint32_t(kind));
    return n;
}

int HtmlLex::body(int from, int kind, uint32_t inner)
{
    const char *tag = kind == 1 ? "script" : "style";
    const int tl = kind == 1 ? 6 : 5;
    int close = -1;
    for (int j = from; j + 1 < n; ++j) {
        if (s[j] == u'<' && s[j + 1] == u'/' && j + 2 + tl <= n && equalsAsciiCI(s + j + 2, tl, tag) &&
            (j + 2 + tl >= n || isSpaceCh(s[j + 2 + tl]) || s[j + 2 + tl] == u'>' || s[j + 2 + tl] == u'/')) {
            close = j;
            break;
        }
    }
    const int end = close < 0 ? n : close;
    int innerOut = 0;
    if (end > from || inner != 0) {
        const Language *lg = languageById(kind == 1 ? Lang::JavaScript : Lang::Css);
        Emit sub{em.out, em.base + from, em.floor};
        innerOut = lg->lex(*lg, QStringView(s + from, end - from), int(inner), sub);
    }
    if (close < 0) {
        out = packState(kind == 1 ? H_SCRIPT : H_STYLE, uint32_t(innerOut));
        return n;
    }
    return close;
}

int HtmlLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    const uint32_t mode = st & 15u;
    const uint32_t pl = st >> 4;

    switch (mode) {
    case H_COMMENT: {
        const int e = find(0, u'-', u'-', u'>');
        if (e >= 0) {
            em.add(0, e + 3, Token::Comment);
            i = e + 3;
        } else {
            em.add(0, n, Token::Comment);
            return packState(H_COMMENT, 0);
        }
        break;
    }
    case H_CDATA: {
        const int e = find(0, u']', u']', u'>');
        if (e >= 0) {
            em.add(0, e + 3, Token::String);
            i = e + 3;
        } else {
            em.add(0, n, Token::String);
            return packState(H_CDATA, 0);
        }
        break;
    }
    case H_DECL: {
        const int e = (pl & 1u) ? find(0, u'?', u'>') : find(0, u'>');
        if (e >= 0) {
            const int end = e + ((pl & 1u) ? 2 : 1);
            em.add(0, end, Token::Preprocessor);
            i = end;
        } else {
            em.add(0, n, Token::Preprocessor);
            return packState(H_DECL, pl & 1u);
        }
        break;
    }
    case H_TAG:
        i = attrs(0, int(pl & 3u));
        break;
    case H_DQ:
    case H_SQ: {
        i = quoted(0, mode == H_DQ ? u'"' : u'\'', int(pl & 3u));
        if (!out)
            i = attrs(i, int(pl & 3u));
        break;
    }
    case H_SCRIPT:
        i = body(0, 1, pl);
        break;
    case H_STYLE:
        i = body(0, 2, pl);
        break;
    default:
        break;
    }
    if (bodyKind && !out) {
        const int k = bodyKind;
        bodyKind = 0;
        i = body(i, k, 0);
    }

    while (i < n && !out) {
        const char16_t c = s[i];
        if (c == u'<') {
            if (i + 1 >= n) {
                ++i;
                continue;
            }
            const char16_t d = s[i + 1];
            if (d == u'!') {
                if (i + 3 < n && s[i + 2] == u'-' && s[i + 3] == u'-') {
                    const int e = find(i + 4, u'-', u'-', u'>');
                    if (e >= 0) {
                        em.add(i, e + 3, Token::Comment);
                        i = e + 3;
                    } else {
                        em.add(i, n, Token::Comment);
                        out = packState(H_COMMENT, 0);
                        i = n;
                    }
                } else if (i + 8 < n && equalsAscii(s + i + 2, 7, "[CDATA[")) {
                    const int e = find(i + 9, u']', u']', u'>');
                    if (e >= 0) {
                        em.add(i, e + 3, Token::String);
                        i = e + 3;
                    } else {
                        em.add(i, n, Token::String);
                        out = packState(H_CDATA, 0);
                        i = n;
                    }
                } else {
                    const int e = find(i + 2, u'>');
                    if (e >= 0) {
                        em.add(i, e + 1, Token::Preprocessor);
                        i = e + 1;
                    } else {
                        em.add(i, n, Token::Preprocessor);
                        out = packState(H_DECL, 0);
                        i = n;
                    }
                }
            } else if (d == u'?') {
                const int e = find(i + 2, u'?', u'>');
                if (e >= 0) {
                    em.add(i, e + 2, Token::Preprocessor);
                    i = e + 2;
                } else {
                    em.add(i, n, Token::Preprocessor);
                    out = packState(H_DECL, 1);
                    i = n;
                }
            } else if (isAsciiAlpha(d) || d == u'_' || (d == u'/' && i + 2 < n && (isAsciiAlpha(s[i + 2]) || s[i + 2] == u'_'))) {
                const bool closing = d == u'/';
                const int ns = i + (closing ? 2 : 1);
                int e = ns;
                while (e < n && nameChar(s[e]))
                    ++e;
                em.add(i, ns, Token::Punctuation);
                em.add(ns, e, Token::Tag);
                int kind = 0;
                if (!closing) {
                    if (e - ns == 6 && equalsAsciiCI(s + ns, 6, "script"))
                        kind = 1;
                    else if (e - ns == 5 && equalsAsciiCI(s + ns, 5, "style"))
                        kind = 2;
                }
                i = attrs(e, kind);
                if (bodyKind && !out) {
                    const int k = bodyKind;
                    bodyKind = 0;
                    i = body(i, k, 0);
                }
            } else {
                ++i;
            }
        } else if (c == u'&') {
            int e = i + 1;
            while (e < n && e - i <= 12 && (isAlnum(s[e]) || (e == i + 1 && s[e] == u'#')))
                ++e;
            if (e < n && s[e] == u';' && e > i + 1) {
                em.add(i, e + 1, Token::Constant);
                i = e + 1;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace

int lexCss(const Language &, QStringView line, int state, Emit &em)
{
    CssLex lx(line, em);
    return lx.run(state);
}

int lexHtml(const Language &, QStringView line, int state, Emit &em)
{
    HtmlLex lx(line, em);
    return lx.run(state);
}

} // namespace folia::code
