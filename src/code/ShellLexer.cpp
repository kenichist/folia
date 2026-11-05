// bash / sh / zsh lexer. Command-position tracking (first word of a simple
// command), $variables inside double quotes, multi-line quotes, heredocs
// (delimiter fingerprint in the state) and backslash line continuation.
//
// Carry state: low 4 bits mode, payload above.
#include "code/LangInternal.h"

namespace folia::code {

namespace {

enum : uint32_t {
    B_NORMAL = 0,
    B_DQ,       // inside "..."
    B_SQ,       // inside '...'
    B_ANSI,     // inside $'...'
    B_HEREDOC,  // payload = len(5) | tabs<<5 | hash<<6
    B_CONT,     // previous line ended with a backslash
};

class BashLex {
public:
    BashLex(const Language &lang, QStringView line, Emit &sink)
        : L(lang), s(line.utf16()), n(int(line.size())), em(sink)
    {
    }
    int run(int stateIn);

private:
    const Language &L;
    const char16_t *s;
    const int n;
    Emit &em;
    int i = 0;
    int out = 0;
    bool cmdPos = true;
    bool pendFunc = false;
    bool inBacktick = false;
    bool commentEnd = false;
    int arith = 0;
    int parenDepth = 0;
    int leadingWs = 0; // index of the first non-blank character
    bool hdPending = false;
    uint32_t hdLen = 0, hdHash = 0;
    bool hdTabs = false;

    static bool isDelim(char16_t c)
    {
        switch (c) {
        case u' ': case u'\t': case u'\r': case u'\n': case u'|': case u'&': case u';': case u'(': case u')':
        case u'<': case u'>': case u'"': case u'\'': case u'`': case u'$':
            return true;
        default:
            return false;
        }
    }
    int varEnd(int j) const;
    int dqBody(int from, int tokStart, bool ansi);
    int sqBody(int from, int tokStart);
    void word();
    bool heredocOpen();
};

int BashLex::varEnd(int j) const
{
    int k = j + 1;
    if (k >= n)
        return j;
    const char16_t c = s[k];
    if (c == u'{') {
        int depth = 1;
        ++k;
        while (k < n && depth > 0) {
            if (s[k] == u'{')
                ++depth;
            else if (s[k] == u'}')
                --depth;
            ++k;
        }
        return k;
    }
    if (isAsciiAlpha(c) || c == u'_') {
        while (k < n && (isAlnum(s[k]) || s[k] == u'_'))
            ++k;
        return k;
    }
    if (isDigit(c))
        return k + 1;
    if (c == u'@' || c == u'*' || c == u'#' || c == u'?' || c == u'$' || c == u'!' || c == u'-')
        return k + 1;
    return j;
}

int BashLex::dqBody(int from, int tokStart, bool ansi)
{
    int seg = tokStart;
    int j = from;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\') {
            j += 2;
            continue;
        }
        if ((!ansi && c == u'"') || (ansi && c == u'\'')) {
            ++j;
            em.add(seg, j, Token::String);
            return j;
        }
        if (!ansi && c == u'$') {
            const int e = varEnd(j);
            if (e > j) {
                em.add(seg, j, Token::String);
                em.add(j, e, Token::Variable);
                seg = e;
                j = e;
                continue;
            }
        }
        ++j;
    }
    if (seg < n)
        em.add(seg, n, Token::String);
    out = packState(ansi ? B_ANSI : B_DQ, 0);
    return n;
}

int BashLex::sqBody(int from, int tokStart)
{
    int j = from;
    while (j < n && s[j] != u'\'')
        ++j;
    if (j < n) {
        em.add(tokStart, j + 1, Token::String);
        return j + 1;
    }
    em.add(tokStart, n, Token::String);
    out = packState(B_SQ, 0);
    return n;
}

// s[i] == '<', s[i+1] == '<'. Returns true if a heredoc operator + delimiter was consumed.
bool BashLex::heredocOpen()
{
    int j = i + 2;
    bool tabs = false;
    if (j < n && s[j] == u'-') {
        tabs = true;
        ++j;
    }
    while (j < n && (s[j] == u' ' || s[j] == u'\t'))
        ++j;
    char16_t q = 0;
    if (j < n && (s[j] == u'\'' || s[j] == u'"')) {
        q = s[j];
        ++j;
    }
    const int d0 = j;
    while (j < n && (isAlnum(s[j]) || s[j] == u'_' || s[j] == u'-' || s[j] == u'.' || s[j] >= 0x80))
        ++j;
    const int len = j - d0;
    if (len < 1 || len > 31)
        return false;
    int end = j;
    if (q) {
        if (j >= n || s[j] != q)
            return false;
        end = j + 1;
    }
    if (isDigit(s[d0]) && len == 1 && !q) // `1 << 2` style arithmetic, not a heredoc
        return false;
    em.add(i, d0 - (q ? 1 : 0), Token::Operator);
    em.add(d0 - (q ? 1 : 0), end, Token::Constant);
    hdPending = true;
    hdLen = uint32_t(len);
    hdHash = hashUnits(s + d0, len) & 0x1FFFFFu;
    hdTabs = tabs;
    i = end;
    return true;
}

void BashLex::word()
{
    const int a = i;
    int j = i;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u'\\') {
            if (j + 1 >= n) {
                break;
            }
            j += 2;
            continue;
        }
        if (isDelim(c) || c == u'=')
            break;
        ++j;
    }
    if (j == a) { // lone trailing backslash or similar
        ++i;
        return;
    }
    const int len = j - a;
    i = j;

    // assignment NAME= / NAME+=
    if (i < n && s[i] == u'=') {
        int e = j;
        if (e > a && s[e - 1] == u'+')
            --e;
        bool ok = e > a && (isAsciiAlpha(s[a]) || s[a] == u'_');
        for (int k = a; ok && k < e; ++k)
            ok = isAlnum(s[k]) || s[k] == u'_';
        if (ok) {
            em.add(a, e, Token::Variable);
            em.add(e, e + (i - e), Token::Operator); // optional '+'
            em.add(i, i + 1, Token::Operator);
            ++i;
            pendFunc = false;
            return; // cmdPos unchanged: FOO=1 cmd
        }
    }

    // classification
    bool allDigits = true;
    for (int k = a; k < j; ++k)
        if (!isDigit(s[k])) {
            allDigits = false;
            break;
        }

    if (pendFunc) {
        pendFunc = false;
        em.add(a, j, Token::Function);
        cmdPos = false;
        return;
    }
    if (cmdPos) {
        const uint8_t info = L.kw.find(s + a, len);
        if (info) {
            const Token t = Token(info & kInfoToken);
            em.add(a, j, t);
            pendFunc = (info & kNextFunc) != 0;
            if (t == Token::Keyword) {
                cmdPos = equalsAscii(s + a, len, "if") || equalsAscii(s + a, len, "then") ||
                         equalsAscii(s + a, len, "else") || equalsAscii(s + a, len, "elif") ||
                         equalsAscii(s + a, len, "while") || equalsAscii(s + a, len, "until") ||
                         equalsAscii(s + a, len, "do") || equalsAscii(s + a, len, "!") ||
                         equalsAscii(s + a, len, "{") || equalsAscii(s + a, len, "time") ||
                         equalsAscii(s + a, len, "coproc");
            } else {
                cmdPos = false;
            }
            return;
        }
        if (allDigits) {
            em.add(a, j, Token::Number);
            cmdPos = false;
            return;
        }
        em.add(a, j, Token::Function);
        cmdPos = false;
        return;
    }
    if ((len == 2 && equalsAscii(s + a, 2, "in")) || (len == 4 && equalsAscii(s + a, 4, "esac"))) {
        em.add(a, j, Token::Keyword);
        return;
    }
    if (len == 1 && (s[a] == u'{' || s[a] == u'}')) { // function body / group braces
        em.add(a, j, Token::Keyword);
        cmdPos = s[a] == u'{';
        return;
    }
    if (allDigits || (len > 1 && s[a] == u'-' && [&] {
            for (int k = a + 1; k < j; ++k)
                if (!isDigit(s[k]))
                    return false;
            return true;
        }())) {
        em.add(a, j, Token::Number);
        return;
    }
    if (s[a] == u'-' && len > 1) {
        em.add(a, j, Token::Attribute);
        return;
    }
    // `]]` closes a test; `]` closes `[ ... ]`
    if (len == 2 && equalsAscii(s + a, 2, "]]")) {
        em.add(a, j, Token::Keyword);
        return;
    }
    if (len == 1 && s[a] == u']')
        em.add(a, j, Token::Builtin);
}

int BashLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    const uint32_t mode = st & 15u;
    const uint32_t pl = st >> 4;

    switch (mode) {
    case B_DQ:
        i = dqBody(0, 0, false);
        cmdPos = false;
        break;
    case B_ANSI:
        i = dqBody(0, 0, true);
        cmdPos = false;
        break;
    case B_SQ:
        i = sqBody(0, 0);
        cmdPos = false;
        break;
    case B_CONT:
        cmdPos = false;
        break;
    case B_HEREDOC: {
        const uint32_t len = pl & 31u;
        const bool tabs = ((pl >> 5) & 1u) != 0;
        int a = 0;
        int e = n;
        if (tabs)
            while (a < e && s[a] == u'\t')
                ++a;
        while (e > a && s[e - 1] == u'\r')
            --e;
        em.add(0, n, Token::String);
        if (uint32_t(e - a) == len && len > 0 && (hashUnits(s + a, e - a) & 0x1FFFFFu) == (pl >> 6))
            return 0;
        return packState(B_HEREDOC, pl & 0x7FFFFFFu);
    }
    default:
        break;
    }

    while (leadingWs < n && (s[leadingWs] == u' ' || s[leadingWs] == u'\t'))
        ++leadingWs;
    while (i < n) {
        const char16_t c = s[i];
        if (c == u' ' || c == u'\t' || c == u'\r' || c == u'\n') {
            ++i;
            continue;
        }
        switch (c) {
        case u'#':
            em.add(i, n, Token::Comment);
            i = n;
            commentEnd = true;
            break;
        case u'"':
            i = dqBody(i + 1, i, false);
            cmdPos = false;
            break;
        case u'\'':
            i = sqBody(i + 1, i);
            cmdPos = false;
            break;
        case u'`':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            inBacktick = !inBacktick;
            cmdPos = inBacktick;
            break;
        case u'$': {
            if (i == leadingWs && (i + 1 >= n || s[i + 1] == u' ' || s[i + 1] == u'\t')) {
                // `$ command` : a console-session prompt, the command follows
                em.add(i, i + 1, Token::Operator);
                ++i;
                cmdPos = true;
            } else if (i + 1 < n && s[i + 1] == u'(') {
                if (i + 2 < n && s[i + 2] == u'(') {
                    em.add(i, i + 3, Token::Punctuation);
                    i += 3;
                    ++arith;
                    cmdPos = false;
                } else {
                    em.add(i, i + 2, Token::Punctuation);
                    i += 2;
                    ++parenDepth;
                    cmdPos = true;
                }
            } else if (i + 1 < n && s[i + 1] == u'\'') {
                i = dqBody(i + 2, i, true);
                cmdPos = false;
            } else if (i + 1 < n && s[i + 1] == u'"') {
                i = dqBody(i + 2, i, false);
                cmdPos = false;
            } else {
                const int e = varEnd(i);
                if (e > i) {
                    em.add(i, e, Token::Variable);
                    i = e;
                } else {
                    ++i;
                }
                cmdPos = false;
            }
            break;
        }
        case u';':
            if (i + 1 < n && s[i + 1] == u';') { // `;;` ends a case branch: a pattern follows
                em.add(i, i + 2, Token::Operator);
                i += 2;
                cmdPos = false;
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
                cmdPos = (arith == 0);
            }
            pendFunc = false;
            break;
        case u'|':
        case u'&':
            em.add(i, i + 1, Token::Operator);
            ++i;
            cmdPos = (arith == 0);
            pendFunc = false;
            break;
        case u'<':
            if (i + 2 < n && s[i + 1] == u'<' && s[i + 2] == u'<') {
                em.add(i, i + 3, Token::Operator);
                i += 3;
            } else if (i + 1 < n && s[i + 1] == u'<' && arith == 0 && !hdPending && heredocOpen()) {
                // consumed
            } else {
                em.add(i, i + 1, Token::Operator);
                ++i;
            }
            break;
        case u'>':
            em.add(i, i + 1, Token::Operator);
            ++i;
            break;
        case u'(':
            em.add(i, i + 1, Token::Punctuation);
            if (i + 1 < n && s[i + 1] == u'(') {
                em.add(i + 1, i + 2, Token::Punctuation);
                ++arith;
                ++i;
                cmdPos = false;
            } else {
                ++parenDepth;
                cmdPos = !(i > 0 && s[i - 1] == u'=');
            }
            ++i;
            break;
        case u')':
            em.add(i, i + 1, Token::Punctuation);
            if (arith > 0 && i + 1 < n && s[i + 1] == u')') {
                em.add(i + 1, i + 2, Token::Punctuation);
                --arith;
                ++i;
                cmdPos = false;
            } else if (parenDepth > 0) {
                --parenDepth;
                cmdPos = false;
            } else {
                cmdPos = (arith == 0); // `pattern)` closes a case label: a command follows
            }
            ++i;
            break;
        case u'=': {
            int j = i;
            while (j < n && (s[j] == u'=' || s[j] == u'~'))
                ++j;
            em.add(i, j, Token::Operator);
            i = j;
            break;
        }
        default:
            word();
            break;
        }
    }

    if (out == 0) {
        if (hdPending)
            out = packState(B_HEREDOC, hdLen | (hdTabs ? 32u : 0u) | (hdHash << 6));
        else if (!commentEnd && n > 0 && s[n - 1] == u'\\') {
            int k = n - 1;
            int cnt = 0;
            while (k >= 0 && s[k] == u'\\') {
                ++cnt;
                --k;
            }
            if (cnt & 1)
                out = packState(B_CONT, 0);
        }
    }
    return out;
}

} // namespace

int lexBash(const Language &L, QStringView line, int state, Emit &em)
{
    BashLex lx(L, line, em);
    return lx.run(state);
}

} // namespace folia::code
