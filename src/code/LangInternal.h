// Internal definitions shared by the folia_code lexers. Not part of the public
// contract (Highlighter.h is). Everything here is immutable after a Language
// has been constructed.
#pragma once

#include "code/Highlighter.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace folia::code {

// Alphabetical == supportedLanguages() order.
enum class Lang : int {
    Bash, C, Cpp, CSharp, Css, Diff, Go, Html, Java, JavaScript, Json, Lua, Python, Rust, Sql, Swift, Toml,
    TypeScript, Yaml,
    Count_
};

// ---------------------------------------------------------------------------
// character helpers (UTF-16 code units). Anything >= 0x80 is treated as an
// identifier character so surrogate pairs are never split by a token boundary.
// ---------------------------------------------------------------------------
inline bool isAsciiAlpha(char16_t c) { return unsigned((c | 0x20) - 'a') < 26u; }
inline bool isDigit(char16_t c) { return unsigned(c - u'0') < 10u; }
inline bool isAlnum(char16_t c) { return isAsciiAlpha(c) || isDigit(c); }
inline bool isHexDigit(char16_t c) { return isDigit(c) || unsigned((c | 0x20) - 'a') < 6u; }
inline bool isSpaceCh(char16_t c) { return c == u' ' || unsigned(c - 9u) < 5u; }
inline bool isHighSurrogate(char16_t c) { return (c & 0xFC00) == 0xD800; }
inline bool isLowSurrogate(char16_t c) { return (c & 0xFC00) == 0xDC00; }
inline bool isUpper(char16_t c) { return unsigned(c - u'A') < 26u; }
inline bool isLower(char16_t c) { return unsigned(c - u'a') < 26u; }

// ---------------------------------------------------------------------------
// Emission with run merging. Only spans appended by *this* highlightLine call
// (index >= floor) are merged, so callers may share one output vector.
// ---------------------------------------------------------------------------
struct Emit {
    QVector<Span> &out;
    int base;   // offset added to every span (embedded sub-lexers)
    int floor;  // first index in `out` that belongs to the current call

    void add(int s, int e, Token t)
    {
        if (e <= s || t == Token::Plain)
            return;
        const int bs = base + s;
        if (out.size() > floor) {
            Span &b = out.last();
            if (b.token == t && b.start + b.length == bs) {
                b.length += e - s;
                return;
            }
        }
        out.append(Span{bs, e - s, t});
    }
};

// ---------------------------------------------------------------------------
// Keyword table: open addressing, built once per language, ASCII only.
// info byte: low 4 bits = Token, bit4 = next identifier is a type name,
// bit5 = next identifier is a function name, bit6 = "soft" (only when the next
// non-blank character starts an identifier or quote), bit7 = "soft line" (only
// as the first token of a line that ends with ':').
// ---------------------------------------------------------------------------
constexpr uint8_t kInfoToken = 0x0F;
constexpr uint8_t kNextType = 0x10;
constexpr uint8_t kNextFunc = 0x20;
constexpr uint8_t kSoftNext = 0x40;
constexpr uint8_t kSoftLine = 0x80;

class KwTable {
public:
    explicit KwTable(bool caseInsensitive = false) : ci_(caseInsensitive) {}

    // Space separated words. Suffixes: ":T" next ident is a type, ":F" next ident
    // is a function, "~" soft (next token ident/quote), "?" soft line (python
    // match/case). First definition of a word wins.
    void add(const char *words, Token tok);
    void finish();

    uint8_t find(const char16_t *p, int len) const
    {
        if (len <= 0 || len > 32 || slots_.empty())
            return 0;
        char buf[32];
        uint32_t h = 2166136261u;
        for (int k = 0; k < len; ++k) {
            const char16_t c = p[k];
            if (c > 127)
                return 0;
            char ch = char(c);
            if (ci_ && ch >= 'A' && ch <= 'Z')
                ch = char(ch + 32);
            buf[k] = ch;
            h = (h ^ uint8_t(ch)) * 16777619u;
        }
        const uint32_t mask = uint32_t(slots_.size()) - 1;
        uint32_t idx = h & mask;
        for (;;) {
            const Slot &sl = slots_[idx];
            if (sl.len == 0)
                return 0;
            if (sl.hash == h && sl.len == len && std::memcmp(pool_.data() + sl.off, buf, size_t(len)) == 0)
                return sl.info;
            idx = (idx + 1) & mask;
        }
    }

private:
    struct Slot {
        uint32_t hash = 0;
        uint16_t off = 0;
        uint8_t len = 0;
        uint8_t info = 0;
    };
    struct Pending {
        std::string word;
        uint8_t info;
    };
    bool ci_;
    std::vector<Pending> pending_;
    std::vector<Slot> slots_;
    std::string pool_;
};

// ---------------------------------------------------------------------------
// Lexical features for the generic C-like lexer.
// ---------------------------------------------------------------------------
struct Features {
    bool blockC = false;       // /* */
    bool nest = false;         // nested block comments (rust, swift)
    bool lineSlash = false;    // //
    bool lineHash = false;     // #  (python)
    bool lineDash = false;     // -- (sql, lua)
    bool luaLong = false;      // [==[ ]==] and --[==[ ]==]
    bool preproc = false;      // #directive at line start (c, c++, c#)
    bool swiftHash = false;    // #if, #"raw"#
    bool dq = true;            // "..."
    bool sq = false;           // '...' (string or char literal)
    bool dqMulti = false;      // "..." may span lines (rust)
    bool sqMulti = false;      // '...' may span lines (sql)
    bool strCont = false;      // backslash-newline continues a string
    bool lineCommentCont = false;
    bool backtickRaw = false;  // `...` raw multi-line (go)
    bool templ = false;        // js template literals with ${}
    bool triple = false;       // """ strings
    bool tripleSq = false;     // ''' strings too
    bool regex = false;        // js regex literals
    bool atAttr = false;       // @Name -> Attribute
    bool pyDecor = false;      // @decorator at line start
    bool rustRaw = false;
    bool rustAttr = false;
    bool rustLifetime = false;
    bool cppAttr = false;      // [[attribute]] (c23, c++11)
    bool rustMacro = false;
    bool cppRaw = false;
    bool csStr = false;        // @"..." and $"..."
    bool sqlStr = false;       // '' doubling, no backslash escapes, sql variables
    bool dollarQuote = false;  // $$...$$ strings (postgres)
    bool dollarIdent = false;  // $ is an identifier char (js)
    bool dollarVar = false;    // $0 / $name -> Variable (swift)
    bool hashIdent = false;    // #private (js)
    bool numApos = false;      // 1'000 digit separators
    bool capType = false;      // CamelCase -> Type
    bool capConst = false;     // ALL_CAPS  -> Constant
    Token btTok = Token::Plain; // token for `...` identifiers (sql: Variable; swift: none)
    bool btIdent = false;      // `...` single-line identifier quoting
    const char *prefixes = nullptr; // letters accepted as a string prefix
    int maxPrefix = 0;
};

class Language;
using LexFn = int (*)(const Language &, QStringView, int, Emit &);

class Language {
public:
    Lang id = Lang::Count_;
    const char *name = "";
    Features f;
    KwTable kw;
    LexFn lex = nullptr;
};

// Registry access (Highlighter.cpp) and builder (Languages.cpp).
const Language *languageById(Lang id);
Language *buildLanguage(Lang id);

// Lexers.
int lexCLike(const Language &L, QStringView line, int state, Emit &em);
int lexBash(const Language &L, QStringView line, int state, Emit &em);
int lexCss(const Language &L, QStringView line, int state, Emit &em);
int lexHtml(const Language &L, QStringView line, int state, Emit &em);
int lexJson(const Language &L, QStringView line, int state, Emit &em);
int lexYaml(const Language &L, QStringView line, int state, Emit &em);
int lexToml(const Language &L, QStringView line, int state, Emit &em);
int lexDiff(const Language &L, QStringView line, int state, Emit &em);

inline int packState(uint32_t mode, uint32_t payload) { return int(mode | (payload << 4)); }

// FNV-1a over UTF-16 units folded to 22 bits (delimiter fingerprints for C++
// raw strings, SQL dollar quotes and shell heredocs).
inline uint32_t hashUnits(const char16_t *p, int len)
{
    uint32_t h = 2166136261u;
    for (int k = 0; k < len; ++k)
        h = (h ^ uint32_t(p[k])) * 16777619u;
    return (h ^ (h >> 22)) & 0x3FFFFFu;
}

inline bool equalsAscii(const char16_t *p, int len, const char *lit)
{
    for (int k = 0; k < len; ++k)
        if (lit[k] == 0 || p[k] != char16_t(uint8_t(lit[k])))
            return false;
    return lit[len] == 0;
}

// Case-insensitive ASCII compare against a lowercase literal.
inline bool equalsAsciiCI(const char16_t *p, int len, const char *lit)
{
    for (int k = 0; k < len; ++k) {
        if (lit[k] == 0)
            return false;
        char16_t c = p[k];
        if (c >= u'A' && c <= u'Z')
            c = char16_t(c + 32);
        if (c != char16_t(uint8_t(lit[k])))
            return false;
    }
    return lit[len] == 0;
}

} // namespace folia::code
