// json, yaml, toml and diff lexers.
//
// Carry states:
//   json : 1 = inside /* */ (jsonc comments), else 0
//   yaml : mode 1 = inside a block scalar, payload = parent indentation column
//   toml : mode 0/1/2 = normal / inside """ / inside ''' ; payload = bracket depth
//   diff : always 0 (every line is classified on its own)
#include "code/LangInternal.h"

namespace folia::code {

namespace {

// ===========================================================================
// JSON
// ===========================================================================
} // namespace

int lexJson(const Language &, QStringView line, int state, Emit &em)
{
    const char16_t *s = line.utf16();
    const int n = int(line.size());
    int i = 0;
    int out = 0;

    auto blockComment = [&](int from, int tokStart) {
        int j = from;
        while (j + 1 < n && !(s[j] == u'*' && s[j + 1] == u'/'))
            ++j;
        if (j + 1 < n) {
            em.add(tokStart, j + 2, Token::Comment);
            i = j + 2;
        } else {
            em.add(tokStart, n, Token::Comment);
            out = 1;
            i = n;
        }
    };
    if ((state & 15) == 1)
        blockComment(0, 0);

    while (i < n) {
        const char16_t c = s[i];
        if (isSpaceCh(c)) {
            ++i;
            continue;
        }
        switch (c) {
        case u'"': {
            int j = i + 1;
            while (j < n && s[j] != u'"')
                j += (s[j] == u'\\') ? 2 : 1;
            const int e = j < n ? j + 1 : n;
            int k = e;
            while (k < n && isSpaceCh(s[k]))
                ++k;
            em.add(i, e, (k < n && s[k] == u':') ? Token::Attribute : Token::String);
            i = e;
            break;
        }
        case u'/':
            if (i + 1 < n && s[i + 1] == u'/') {
                em.add(i, n, Token::Comment);
                i = n;
            } else if (i + 1 < n && s[i + 1] == u'*') {
                blockComment(i + 2, i);
            } else {
                ++i;
            }
            break;
        case u'{':
        case u'}':
        case u'[':
        case u']':
        case u',':
        case u':':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        default:
            if (isDigit(c) || ((c == u'-' || c == u'+') && i + 1 < n && (isDigit(s[i + 1]) || s[i + 1] == u'.')) ||
                (c == u'.' && i + 1 < n && isDigit(s[i + 1]))) {
                int j = i + 1;
                while (j < n && isDigit(s[j]))
                    ++j;
                if (j < n && s[j] == u'.') {
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
                em.add(i, j, Token::Number);
                i = j;
            } else if (isAsciiAlpha(c) || c == u'_' || c == u'$' || c >= 0x80) {
                int j = i + 1;
                while (j < n && (isAlnum(s[j]) || s[j] == u'_' || s[j] == u'$' || s[j] >= 0x80))
                    ++j;
                const int len = j - i;
                int k = j;
                while (k < n && isSpaceCh(s[k]))
                    ++k;
                if (equalsAscii(s + i, len, "true") || equalsAscii(s + i, len, "false") ||
                    equalsAscii(s + i, len, "null") || equalsAscii(s + i, len, "NaN") ||
                    equalsAscii(s + i, len, "Infinity"))
                    em.add(i, j, (k < n && s[k] == u':') ? Token::Attribute : Token::Constant);
                else if (k < n && s[k] == u':')
                    em.add(i, j, Token::Attribute);
                i = j;
            } else {
                ++i;
            }
            break;
        }
    }
    return out;
}

// ===========================================================================
// YAML
// ===========================================================================
namespace {

class YamlLex {
public:
    YamlLex(QStringView line, Emit &sink) : s(line.utf16()), n(int(line.size())), em(sink) {}
    int run(int stateIn);

private:
    const char16_t *s;
    const int n;
    Emit &em;
    int i = 0;
    int flow = 0;
    int parentCol = -1;
    bool blockHeader = false;

    bool endOrSpace(int j) const { return j >= n || isSpaceCh(s[j]); }
    Token classify(int a, int e) const;
    void quoted();
    void plain();
    bool blockScalarHeader();
};

Token YamlLex::classify(int a, int e) const
{
    const int len = e - a;
    if (len <= 0)
        return Token::Plain;
    const char16_t *p = s + a;
    if (equalsAsciiCI(p, len, "true") || equalsAsciiCI(p, len, "false") || equalsAsciiCI(p, len, "null") ||
        equalsAscii(p, len, "~") || equalsAsciiCI(p, len, "yes") || equalsAsciiCI(p, len, "no") ||
        equalsAsciiCI(p, len, "on") || equalsAsciiCI(p, len, "off"))
        return Token::Constant;
    int k = a;
    if (s[k] == u'+' || s[k] == u'-')
        ++k;
    if (k >= e)
        return Token::Plain;
    if (s[k] == u'.') {
        const int rest = e - k;
        if (equalsAsciiCI(s + k, rest, ".inf") || equalsAsciiCI(s + k, rest, ".nan"))
            return Token::Number;
        if (!(k + 1 < e && isDigit(s[k + 1])))
            return Token::Plain;
    } else if (!isDigit(s[k])) {
        return Token::Plain;
    }
    const bool hex = (e - k >= 3 && s[k] == u'0' && (s[k + 1] | 0x20) == u'x');
    for (int m = k; m < e; ++m) {
        const char16_t c = s[m];
        if (isDigit(c) || c == u'_' || c == u'.' || c == u'+' || c == u'-' || c == u':' || c == u'e' || c == u'E' ||
            c == u'x' || c == u'X' || c == u'o' || c == u'O')
            continue;
        if (hex && isHexDigit(c))
            continue;
        if ((c == u'T' || c == u'Z') && e - a >= 8)
            continue;
        return Token::Plain;
    }
    return Token::Number;
}

void YamlLex::quoted()
{
    const char16_t q = s[i];
    int j = i + 1;
    while (j < n && s[j] != q) {
        if (q == u'"' && s[j] == u'\\')
            ++j;
        ++j;
    }
    const int e = j < n ? j + 1 : n;
    int k = e;
    while (k < n && (s[k] == u' ' || s[k] == u'\t'))
        ++k;
    const bool key = k < n && s[k] == u':' &&
                     (endOrSpace(k + 1) || (flow > 0 && (s[k + 1] == u',' || s[k + 1] == u']' || s[k + 1] == u'}')));
    if (key) {
        em.add(i, e, Token::Attribute);
        parentCol = i;
        em.add(k, k + 1, Token::Punctuation);
        i = k + 1;
    } else {
        em.add(i, e, Token::String);
        i = e;
    }
}

void YamlLex::plain()
{
    const int a = i;
    int j = i;
    bool key = false;
    while (j < n) {
        const char16_t c = s[j];
        if (c == u':' && (endOrSpace(j + 1) ||
                          (flow > 0 && (s[j + 1] == u',' || s[j + 1] == u']' || s[j + 1] == u'}')))) {
            key = true;
            break;
        }
        if (c == u'#' && j > a && isSpaceCh(s[j - 1]))
            break;
        if (flow > 0 && (c == u',' || c == u']' || c == u'}' || c == u'[' || c == u'{'))
            break;
        ++j;
    }
    int e = j;
    while (e > a && isSpaceCh(s[e - 1]))
        --e;
    if (key) {
        em.add(a, e, Token::Attribute);
        parentCol = a;
        em.add(j, j + 1, Token::Punctuation);
        i = j + 1;
    } else {
        em.add(a, e, classify(a, e));
        i = j > a ? j : a + 1;
    }
}

// s[i] is '|' or '>'. Block scalar header if only chomping/indent indicators and
// then end of line or a comment follow.
bool YamlLex::blockScalarHeader()
{
    int j = i + 1;
    while (j < n && (s[j] == u'+' || s[j] == u'-' || isDigit(s[j])))
        ++j;
    int k = j;
    while (k < n && isSpaceCh(s[k]))
        ++k;
    if (k >= n || (s[k] == u'#' && k > j)) {
        em.add(i, j, Token::Operator);
        i = j;
        blockHeader = true;
        return true;
    }
    return false;
}

int YamlLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    if ((st & 15u) == 1u) {
        const uint32_t parentU = (st >> 4) > (1u << 20) ? (1u << 20) : (st >> 4);
        const int parent = int(parentU);
        int ind = 0;
        while (ind < n && (s[ind] == u' ' || s[ind] == u'\t'))
            ++ind;
        int e = n;
        while (e > ind && isSpaceCh(s[e - 1]))
            --e;
        if (e == ind)
            return packState(1, parentU); // blank line stays inside the scalar
        if (ind > parent) {
            em.add(ind, e, Token::String);
            return packState(1, parentU);
        }
    }

    // directive / document markers
    if (n > 0 && s[0] == u'%') {
        em.add(0, n, Token::Preprocessor);
        return 0;
    }
    if (n >= 3 && ((s[0] == u'-' && s[1] == u'-' && s[2] == u'-') || (s[0] == u'.' && s[1] == u'.' && s[2] == u'.')) &&
        endOrSpace(3)) {
        em.add(0, 3, Token::Punctuation);
        i = 3;
    }

    while (i < n) {
        const char16_t c = s[i];
        if (isSpaceCh(c)) {
            ++i;
            continue;
        }
        if (c == u'#' && (i == 0 || isSpaceCh(s[i - 1]))) {
            em.add(i, n, Token::Comment);
            i = n;
            break;
        }
        switch (c) {
        case u'-':
            if (endOrSpace(i + 1)) {
                em.add(i, i + 1, Token::Punctuation);
                parentCol = i;
                ++i;
                continue;
            }
            plain();
            break;
        case u'?':
            if (endOrSpace(i + 1)) {
                em.add(i, i + 1, Token::Punctuation);
                ++i;
                continue;
            }
            plain();
            break;
        case u':':
            if (endOrSpace(i + 1)) {
                em.add(i, i + 1, Token::Punctuation);
                ++i;
                continue;
            }
            plain();
            break;
        case u'&':
        case u'*': {
            int e = i + 1;
            while (e < n && !isSpaceCh(s[e]) && s[e] != u',' && s[e] != u']' && s[e] != u'}' && s[e] != u'[' &&
                   s[e] != u'{')
                ++e;
            em.add(i, e, Token::Variable);
            i = e;
            break;
        }
        case u'!': {
            int e = i + 1;
            while (e < n && !isSpaceCh(s[e]) && s[e] != u',' && s[e] != u']' && s[e] != u'}')
                ++e;
            em.add(i, e, Token::Type);
            i = e;
            break;
        }
        case u'|':
        case u'>':
            if (!blockScalarHeader())
                plain();
            break;
        case u'"':
        case u'\'':
            quoted();
            break;
        case u'[':
        case u'{':
            ++flow;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u']':
        case u'}':
            if (flow > 0)
                --flow;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u',':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        default:
            plain();
            break;
        }
    }

    if (blockHeader) {
        int col = parentCol;
        if (col < 0) {
            col = 0;
            while (col < n && (s[col] == u' ' || s[col] == u'\t'))
                ++col;
        }
        return packState(1, uint32_t(col));
    }
    return 0;
}

// ===========================================================================
// TOML
// ===========================================================================
class TomlLex {
public:
    TomlLex(QStringView line, Emit &sink) : s(line.utf16()), n(int(line.size())), em(sink) {}
    int run(int stateIn);

private:
    const char16_t *s;
    const int n;
    Emit &em;
    int i = 0;
    uint32_t depth = 0;

    int nextNonSpace(int j) const
    {
        while (j < n && (s[j] == u' ' || s[j] == u'\t'))
            ++j;
        return j;
    }
    // Scan ML string body from `from`. Returns true if it closed.
    bool mlBody(int from, int tokStart, bool basic)
    {
        const char16_t q = basic ? u'"' : u'\'';
        int j = from;
        while (j < n) {
            if (basic && s[j] == u'\\') {
                j += 2;
                continue;
            }
            if (s[j] == q && j + 2 < n && s[j + 1] == q && s[j + 2] == q) {
                j += 3;
                em.add(tokStart, j, Token::String);
                i = j;
                return true;
            }
            ++j;
        }
        em.add(tokStart, n, Token::String);
        i = n;
        return false;
    }
};

int TomlLex::run(int stateIn)
{
    const uint32_t st = uint32_t(stateIn);
    depth = (st >> 4);
    if (depth > (1u << 20))
        depth = 1u << 20;
    const uint32_t mode = st & 15u;
    if (mode == 1 || mode == 2) {
        if (!mlBody(0, 0, mode == 1))
            return packState(mode, depth);
    }
    bool first = (i == 0);

    while (i < n) {
        const char16_t c = s[i];
        if (isSpaceCh(c)) {
            ++i;
            continue;
        }
        const bool wasFirst = first;
        first = false;
        if (c == u'#') {
            em.add(i, n, Token::Comment);
            i = n;
            break;
        }
        if (c == u'[' && depth == 0 && wasFirst) {
            const bool arr = i + 1 < n && s[i + 1] == u'[';
            int j = i + (arr ? 2 : 1);
            while (j < n && s[j] != u']') {
                if (s[j] == u'"' || s[j] == u'\'') {
                    const char16_t q = s[j];
                    ++j;
                    while (j < n && s[j] != q)
                        ++j;
                }
                if (j < n)
                    ++j;
            }
            int e = j < n ? j + 1 : n;
            if (arr && e < n && s[e] == u']')
                ++e;
            em.add(i, e, Token::Tag);
            i = e;
            continue;
        }
        switch (c) {
        case u'"':
        case u'\'': {
            const bool basic = c == u'"';
            if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
                if (!mlBody(i + 3, i, basic))
                    return int(packState(basic ? 1u : 2u, depth));
                break;
            }
            int j = i + 1;
            while (j < n && s[j] != c) {
                if (basic && s[j] == u'\\')
                    ++j;
                ++j;
            }
            const int e = j < n ? j + 1 : n;
            const int k = nextNonSpace(e);
            em.add(i, e, (k < n && (s[k] == u'=' || s[k] == u'.')) ? Token::Attribute : Token::String);
            i = e;
            break;
        }
        case u'=':
            em.add(i, i + 1, Token::Operator);
            ++i;
            break;
        case u'[':
        case u'{':
            if (depth < (1u << 20))
                ++depth;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u']':
        case u'}':
            if (depth > 0)
                --depth;
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        case u',':
        case u'.':
            em.add(i, i + 1, Token::Punctuation);
            ++i;
            break;
        default: {
            const bool signNum = (c == u'+' || c == u'-') && i + 1 < n && isDigit(s[i + 1]);
            if (isDigit(c) || signNum) {
                int j = i + 1;
                while (j < n && (isAlnum(s[j]) || s[j] == u'_' || s[j] == u':' || s[j] == u'.' || s[j] == u'+' ||
                                 s[j] == u'-'))
                    ++j;
                const int k = nextNonSpace(j);
                const bool key = k < n && s[k] == u'=';
                em.add(i, j, key ? Token::Attribute : Token::Number);
                i = j;
            } else if (isAsciiAlpha(c) || c == u'_' || c == u'-' || c >= 0x80) {
                int j = i + 1;
                while (j < n && (isAlnum(s[j]) || s[j] == u'_' || s[j] == u'-' || s[j] >= 0x80))
                    ++j;
                const int a0 = i + ((c == u'-' && j - i > 1) ? 1 : 0);
                const int len = j - a0;
                const int k = nextNonSpace(j);
                if (k < n && (s[k] == u'=' || s[k] == u'.'))
                    em.add(i, j, Token::Attribute);
                else if (equalsAscii(s + a0, len, "true") || equalsAscii(s + a0, len, "false") ||
                         equalsAscii(s + a0, len, "inf") || equalsAscii(s + a0, len, "nan"))
                    em.add(i, j, Token::Constant);
                i = j;
            } else {
                ++i;
            }
            break;
        }
        }
    }
    return depth ? packState(0, depth) : 0;
}

// ===========================================================================
// diff
// ===========================================================================
inline bool startsWith(const char16_t *s, int n, const char *lit)
{
    for (int k = 0; lit[k]; ++k)
        if (k >= n || s[k] != char16_t(uint8_t(lit[k])))
            return false;
    return true;
}

} // namespace

int lexToml(const Language &, QStringView line, int state, Emit &em)
{
    TomlLex lx(line, em);
    return lx.run(state);
}

int lexYaml(const Language &, QStringView line, int state, Emit &em)
{
    YamlLex lx(line, em);
    return lx.run(state);
}

int lexDiff(const Language &, QStringView line, int, Emit &em)
{
    const char16_t *s = line.utf16();
    const int n = int(line.size());
    if (n == 0)
        return 0;
    switch (s[0]) {
    case u'+':
        em.add(0, n, startsWith(s, n, "+++") && (n == 3 || s[3] == u' ' || s[3] == u'\t') ? Token::Keyword : Token::String);
        break;
    case u'-':
        em.add(0, n, startsWith(s, n, "---") && (n == 3 || s[3] == u' ' || s[3] == u'\t') ? Token::Keyword : Token::Tag);
        break;
    case u'@':
        if (startsWith(s, n, "@@")) {
            int e = 2;
            while (e + 1 < n && !(s[e] == u'@' && s[e + 1] == u'@'))
                ++e;
            e = (e + 1 < n) ? e + 2 : n;
            em.add(0, e, Token::Preprocessor);
            em.add(e, n, Token::Function);
        }
        break;
    case u'<':
        if (n > 1 && s[1] == u' ')
            em.add(0, n, Token::Tag);
        break;
    case u'>':
        if (n > 1 && s[1] == u' ')
            em.add(0, n, Token::String);
        break;
    case u'\\':
        em.add(0, n, Token::Comment);
        break;
    case u'*':
        if (startsWith(s, n, "***"))
            em.add(0, n, Token::Keyword);
        break;
    case u'=':
        if (startsWith(s, n, "====="))
            em.add(0, n, Token::Comment);
        break;
    default:
        if (startsWith(s, n, "diff ") || startsWith(s, n, "index ") || startsWith(s, n, "new file") ||
            startsWith(s, n, "deleted file") || startsWith(s, n, "old mode") || startsWith(s, n, "new mode") ||
            startsWith(s, n, "similarity ") || startsWith(s, n, "dissimilarity ") || startsWith(s, n, "rename ") ||
            startsWith(s, n, "copy ") || startsWith(s, n, "Binary files") || startsWith(s, n, "GIT binary patch") ||
            startsWith(s, n, "Index: ") || startsWith(s, n, "From ") || startsWith(s, n, "Subject: ") ||
            startsWith(s, n, "Date: "))
            em.add(0, n, Token::Comment);
        break;
    }
    return 0;
}

} // namespace folia::code
