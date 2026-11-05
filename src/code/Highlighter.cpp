// Registry, lazy language construction and the public entry points.
// The lexers live in the *Lexer.cpp files; the per-language tables in
// Languages.cpp. See docs/CODE_LANGUAGES.md.
#include "code/Highlighter.h"
#include "code/LangInternal.h"

#include <atomic>
#include <mutex>

namespace folia::code {

// ---------------------------------------------------------------------------
// KwTable
// ---------------------------------------------------------------------------
void KwTable::add(const char *words, Token tok)
{
    const char *p = words;
    while (*p) {
        while (*p == ' ')
            ++p;
        const char *b = p;
        while (*p && *p != ' ')
            ++p;
        if (p == b)
            break;
        std::string w(b, size_t(p - b));
        uint8_t info = uint8_t(tok);
        for (;;) {
            if (w.size() > 2 && w[w.size() - 2] == ':' && w.back() == 'T') {
                info |= kNextType;
                w.resize(w.size() - 2);
            } else if (w.size() > 2 && w[w.size() - 2] == ':' && w.back() == 'F') {
                info |= kNextFunc;
                w.resize(w.size() - 2);
            } else if (w.size() > 1 && w.back() == '~') {
                info |= kSoftNext;
                w.pop_back();
            } else if (w.size() > 1 && w.back() == '?') {
                info |= kSoftLine;
                w.pop_back();
            } else {
                break;
            }
        }
        if (ci_)
            for (char &c : w)
                if (c >= 'A' && c <= 'Z')
                    c = char(c + 32);
        pending_.push_back({std::move(w), info});
    }
}

void KwTable::finish()
{
    size_t cap = 16;
    while (cap < pending_.size() * 2 + 2)
        cap <<= 1;
    slots_.assign(cap, Slot{});
    pool_.clear();
    for (const Pending &pe : pending_) {
        if (pe.word.empty() || pe.word.size() > 32)
            continue;
        uint32_t h = 2166136261u;
        for (char ch : pe.word)
            h = (h ^ uint8_t(ch)) * 16777619u;
        const uint32_t mask = uint32_t(cap) - 1;
        uint32_t idx = h & mask;
        bool dup = false;
        while (slots_[idx].len != 0) {
            const Slot &sl = slots_[idx];
            if (sl.hash == h && sl.len == pe.word.size() && pool_.compare(sl.off, sl.len, pe.word) == 0) {
                dup = true; // first definition wins
                break;
            }
            idx = (idx + 1) & mask;
        }
        if (dup)
            continue;
        Slot &sl = slots_[idx];
        sl.hash = h;
        sl.off = uint16_t(pool_.size());
        sl.len = uint8_t(pe.word.size());
        sl.info = pe.info;
        pool_.append(pe.word);
    }
    pending_.clear();
    pending_.shrink_to_fit();
}

// ---------------------------------------------------------------------------
// Lazy registry
// ---------------------------------------------------------------------------
namespace {

constexpr int kCount = int(Lang::Count_);

struct Slot {
    std::once_flag once;
    std::atomic<const Language *> lang{nullptr};
};

Slot g_slots[kCount];
std::atomic<int> g_loaded{0};

const char *const kCanonical[kCount] = {"bash", "c",      "cpp",   "csharp", "css",        "diff",   "go",
                                         "html", "java",   "javascript", "json", "lua",       "python", "rust",
                                         "sql",  "swift",  "toml",  "typescript", "yaml"};

struct Alias {
    const char *name;
    Lang id;
};

// Lowercase aliases. Canonical names are included.
const Alias kAliases[] = {
    {"bash", Lang::Bash},        {"sh", Lang::Bash},         {"zsh", Lang::Bash},        {"shell", Lang::Bash},
    {"console", Lang::Bash},     {"shellscript", Lang::Bash}, {"ksh", Lang::Bash},       {"shellsession", Lang::Bash},
    {"c", Lang::C},              {"h", Lang::C},
    {"cpp", Lang::Cpp},          {"c++", Lang::Cpp},         {"cc", Lang::Cpp},          {"cxx", Lang::Cpp},
    {"hpp", Lang::Cpp},          {"hh", Lang::Cpp},          {"hxx", Lang::Cpp},         {"h++", Lang::Cpp},
    {"cplusplus", Lang::Cpp},
    {"csharp", Lang::CSharp},    {"cs", Lang::CSharp},       {"c#", Lang::CSharp},       {"dotnet", Lang::CSharp},
    {"css", Lang::Css},
    {"diff", Lang::Diff},        {"patch", Lang::Diff},      {"udiff", Lang::Diff},
    {"go", Lang::Go},            {"golang", Lang::Go},
    {"html", Lang::Html},        {"htm", Lang::Html},        {"xml", Lang::Html},        {"svg", Lang::Html},
    {"xhtml", Lang::Html},       {"xsl", Lang::Html},        {"xslt", Lang::Html},       {"plist", Lang::Html},
    {"rss", Lang::Html},         {"atom", Lang::Html},
    {"java", Lang::Java},
    {"javascript", Lang::JavaScript}, {"js", Lang::JavaScript}, {"jsx", Lang::JavaScript}, {"mjs", Lang::JavaScript},
    {"cjs", Lang::JavaScript},   {"node", Lang::JavaScript}, {"nodejs", Lang::JavaScript},
    {"json", Lang::Json},        {"jsonc", Lang::Json},      {"json5", Lang::Json},
    {"lua", Lang::Lua},
    {"python", Lang::Python},    {"py", Lang::Python},       {"py3", Lang::Python},      {"python3", Lang::Python},
    {"pyw", Lang::Python},
    {"rust", Lang::Rust},        {"rs", Lang::Rust},
    {"sql", Lang::Sql},          {"mysql", Lang::Sql},       {"postgres", Lang::Sql},    {"postgresql", Lang::Sql},
    {"pgsql", Lang::Sql},        {"plsql", Lang::Sql},       {"tsql", Lang::Sql},        {"sqlite", Lang::Sql},
    {"swift", Lang::Swift},
    {"toml", Lang::Toml},
    {"typescript", Lang::TypeScript}, {"ts", Lang::TypeScript}, {"tsx", Lang::TypeScript}, {"mts", Lang::TypeScript},
    {"cts", Lang::TypeScript},
    {"yaml", Lang::Yaml},        {"yml", Lang::Yaml},
};

// Resolves a (possibly aliased, any-case) name to a language id without
// allocating or constructing anything. Returns Count_ if unknown.
Lang resolve(QStringView name)
{
    while (!name.isEmpty() && name.front().unicode() <= 32)
        name = name.sliced(1);
    while (!name.isEmpty() && name.back().unicode() <= 32)
        name.chop(1);
    if (name.isEmpty() || name.size() > 16)
        return Lang::Count_;
    char buf[17];
    int len = 0;
    for (QChar qc : name) {
        const char16_t c = qc.unicode();
        if (c >= 127)
            return Lang::Count_;
        buf[len++] = (c >= u'A' && c <= u'Z') ? char(c + 32) : char(c);
    }
    buf[len] = 0;
    for (const Alias &a : kAliases)
        if (std::strcmp(a.name, buf) == 0)
            return a.id;
    return Lang::Count_;
}

} // namespace

const Language *languageById(Lang id)
{
    const int idx = int(id);
    if (idx < 0 || idx >= kCount)
        return nullptr;
    Slot &sl = g_slots[idx];
    if (const Language *p = sl.lang.load(std::memory_order_acquire))
        return p;
    std::call_once(sl.once, [&] {
        Language *l = buildLanguage(id);
        l->id = id;
        l->name = kCanonical[idx];
        g_loaded.fetch_add(1, std::memory_order_relaxed);
        sl.lang.store(l, std::memory_order_release);
    });
    return sl.lang.load(std::memory_order_acquire);
}

const Language *languageFor(QStringView name)
{
    const Lang id = resolve(name);
    return id == Lang::Count_ ? nullptr : languageById(id);
}

QStringList supportedLanguages()
{
    QStringList list;
    list.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
        list.append(QString::fromLatin1(kCanonical[i]));
    list.sort();
    return list;
}

QString canonicalName(const Language *lang)
{
    return lang ? QString::fromLatin1(lang->name) : QString();
}

int loadedLanguageCount()
{
    return g_loaded.load(std::memory_order_relaxed);
}

int highlightLine(const Language *lang, QStringView line, int stateIn, QVector<Span> &out)
{
    if (!lang)
        return 0;
    Emit em{out, 0, int(out.size())};
    return lang->lex(*lang, line, stateIn, em);
}

} // namespace folia::code
