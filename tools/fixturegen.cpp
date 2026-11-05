// Deterministic workspace fixture generator (performance-and-QA owner).
//
//   folia_fixturegen --out <db> --seed <n> [--pages 50000] [--manifest <json>]
//                    [--force] [--verify]
//
// * The schema is created by opening and closing a folia::Store once (so the
//   fixture always carries exactly the migrations the app runs), then all
//   rows are written with raw SQLite in a few large transactions.
// * Every byte of content comes from a seeded splitmix64 stream. Each page
//   draws from its own stream derived from (seed, page id), so the same seed
//   always yields the same rows (and, in practice, a byte-identical file).
// * The manifest (stdout, or --manifest) carries the page ids the in-app
//   bench driver needs and a content hash: FNV-1a 64 over every inserted
//   page row and block row in insertion order (ids, parent/page, ord, kind,
//   lang, title/text, target; integers as 8 little-endian bytes, strings
//   length-prefixed UTF-8).
// * --verify re-opens the result through folia::Store, loads the special
//   pages into a real PageSession (offscreen QGuiApplication) and checks that
//   a full save snapshot reproduces the stored blocks exactly (kind, lang,
//   text, target, order), i.e. the fixture maps 1:1 onto editor lines.
//
// Text rules (see editor/PageSession.cpp): Text blocks are single lines (no
// '\n', U+2028/2029, U+FFFC); they never start with ``` and are never "$$" or
// "$$...$$". Code inner lines are never exactly ```; multi-line math inner
// lines are never exactly $$ and use lang ""; one-line math uses lang "single".

#include "editor/EditorContext.h"
#include "editor/PageSession.h"
#include "storage/Sql.h"
#include "storage/Store.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using u64 = std::uint64_t;
using i64 = std::int64_t;

constexpr int kGeneratorVersion = 1;
constexpr i64 kGap = 65536;               // sparse sibling/block order (Store kGap)
constexpr i64 kBaseMs = 1735689600000LL;  // 2025-01-01T00:00:00Z, fixed timestamps

// ------------------------------------------------------------------ PRNG --
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed) {}
    u64 next()
    {
        u64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n).
    std::uint32_t below(std::uint32_t n) { return n ? std::uint32_t(((next() >> 32) * u64(n)) >> 32) : 0; }
    // Uniform in [lo, hi] (inclusive).
    int range(int lo, int hi) { return lo + int(below(std::uint32_t(hi - lo + 1))); }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    bool chance(double p) { return unit() < p; }
    template <std::size_t N>
    const char *pick(const char *const (&a)[N]) { return a[below(std::uint32_t(N))]; }
};

u64 streamSeed(u64 seed, u64 salt)
{
    Rng r(seed ^ (salt * 0xD1B54A32D192ED03ull) ^ 0xA0761D6478BD642Full);
    r.next();
    return r.next();
}

// ------------------------------------------------------------- FNV-1a 64 --
struct Fnv {
    u64 h = 0xcbf29ce484222325ull;
    void bytes(const void *p, std::size_t n)
    {
        const auto *b = static_cast<const unsigned char *>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 0x100000001b3ull;
        }
    }
    void num(i64 v)
    {
        unsigned char b[8];
        for (int i = 0; i < 8; ++i)
            b[i] = static_cast<unsigned char>(u64(v) >> (8 * i));
        bytes(b, 8);
    }
    void str(const std::string &s)
    {
        num(i64(s.size()));
        bytes(s.data(), s.size());
    }
};

// ------------------------------------------------------------ word pools --
const char *const kLatin[] = {
    "the", "note", "system", "layout", "garden", "river", "method", "quiet", "signal", "vector", "paper",
    "window", "search", "memory", "thread", "cache", "render", "fixture", "bench", "stone", "light",
    "archive", "index", "border", "marker", "table", "folder", "draft", "journal", "review", "budget",
    "coffee", "harbor", "lantern", "meadow", "orbit", "pixel", "quartz", "ribbon", "saddle", "tundra",
    "velvet", "willow", "zephyr", "and", "of", "with", "for", "into", "over", "under", "between",
    "a", "an", "is", "was", "will", "should", "could", "every", "some", "many", "few", "first", "last",
    "schema", "query", "commit", "branch", "outline", "chapter", "section", "summary", "question",
    "answer", "deadline", "meeting", "project", "release", "latency", "frame", "budgeted", "measured",
    "careful", "rapid", "simple", "robust", "visible", "hidden", "nested", "linked", "shared", "local",
    "keyboard", "editor", "formula", "theorem", "proof", "lemma", "sketch", "plan", "idea", "travel",
};
// Latin with combining marks (NFD, e.g. e + U+0301) mixed with precomposed forms.
const char *const kLatinMarked[] = {
    u8"cafe\u0301", u8"nai\u0308ve", u8"re\u0301sume\u0301", u8"fac\u0327ade", u8"an\u0303o",
    u8"Zoe\u0308", u8"cre\u0300me", u8"bru\u0302le\u0301e", u8"pin\u0303ata", u8"jalapen\u0303o",
    u8"Mu\u0308ller", u8"Dvor\u030Ca\u0301k", u8"Tie\u0302\u0301ng", u8"Vie\u0323\u0302t",
    u8"smo\u0308rga\u030Asbord", u8"A\u030Angstro\u0308m", u8"S\u00E3o", u8"caf\u00E9", u8"na\u00EFve",
    u8"\u00C5ngstr\u00F6m", u8"\u0110\u00E0 N\u1EB5ng", u8"e\u0301le\u0300ve", u8"Bjo\u0308rk",
};
const char *const kCjk[] = {
    u8"笔记", u8"数据库", u8"东京", u8"日本語", u8"中文", u8"汉字", u8"研究", u8"会议", u8"计划",
    u8"設計", u8"ひらがな", u8"カタカナ", u8"ノート", u8"検索", u8"整理", u8"記録", u8"性能", u8"測定",
    u8"編集", u8"見出し", u8"数式", u8"プログラム", u8"图书馆", u8"天气", u8"春夏秋冬",
};
const char *const kHangul[] = {
    u8"한국어", u8"노트", u8"데이터", u8"검색", u8"서울", u8"회의", u8"계획", u8"편집기", u8"수식",
    u8"\u1100\u1161\u11A8\u1109\u1165\u11BC" /* conjoining jamo (NFD) */, u8"안녕하세요", u8"성능",
};
const char *const kArabic[] = {
    u8"مرحبا", u8"العربية", u8"كتاب", u8"مَرْحَبًا", u8"بيانات", u8"ملاحظة", u8"بحث", u8"مدينة",
    u8"اجتماع", u8"قائمة", u8"صفحة", u8"سريع",
};
const char *const kHebrew[] = {
    u8"שלום", u8"עברית", u8"שָׁלוֹם", u8"מחברת", u8"חיפוש", u8"פגישה", u8"רשימה", u8"עמוד", u8"מהיר",
};
const char *const kDevanagari[] = {
    u8"नमस्ते", u8"हिन्दी", u8"क्षत्रिय", u8"विज्ञान", u8"टिप्पणी", u8"खोज", u8"पुस्तकालय", u8"सूत्र",
    u8"श्रीमान", u8"द्वार",
};
const char *const kCyrillic[] = {
    u8"Привет", u8"русский", u8"ёлка", u8"и\u0306од" /* й as и + U+0306 */, u8"заметка", u8"поиск",
    u8"данные", u8"страница", u8"формула", u8"быстро", u8"Україна", u8"щёлк",
};
const char *const kThai[] = {
    u8"สวัสดี", u8"ภาษาไทย", u8"บันทึก", u8"ค้นหา", u8"ข้อมูล", u8"หน้า", u8"เร็ว", u8"สูตร", u8"ประชุม",
};
const char *const kGreek[] = {u8"αλφα", u8"λόγος", u8"Ωμέγα", u8"θεώρημα", u8"ἀρχή"};
// Emoji incl. skin-tone modifiers, ZWJ sequences, VS16, flags, keycaps.
const char *const kEmoji[] = {
    u8"\U0001F600", u8"\U0001F44D\U0001F3FD", u8"\U0001F469\u200D\U0001F4BB",
    u8"\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466", u8"\U0001F3F3\uFE0F\u200D\U0001F308",
    u8"\U0001F1EF\U0001F1F5", u8"\u2764\uFE0F", u8"\U0001F9D1\U0001F3FF\u200D\U0001F680",
    u8"\U0001F926\U0001F3FB\u200D\u2640\uFE0F", u8"\u2705", u8"\U0001F4DD", u8"\U0001F525",
    u8"\U0001F44B\U0001F3FF", u8"\U0001F9D1\U0001F3FD\u200D\U0001F91D\u200D\U0001F9D1\U0001F3FB",
    u8"1\uFE0F\u20E3", u8"\U0001F1E7\U0001F1F7",
};

const char *const kInlineCode[] = {
    "`std::vector<int>`", "`ctx.run(\"page.open\")`", "`SELECT id FROM pages`", "`git status`",
    "`O(1)`", "`--bench open`", "`fn main()`", "`let x = 42;`", "`0xFF`", "`nullptr`",
    "`QTextLayout::draw`", "`PRAGMA user_version`", "`a[i] += b[j]`", "`async/await`", "`#include <map>`",
};
const char *const kInlineMath[] = {
    "$x^2 + y^2 = r^2$", "$e^{i\\pi} + 1 = 0$", "$\\frac{a}{b}$", "$\\sum_{k=1}^{n} k = \\frac{n(n+1)}{2}$",
    "$\\alpha_i \\le \\beta_j$", "$O(n \\log n)$", "$\\sqrt{2}$", "$\\int_0^1 f(t)\\,dt$", "$\\mathbb{R}^n$",
    "$p < 0.05$", "$\\lim_{n\\to\\infty} a_n$", "$\\nabla f$", "$\\binom{n}{k}$", "$a \\equiv b \\pmod{m}$",
};

enum class Script { Latin, Cjk, Hangul, Arabic, Hebrew, Devanagari, Cyrillic, Thai, Greek };

const char *wordOf(Rng &r, Script s)
{
    switch (s) {
    case Script::Latin: return r.chance(0.07) ? r.pick(kLatinMarked) : r.pick(kLatin);
    case Script::Cjk: return r.pick(kCjk);
    case Script::Hangul: return r.pick(kHangul);
    case Script::Arabic: return r.pick(kArabic);
    case Script::Hebrew: return r.pick(kHebrew);
    case Script::Devanagari: return r.pick(kDevanagari);
    case Script::Cyrillic: return r.pick(kCyrillic);
    case Script::Thai: return r.pick(kThai);
    case Script::Greek: return r.pick(kGreek);
    }
    return "x";
}

Script foreignScript(Rng &r)
{
    static const Script all[] = {Script::Cjk, Script::Hangul, Script::Arabic, Script::Hebrew, Script::Devanagari,
                                 Script::Cyrillic, Script::Thai, Script::Greek, Script::Cjk, Script::Cyrillic};
    return all[r.below(10)];
}

void capitalize(std::string &s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = char(s[0] - 'a' + 'A');
}

// One sentence. Mixed sentences are Latin with sprinkled other scripts;
// monolingual ones follow that script's spacing/punctuation conventions.
std::string sentence(Rng &r, int minW, int maxW, bool markup)
{
    std::string out;
    const int n = r.range(minW, maxW);
    if (r.chance(0.14)) {
        const Script s = foreignScript(r);
        const bool spaced = s != Script::Cjk && s != Script::Thai;
        for (int i = 0; i < n; ++i) {
            if (i && spaced)
                out += ' ';
            else if (i && s == Script::Thai && r.chance(0.3))
                out += ' ';
            out += wordOf(r, s);
        }
        switch (s) {
        case Script::Cjk: out += u8"。"; break;
        case Script::Arabic: out += r.chance(0.3) ? u8"؟" : "."; break;
        case Script::Devanagari: out += u8"।"; break;
        case Script::Thai: break;
        default: out += '.'; break;
        }
        return out;
    }
    for (int i = 0; i < n; ++i) {
        std::string w;
        if (r.chance(0.12))
            w = wordOf(r, foreignScript(r));
        else
            w = wordOf(r, Script::Latin);
        if (i == 0)
            capitalize(w);
        if (i)
            out += ' ';
        if (markup && i > 0 && i < n - 1) {
            const double x = r.unit();
            if (x < 0.035) {
                out += r.pick(kInlineCode);
                out += ' ';
            } else if (x < 0.06) {
                out += r.pick(kInlineMath);
                out += ' ';
            } else if (x < 0.075) {
                w = "**" + w + "**";
            } else if (x < 0.09) {
                w = "*" + w + "*";
            } else if (x < 0.1) {
                out += r.pick(kEmoji);
                out += ' ';
            }
        }
        out += w;
    }
    const double e = r.unit();
    out += e < 0.8 ? "." : e < 0.9 ? "?" : "!";
    if (r.chance(0.04)) {
        out += ' ';
        out += r.pick(kEmoji);
    }
    return out;
}

std::string words(Rng &r, int minW, int maxW)
{
    std::string out;
    const int n = r.range(minW, maxW);
    for (int i = 0; i < n; ++i) {
        std::string w = r.chance(0.15) ? std::string(wordOf(r, foreignScript(r))) : std::string(wordOf(r, Script::Latin));
        if (i == 0)
            capitalize(w);
        if (i)
            out += ' ';
        out += w;
    }
    return out;
}

std::string pageTitle(Rng &r)
{
    std::string t;
    if (r.chance(0.08)) {
        t += r.pick(kEmoji);
        t += ' ';
    }
    if (r.chance(0.12)) {
        // Fully non-Latin title.
        const Script s = foreignScript(r);
        const int n = r.range(1, 3);
        for (int i = 0; i < n; ++i) {
            if (i && s != Script::Cjk)
                t += ' ';
            t += wordOf(r, s);
        }
    } else {
        t += words(r, 1, 4);
    }
    if (r.chance(0.1)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, r.chance(0.5) ? " %d" : " #%d", r.range(1, 9999));
        t += buf;
    }
    return t;
}

// Replace @X placeholders in code/math templates.
std::string fill(std::string s, const std::map<std::string, std::string> &vars)
{
    for (const auto &kv : vars) {
        std::size_t p = 0;
        while ((p = s.find(kv.first, p)) != std::string::npos) {
            s.replace(p, kv.first.size(), kv.second);
            p += kv.second.size();
        }
    }
    return s;
}

std::string num(i64 v) { return std::to_string(v); }

// --------------------------------------------------------- block records --
struct Gen {
    int kind = 0; // folia::BlockKind
    std::string lang;
    std::string text;
    i64 target = 0;
    bool para = false;
    double weight = 1.0;
};

// ---------------------------------------------------------- code snippets --
const char *const kSnippetLangs[] = {"cpp", "python", "rust", "javascript", "sql", "bash", "go", "json", "typescript", "lua"};

std::string snippet(Rng &r, const std::string &lang, int scale)
{
    std::map<std::string, std::string> v = {
        {"@K", num(r.range(1, 999))}, {"@A", num(r.range(2, 64))}, {"@B", num(r.range(100, 99999))},
        {"@W", std::string(wordOf(r, Script::Latin))}, {"@U", std::string(r.pick(kCjk))}};
    std::string body;
    if (lang == "cpp") {
        body = R"__(// Sum the first n squares (overflow-checked).
template <typename T>
T sum_squares_@K(T n) {
    static_assert(std::is_integral_v<T>, "integral only");
    T total{};
    for (T i = 1; i <= n; ++i) {
        total += i * i; /* @W */
    }
    return total;
}
std::printf("%lld\n", static_cast<long long>(sum_squares_@K<long long>(@A)));)__";
    } else if (lang == "python") {
        body = R"__(def histogram_@K(values, bins=@A):
    """Bucket values into `bins` equal-width bins ("@W", @U)."""
    lo, hi = min(values), max(values)
    width = (hi - lo) / bins or 1.0
    counts = [0] * bins
    for v in values:
        counts[min(int((v - lo) / width), bins - 1)] += 1
    return {f"bin{i}": c for i, c in enumerate(counts)}  # @B)__";
    } else if (lang == "rust") {
        body = R"__(/// Parses "key=value" into a pair; @W.
fn parse_pair_@K<'a>(s: &'a str) -> Option<(&'a str, &'a str)> {
    let (k, v) = s.split_once('=')?;
    match k.trim() {
        "" => None,
        key => Some((key, v.trim())),
    }
}
const LIMIT_@K: u64 = 0x@B_u64;)__";
    } else if (lang == "javascript") {
        body = R"__(// Debounce @W events (@U).
export function debounce_@K(fn, ms = @A) {
  let timer = null;
  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
}
console.log(`ready: ${debounce_@K.name} @B`);)__";
    } else if (lang == "sql") {
        body = R"__(-- Children of a page in sibling order (@W).
SELECT p.id, p.title, COUNT(c.id) AS children
FROM pages p
LEFT JOIN pages c ON c.parent_id = p.id AND c.trashed = 0
WHERE p.parent_id = @B AND p.trashed = 0
GROUP BY p.id
ORDER BY p.ord
LIMIT @A;)__";
    } else if (lang == "bash") {
        body = R"__(#!/usr/bin/env bash
set -euo pipefail
# Run @A bench rounds for "@W".
for i in $(seq 1 @A); do
  ./Folia --bench open --bench-out "out_${i}.json" || echo "round $i failed" >&2
done
echo "done: @B")__";
    } else if (lang == "go") {
        body = R"__(// Clamp limits v to [lo, hi] (@W).
func Clamp@K(v, lo, hi int) int {
	if v < lo {
		return lo
	} else if v > hi {
		return hi
	}
	return v // @B
})__";
    } else if (lang == "json") {
        body = R"__({
  "name": "@W-@K",
  "threshold": 0.95,
  "limits": [@A, @B, 16.7],
  "label": "@U",
  "enabled": true
})__";
    } else if (lang == "typescript") {
        body = R"__(interface Sample@K {
  id: number;
  label: string; // "@W"
}
export const pick@K = <T extends Sample@K>(xs: T[], n = @A): T[] =>
  xs.filter((x) => x.id % n === 0).slice(0, @B);)__";
    } else {
        body = R"__(-- Lua: count words in "@W" (@U).
local function count_@K(s)
  local n = 0
  for _ in s:gmatch("%S+") do n = n + 1 end
  return n
end
print(count_@K("a b c"), @A))__";
    }
    std::string out = fill(body, v);
    // Larger notes get longer blocks: repeat the body with fresh identifiers.
    for (int i = 1; i < scale; ++i) {
        v["@K"] = num(r.range(1000, 99999));
        v["@A"] = num(r.range(2, 64));
        out += "\n\n" + fill(body, v);
    }
    return out;
}

// --------------------------------------------------------- display math --
// Returns {lang, tex}. kind 0: one-line fraction ("single"), 1: matrix,
// 2: aligned, 3+: further display formulas (math page / stress).
std::pair<std::string, std::string> displayMath(Rng &r, int kind)
{
    std::map<std::string, std::string> v = {{"@A", num(r.range(2, 9))}, {"@B", num(r.range(10, 99))},
                                            {"@C", num(r.range(1, 5))}};
    switch (kind % 9) {
    case 0:
        return {"single", fill("\\frac{a_{@A} + b}{c^{2} - d} = \\sum_{k=0}^{@B} \\binom{@B}{k} x^{k}", v)};
    case 1:
        return {"", fill("A = \\begin{pmatrix}\na_{11} & a_{12} & @A \\\\\na_{21} & a_{22} & @C \\\\\n0 & 0 & 1\n\\end{pmatrix}", v)};
    case 2:
        return {"", fill("\\begin{aligned}\nf(x) &= (x + @A)^2 \\\\\n&= x^2 + 2 \\cdot @A x + @A^2\n\\end{aligned}", v)};
    case 3:
        return {"", fill("|x| = \\begin{cases}\nx & \\text{if } x \\ge 0 \\\\\n-x & \\text{otherwise}\n\\end{cases}", v)};
    case 4:
        return {"single", fill("\\int_{0}^{@A} e^{-t^{2}}\\,dt \\approx \\frac{\\sqrt{\\pi}}{2}", v)};
    case 5:
        return {"single", fill("\\lim_{n \\to \\infty} \\left(1 + \\frac{1}{n}\\right)^{n} = e", v)};
    case 6:
        return {"", fill("\\begin{bmatrix}\n\\cos\\theta & -\\sin\\theta \\\\\n\\sin\\theta & \\cos\\theta\n\\end{bmatrix}\n\\begin{bmatrix} x \\\\ y \\end{bmatrix}", v)};
    case 7:
        return {"single", fill("x = \\frac{-b \\pm \\sqrt{b^{2} - 4ac}}{2a}, \\quad a \\ne 0, \\; b = @B", v)};
    default:
        return {"", fill("\\begin{gathered}\n\\nabla \\cdot \\mathbf{E} = \\frac{\\rho}{\\varepsilon_0} \\\\\n\\nabla \\times \\mathbf{B} = \\mu_0 \\mathbf{J} + \\mu_0 \\varepsilon_0 \\frac{\\partial \\mathbf{E}}{\\partial t}\n\\end{gathered}", v)};
    }
}

// ------------------------------------------------------- note generation --
struct NoteSpec {
    int blocks = 100;        // content blocks (links are appended separately)
    i64 targetBytes = 25 * 1024;
    int code = 3;
    int math = 3;
    int codeScale = 1;
    double paraWeightLo = 0.4, paraWeightHi = 1.6;
};

std::vector<Gen> generateNote(Rng &r, const NoteSpec &spec)
{
    const int n = spec.blocks;
    std::vector<Gen> out(n);
    std::vector<int> fixedKind(n, -1); // -1 free, 1 code, 2 math
    auto place = [&](int count, int kind, double phase) {
        for (int k = 0; k < count; ++k) {
            int pos = int((k + phase) * n / double(count));
            pos = std::max(1, std::min(n - 1, pos));
            while (pos < n && fixedKind[pos] != -1)
                ++pos;
            if (pos >= n) {
                pos = 1;
                while (pos < n && fixedKind[pos] != -1)
                    ++pos;
            }
            if (pos < n)
                fixedKind[pos] = kind;
        }
    };
    place(spec.code, 1, 0.3);
    place(spec.math, 2, 0.65);
    // Three distinct languages per note (rotating through the pool).
    std::vector<std::string> langs(std::begin(kSnippetLangs), std::end(kSnippetLangs));
    for (std::size_t i = langs.size() - 1; i > 0; --i)
        std::swap(langs[i], langs[r.below(std::uint32_t(i + 1))]);
    int codeIdx = 0, mathIdx = 0;
    int lastHeading = 0;
    int listLeft = 0;
    std::string listKind;
    int listNo = 0;
    for (int i = 0; i < n; ++i) {
        Gen &g = out[i];
        if (fixedKind[i] == 1) {
            g.kind = 1;
            g.lang = langs[codeIdx % langs.size()];
            g.text = snippet(r, g.lang, spec.codeScale);
            ++codeIdx;
            listLeft = 0;
            continue;
        }
        if (fixedKind[i] == 2) {
            auto m = displayMath(r, mathIdx++);
            g.kind = 2;
            g.lang = m.first;
            g.text = m.second;
            listLeft = 0;
            continue;
        }
        g.kind = 0;
        if (i == 0) {
            g.text = "# " + words(r, 2, 6);
            continue;
        }
        if (listLeft > 0) {
            --listLeft;
            if (listKind == "1.") {
                g.text = std::to_string(++listNo) + ". " + sentence(r, 4, 12, true);
            } else if (listKind == "task") {
                g.text = std::string(r.chance(0.4) ? "- [x] " : "- [ ] ") + sentence(r, 3, 9, true);
            } else {
                g.text = std::string(r.chance(0.25) ? "  - " : "- ") + sentence(r, 3, 12, true);
            }
            continue;
        }
        if (i - lastHeading >= r.range(8, 14)) {
            g.text = (r.chance(0.3) ? "### " : "## ") + words(r, 2, 6);
            lastHeading = i;
            continue;
        }
        const double x = r.unit();
        if (x < 0.62) {
            g.para = true;
            g.weight = spec.paraWeightLo + (spec.paraWeightHi - spec.paraWeightLo) * r.unit();
        } else if (x < 0.75) {
            listKind = "-";
            listLeft = r.range(2, 5);
            g.text = "- " + sentence(r, 3, 12, true);
        } else if (x < 0.83) {
            listKind = "1.";
            listLeft = r.range(2, 4);
            listNo = 1;
            g.text = "1. " + sentence(r, 4, 12, true);
        } else if (x < 0.88) {
            listKind = "task";
            listLeft = r.range(1, 3);
            g.text = "- [ ] " + sentence(r, 3, 9, true);
        } else if (x < 0.94) {
            g.text = "> " + sentence(r, 8, 22, true);
        } else if (x < 0.97) {
            g.text = "| " + words(r, 1, 2) + " | " + words(r, 1, 3) + " | " + num(r.range(1, 9999)) + " |";
        } else {
            g.para = true; // a short single-sentence paragraph
            g.weight = 0.15;
        }
    }
    // Fill paragraphs to hit the byte target, correcting drift as we go.
    i64 fixed = 0;
    double sumW = 0;
    for (const Gen &g : out) {
        if (g.para)
            sumW += g.weight;
        else
            fixed += i64(g.text.size());
    }
    const i64 remaining = std::max<i64>(0, spec.targetBytes - fixed);
    double cumTarget = 0;
    i64 cumActual = 0;
    for (Gen &g : out) {
        if (!g.para)
            continue;
        cumTarget += sumW > 0 ? remaining * g.weight / sumW : 0;
        const i64 budget = i64(cumTarget) - cumActual;
        do {
            if (!g.text.empty())
                g.text += ' ';
            g.text += sentence(r, 6, 18, true);
        } while (i64(g.text.size()) < budget - 40);
        cumActual += i64(g.text.size());
    }
    return out;
}

std::vector<Gen> shortBody(Rng &r)
{
    std::vector<Gen> out;
    const int n = r.range(1, 5);
    for (int i = 0; i < n; ++i) {
        Gen g;
        const double x = r.unit();
        if (x < 0.66) {
            g.text = sentence(r, 4, 16, true);
            if (r.chance(0.35))
                g.text += ' ' + sentence(r, 4, 14, true);
        } else if (x < 0.76) {
            g.text = "- " + sentence(r, 3, 10, true);
        } else if (x < 0.83) {
            g.text = "## " + words(r, 1, 4);
        } else if (x < 0.88) {
            g.text = std::string(r.chance(0.5) ? "- [ ] " : "- [x] ") + sentence(r, 2, 8, false);
        } else if (x < 0.92) {
            g.text = "> " + sentence(r, 5, 14, true);
        } else if (x < 0.96) {
            auto m = displayMath(r, r.chance(0.5) ? 0 : 4);
            g.kind = 2;
            g.lang = m.first;
            g.text = m.second;
        } else {
            g.kind = 1;
            g.lang = r.chance(0.5) ? "python" : "bash";
            g.text = snippet(r, g.lang, 1);
        }
        out.push_back(std::move(g));
    }
    return out;
}

// ------------------------------------------------------- huge C++ source --
const char *const kCppUnits[] = {
R"__(/**
 * RingBuffer@K: fixed-capacity FIFO used by sampler @A.
 * Thread-compatible, not thread-safe. "Quoted" text and a \backslash.
 */
template <typename T, std::size_t Capacity = @A>
class RingBuffer@K {
public:
    static_assert(Capacity > 0, "capacity must be positive");
    using value_type = T;

    bool push(const T &value) noexcept {
        if (size_ == Capacity) return false; // full
        data_[(head_ + size_) % Capacity] = value;
        ++size_;
        return true;
    }
    std::optional<T> pop() {
        if (size_ == 0) { return std::nullopt; }
        T v = std::move(data_[head_]);
        head_ = (head_ + 1) % Capacity;
        --size_;
        return v;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::array<T, Capacity> data_{};
    std::size_t head_ = 0, size_ = 0;
};
)__",
R"__(// Parses "key=value;key2=value2" pairs; returns the number parsed.
int parse_pairs_@K(std::string_view in, std::map<std::string, std::string> &out)
{
    int count = 0;
    std::size_t pos = 0;
    while (pos < in.size()) {
        const auto eq = in.find('=', pos);
        const auto end = in.find(';', pos);
        if (eq == std::string_view::npos) break;
        out.emplace(std::string(in.substr(pos, eq - pos)),
                    std::string(in.substr(eq + 1, (end == std::string_view::npos ? in.size() : end) - eq - 1)));
        ++count;
        pos = end == std::string_view::npos ? in.size() : end + 1;
    }
    std::printf("parsed %d pairs from \"%.*s\" (limit @B)\n", count, int(in.size()), in.data());
    return count;
}
)__",
R"__(constexpr double kScale@K = 6.022'140'76e23 / 1'000'000.0;
constexpr std::uint64_t kMask@K = 0xFFFF'0000'@H'ABCDull;
constexpr int kBits@K = 0b1011'0110;
constexpr float kRatio@K = 0.@Bf;
static_assert(sizeof(void *) == 8 || sizeof(void *) == 4, "pointer size");

template <class It, class Pred>
constexpr It find_if_@K(It first, It last, Pred p) {
    for (; first != last; ++first)
        if (p(*first)) return first;
    return last;
}
)__",
R"__(enum class Mode@K : std::uint8_t { Idle = 0, Running, Paused, Stopped = 0x7F };

const char *to_string(Mode@K m) {
    switch (m) {
    case Mode@K::Idle: return "idle";
    case Mode@K::Running: return "running";
    case Mode@K::Paused: return "paused";
    case Mode@K::Stopped: return "stopped";
    }
    return "?";
}

static const char *kConfig@K = R"json({
  "name": "fixture-@K",
  "threshold": 0.95,
  "tags": ["a", "b", "\u00e9"]
})json";
static const char16_t *kWide@K = u"wide \"text\" @K";
static const char kNewline@K = '\n', kQuote@K = '\'';
)__",
R"__(/*
 * Section @K: numeric helpers.
 *   - saturating add
 *   - make_counter (stateful lambda)
 */
#define FOLIA_FIXTURE_CHECK_@K(cond)                              \
    do {                                                          \
        if (!(cond))                                              \
            std::fprintf(stderr, "check failed: %s\n", #cond);    \
    } while (0)

template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
T saturating_add_@K(T a, T b) noexcept {
    T r{};
    if (__builtin_add_overflow(a, b, &r))
        return b > 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
    return r;
}

auto make_counter_@K(int start = @A) {
    return [n = start](int step = 1) mutable { return n += step; };
}
)__",
R"__(struct Point@K {
    double x = 0.0, y = 0.0;
    Point@K operator+(const Point@K &o) const { return {x + o.x, y + o.y}; }
    bool operator==(const Point@K &o) const = default;
};

double polyline_length_@K(const std::vector<Point@K> &pts) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y;
        total += std::sqrt(dx * dx + dy * dy); // Euclidean distance
    }
    return total * 1.0e-3 + @B.5;
}
)__",
R"__(#if defined(__APPLE__) && !defined(FOLIA_NO_SIMD_@K)
// Apple silicon path: NEON is always available on arm64.
static inline int dot_@K(const std::int16_t *a, const std::int16_t *b, int n) {
    int acc = 0;
    for (int i = 0; i < n; ++i) acc += a[i] * b[i];
    return acc;
}
#else
static inline int dot_@K(const std::int16_t *, const std::int16_t *, int) { return 0; }
#endif // FOLIA_NO_SIMD_@K

static const int kTable@K[] = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025, @B};
)__",
R"__(namespace detail_@K {
template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

inline std::string describe(const std::variant<int, double, std::string> &v) {
    return std::visit(overloaded{
        [](int i) { return "int:" + std::to_string(i); },
        [](double d) { return "double:" + std::to_string(d); },
        [](const std::string &s) { return "string:\"" + s + "\""; },
    }, v);
}
} // namespace detail_@K
)__",
};

std::string hugeCpp(Rng &r, i64 targetBytes, u64 seed)
{
    std::string out;
    out.reserve(std::size_t(targetBytes) + 8192);
    out += "// SPDX-License-Identifier: MIT\n";
    out += "// Generated fixture (folia_fixturegen, seed " + std::to_string(seed) + "): one large C++ translation unit.\n";
    out += "// It is highlighted, never compiled.\n";
    for (const char *h : {"algorithm", "array", "cmath", "cstdint", "cstdio", "limits", "map", "optional", "string",
                          "string_view", "type_traits", "variant", "vector"})
        out += std::string("#include <") + h + ">\n";
    out += "\nnamespace fixture {\n\n";
    const std::string tail = "} // namespace fixture\n";
    int k = 0;
    while (i64(out.size() + tail.size()) < targetBytes) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04X", unsigned(r.below(0x10000)));
        const std::map<std::string, std::string> v = {
            {"@K", num(++k)}, {"@A", num(r.range(2, 512))}, {"@B", num(r.range(10, 99999))}, {"@H", hex}};
        out += fill(kCppUnits[r.below(std::uint32_t(sizeof kCppUnits / sizeof kCppUnits[0]))], v);
        out += '\n';
    }
    out += tail;
    return out;
}

// --------------------------------------------------------------- tree ----
enum class Role { Fixtures, Ordinary, OrdinaryChild, Stress, Code, Math, Broad, BroadChild, Deep, Root, Random, Sample };

struct PageRec {
    i64 id = 0;
    i64 parent = 0;
    i64 ord = 0;
    int depth = 1;
    Role role = Role::Random;
    std::vector<i64> kids;
};

struct Options {
    QString out;
    QString manifest;
    u64 seed = 1;
    i64 pages = 50000;
    bool force = false;
    bool verify = false;
};

[[noreturn]] void die(const char *fmt, const char *arg = "")
{
    std::fprintf(stderr, "folia_fixturegen: ");
    std::fprintf(stderr, fmt, arg);
    std::fprintf(stderr, "\n");
    std::exit(2);
}

bool removeDbFiles(const QString &path, bool force)
{
    if (QFileInfo::exists(path)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QByteArray head = f.read(16);
        f.close();
        const bool sqlite = head.startsWith(QByteArrayLiteral("SQLite format 3"));
        if (!sqlite && !head.isEmpty() && !force)
            die("refusing to overwrite %s: not an SQLite database (pass --force)", qPrintable(path));
        if (!QFile::remove(path))
            return false;
    }
    for (const char *suffix : {"-wal", "-shm", ".lock"})
        QFile::remove(path + QLatin1String(suffix));
    return true;
}

void exec(sqlite3 *db, const char *sql)
{
    char *msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        std::fprintf(stderr, "folia_fixturegen: SQL failed: %s\n  %s\n", msg ? msg : "?", sql);
        std::exit(3);
    }
}

struct Stats {
    i64 blocks = 0;
    i64 byKind[4] = {0, 0, 0, 0};
    i64 textBytes = 0;
    i64 maxDepth = 0;
};

struct PageSize {
    i64 blocks = 0;
    i64 textBytes = 0;
    i64 links = 0;
};

// ------------------------------------------------------------- verify ----
bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QEventLoop loop;
        QTimer::singleShot(1, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

QJsonObject verifyFixture(const QString &path, const QJsonObject &manifest, const std::map<i64, PageSize> &expected)
{
    QJsonObject res;
    folia::Store store;
    folia::Store::Options so;
    so.path = path;
    so.singleInstanceLock = false;
    QString err;
    if (!store.open(so, &err)) {
        res.insert(QStringLiteral("ok"), false);
        res.insert(QStringLiteral("error"), err);
        return res;
    }
    bool allOk = store.pageCount() == manifest.value(QStringLiteral("pages")).toInteger();
    res.insert(QStringLiteral("page_count_ok"), allOk);
    folia::EditorContext ectx;
    QJsonArray pages;
    QElapsedTimer t;
    for (const auto &e : expected) {
        const i64 id = e.first;
        folia::PageContent pc;
        bool got = false;
        QObject guard;
        t.restart();
        store.openPage(id, &guard, [&](folia::PageContent c) {
            pc = std::move(c);
            got = true;
        });
        waitUntil([&] { return got; }, 60000);
        const double openMs = t.nsecsElapsed() / 1e6;
        QJsonObject p;
        p.insert(QStringLiteral("id"), double(id));
        p.insert(QStringLiteral("blocks"), int(pc.blocks.size()));
        bool ok = got && pc.found && pc.blocks.size() == e.second.blocks;
        t.restart();
        folia::PageSession s(id, &ectx, [&store] { return store.allocateBlockId(); });
        s.load(pc);
        const double loadMs = t.nsecsElapsed() / 1e6;
        for (QTextBlock b = s.document()->firstBlock(); b.isValid(); b = b.next())
            if (folia::BlockData *d = folia::PageSession::dataOf(b))
                d->dirty = true;
        s.tracker()->structureChanged = true;
        const folia::SaveBatch batch = s.takeSaveBatch();
        bool same = batch.order.size() == pc.blocks.size() && batch.upserts.size() == pc.blocks.size();
        for (int i = 0; same && i < pc.blocks.size(); ++i) {
            const folia::BlockRecord &a = pc.blocks[i];
            const folia::BlockRecord &b = batch.upserts[i];
            same = batch.order[i] == a.id && b.id == a.id && b.kind == a.kind && b.lang == a.lang
                && b.text == a.text && b.target == a.target;
            if (!same)
                p.insert(QStringLiteral("first_mismatch_index"), i);
        }
        ok = ok && same;
        p.insert(QStringLiteral("editor_lines"), s.lineCount());
        p.insert(QStringLiteral("roundtrip_ok"), same);
        p.insert(QStringLiteral("store_open_ms"), openMs);
        p.insert(QStringLiteral("session_load_ms"), loadMs);
        pages.append(p);
        allOk = allOk && ok;
    }
    res.insert(QStringLiteral("pages"), pages);
    // Broad branch child count and deep branch depth through the store's own queries.
    const i64 broad = manifest.value(QStringLiteral("broad_parent")).toInteger();
    int total = -1;
    QObject guard;
    bool got = false;
    store.fetchChildren(broad, 0, 1, &guard, [&](QVector<folia::PageMeta>, int n) {
        total = n;
        got = true;
    });
    waitUntil([&] { return got; }, 60000);
    res.insert(QStringLiteral("broad_children_seen"), total);
    allOk = allOk && total == manifest.value(QStringLiteral("broad_children")).toInt();
    got = false;
    int chain = -1;
    store.ancestors(manifest.value(QStringLiteral("deep_leaf")).toInteger(), &guard, [&](QVector<folia::PageMeta> rows) {
        chain = rows.size();
        got = true;
    });
    waitUntil([&] { return got; }, 60000);
    res.insert(QStringLiteral("deep_leaf_ancestor_chain"), chain);
    allOk = allOk && chain == manifest.value(QStringLiteral("deep_leaf_depth")).toInt();
    store.close();
    res.insert(QStringLiteral("ok"), allOk);
    return res;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    bool haveOut = false, haveSeed = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc)
                die("missing value for %s", a.c_str());
            return argv[++i];
        };
        if (a == "--out") {
            opt.out = QString::fromLocal8Bit(val());
            haveOut = true;
        } else if (a == "--seed") {
            opt.seed = std::strtoull(val(), nullptr, 0);
            haveSeed = true;
        } else if (a == "--pages") {
            opt.pages = std::strtoll(val(), nullptr, 10);
        } else if (a == "--manifest") {
            opt.manifest = QString::fromLocal8Bit(val());
        } else if (a == "--force") {
            opt.force = true;
        } else if (a == "--verify") {
            opt.verify = true;
        } else if (a == "-h" || a == "--help") {
            std::printf("usage: folia_fixturegen --out <db> --seed <n> [--pages 50000] [--manifest <json>] [--force] [--verify]\n");
            return 0;
        } else {
            die("unknown argument %s", a.c_str());
        }
    }
    if (!haveOut || !haveSeed)
        die("--out and --seed are required%s", "");
    if (opt.pages < 200)
        die("--pages must be >= 200%s", "");

    // --verify loads pages into a real editor session, which needs a GUI app.
    std::unique_ptr<QCoreApplication> app;
    if (opt.verify) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        app = std::make_unique<QGuiApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }

    QElapsedTimer total;
    total.start();
    const QString path = QFileInfo(opt.out).absoluteFilePath();
    if (!removeDbFiles(path, opt.force))
        die("cannot remove existing %s", qPrintable(path));

    // 1. Schema through the real store (migrations), then close it.
    int schemaVersion = 0;
    {
        folia::Store store;
        folia::Store::Options so;
        so.path = path;
        so.singleInstanceLock = false;
        QString err;
        if (!store.open(so, &err))
            die("Store::open failed: %s", qPrintable(err));
        store.close();
    }

    // 2. Tree shape (main stream).
    Rng tr(streamSeed(opt.seed, 0));
    std::vector<PageRec> pages;
    pages.reserve(std::size_t(opt.pages));
    std::vector<i64> topLevel;
    auto addPage = [&](i64 parent, Role role) -> i64 {
        PageRec p;
        p.id = i64(pages.size()) + 1;
        p.parent = parent;
        p.role = role;
        std::vector<i64> &sib = parent ? pages[std::size_t(parent - 1)].kids : topLevel;
        sib.push_back(p.id);
        p.ord = i64(sib.size()) * kGap;
        p.depth = parent ? pages[std::size_t(parent - 1)].depth + 1 : 1;
        pages.push_back(p);
        return p.id;
    };
    const i64 N = opt.pages;
    const i64 broadChildren = std::min<i64>(5000, N / 5);
    const i64 deepLevels = std::min<i64>(256, N / 10);
    const i64 sampleCount = std::min<i64>(160, N / 20);
    const int ordinaryKids = 3;

    const i64 fixtures = addPage(0, Role::Fixtures);
    const i64 ordinary = addPage(fixtures, Role::Ordinary);
    for (int i = 0; i < ordinaryKids; ++i)
        addPage(ordinary, Role::OrdinaryChild);
    const i64 stress = addPage(fixtures, Role::Stress);
    const i64 code = addPage(fixtures, Role::Code);
    const i64 math = addPage(fixtures, Role::Math);
    const i64 broad = addPage(0, Role::Broad);
    for (i64 i = 0; i < broadChildren; ++i)
        addPage(broad, Role::BroadChild);
    const i64 deepRoot = addPage(0, Role::Deep);
    i64 deepLeaf = deepRoot;
    for (i64 i = 1; i < deepLevels; ++i)
        deepLeaf = addPage(deepLeaf, Role::Deep);
    std::vector<i64> roots;
    for (int i = 0; i < 17 && i64(pages.size()) < N; ++i)
        roots.push_back(addPage(0, Role::Root));
    std::vector<i64> pool(roots);
    const i64 remaining = N - i64(pages.size());
    std::set<i64> sampleSlots;
    while (i64(sampleSlots.size()) < std::min(sampleCount, remaining))
        sampleSlots.insert(i64(tr.below(std::uint32_t(remaining))));
    std::vector<i64> samples;
    for (i64 j = 0; j < remaining; ++j) {
        i64 parent = 0;
        for (int attempt = 0; attempt < 8 && !parent; ++attempt) {
            i64 cand;
            if (tr.chance(0.03))
                cand = roots[tr.below(std::uint32_t(roots.size()))];
            else if (tr.chance(0.5) && pool.size() > 500)
                cand = pool[pool.size() - 1 - tr.below(500)];
            else
                cand = pool[tr.below(std::uint32_t(pool.size()))];
            if (pages[std::size_t(cand - 1)].depth < 14)
                parent = cand;
        }
        if (!parent)
            parent = roots[tr.below(std::uint32_t(roots.size()))];
        const bool sample = sampleSlots.count(j) > 0;
        const i64 id = addPage(parent, sample ? Role::Sample : Role::Random);
        if (sample)
            samples.push_back(id);
        else
            pool.push_back(id);
    }
    const double treeMs = total.nsecsElapsed() / 1e6;

    // 3. Rows, raw SQLite, a few big transactions.
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        die("cannot open %s", qPrintable(path));
    exec(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-65536;");
    {
        sqlite3_stmt *v = nullptr;
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &v, nullptr);
        if (sqlite3_step(v) == SQLITE_ROW)
            schemaVersion = sqlite3_column_int(v, 0);
        sqlite3_finalize(v);
    }
    Fnv hash;
    Stats st;
    std::map<i64, PageSize> sizes;
    QElapsedTimer wt;
    wt.start();

    sqlite3_stmt *insPage = nullptr, *insBlock = nullptr;
    sqlite3_prepare_v2(db,
                       "INSERT INTO pages(id, parent_id, ord, title, rev, created_at, updated_at) "
                       "VALUES(?1, ?2, ?3, ?4, 1, ?5, ?6)", -1, &insPage, nullptr);
    sqlite3_prepare_v2(db,
                       "INSERT INTO blocks(id, page_id, ord, kind, lang, text, target) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)",
                       -1, &insBlock, nullptr);
    auto step = [&](sqlite3_stmt *s) {
        if (sqlite3_step(s) != SQLITE_DONE) {
            std::fprintf(stderr, "folia_fixturegen: insert failed: %s\n", sqlite3_errmsg(db));
            std::exit(3);
        }
        sqlite3_reset(s);
    };

    exec(db, "BEGIN");
    for (const PageRec &p : pages) {
        Rng r(streamSeed(opt.seed, u64(p.id) * 2 + 1));
        std::string title;
        switch (p.role) {
        case Role::Fixtures: title = u8"Fixtures \u00B7 benchmark pages"; break;
        case Role::Ordinary: title = u8"Ordinary note \u00B7 cafe\u0301 \u00B7 東京 \u00B7 \U0001F4DD"; break;
        case Role::Stress: title = u8"Stress note \u00B7 10 MiB \u00B7 \u0928\u092E\u0938\u094D\u0924\u0947"; break;
        case Role::Code: title = "Huge code note (1 MiB C++)"; break;
        case Role::Math: title = u8"Formula gallery \u00B7 \u2211 \u222B \u221A"; break;
        case Role::Broad:
            title = u8"Broad branch \u00B7 " + std::to_string(p.kids.size()) + u8" children \u00B7 \u0645\u0631\u062D\u0628\u0627";
            break;
        case Role::Deep:
            title = (p.id == deepRoot ? std::string("Deep branch") : pageTitle(r)) + u8" \u00B7 L" + std::to_string(p.depth);
            break;
        default: title = pageTitle(r); break;
        }
        const i64 created = kBaseMs + p.id * 1000;
        const i64 updated = created + i64(r.below(86400)) * 1000;
        sqlite3_bind_int64(insPage, 1, p.id);
        sqlite3_bind_int64(insPage, 2, p.parent);
        sqlite3_bind_int64(insPage, 3, p.ord);
        sqlite3_bind_text(insPage, 4, title.data(), int(title.size()), SQLITE_STATIC);
        sqlite3_bind_int64(insPage, 5, created);
        sqlite3_bind_int64(insPage, 6, updated);
        step(insPage);
        hash.bytes("P", 1);
        hash.num(p.id);
        hash.num(p.parent);
        hash.num(p.ord);
        hash.str(title);
        hash.num(1);
        hash.num(created);
        hash.num(updated);
        st.maxDepth = std::max<i64>(st.maxDepth, p.depth);
    }
    exec(db, "COMMIT");

    i64 nextBlock = 1;
    const std::set<Role> tracked = {Role::Ordinary, Role::Stress, Role::Code, Role::Math, Role::Broad, Role::Sample};
    exec(db, "BEGIN");
    for (std::size_t pi = 0; pi < pages.size(); ++pi) {
        const PageRec &p = pages[pi];
        Rng r(streamSeed(opt.seed, u64(p.id) * 2 + 2));
        std::vector<Gen> body;
        switch (p.role) {
        case Role::Ordinary:
        case Role::Sample: {
            NoteSpec ns;
            ns.blocks = 100 - int(p.kids.size());
            body = generateNote(r, ns);
            break;
        }
        case Role::Stress: {
            NoteSpec ns;
            ns.blocks = 10000 - int(p.kids.size());
            ns.targetBytes = 10ll << 20;
            ns.code = 300;
            ns.math = 300;
            ns.codeScale = 2;
            ns.paraWeightLo = 0.6;
            ns.paraWeightHi = 2.4;
            body = generateNote(r, ns);
            break;
        }
        case Role::Math: {
            NoteSpec ns;
            ns.blocks = 100;
            ns.targetBytes = 16 * 1024;
            ns.code = 1;
            ns.math = 30;
            body = generateNote(r, ns);
            break;
        }
        case Role::Code: {
            Gen g;
            g.kind = 1;
            g.lang = "cpp";
            g.text = hugeCpp(r, 1ll << 20, opt.seed);
            body.push_back(std::move(g));
            break;
        }
        case Role::Fixtures:
        case Role::Broad:
        case Role::Deep:
        case Role::Root: {
            Gen g;
            g.text = sentence(r, 6, 14, false);
            body.push_back(std::move(g));
            if (p.role != Role::Deep) {
                std::vector<Gen> more = shortBody(r);
                body.insert(body.end(), more.begin(), more.end());
            }
            break;
        }
        default:
            // Leaves: most get a short body (1-5 blocks); some stay title-only.
            if (!p.kids.empty() || r.chance(0.86))
                body = shortBody(r);
            break;
        }
        for (i64 kid : p.kids) {
            Gen g;
            g.kind = 3;
            g.target = kid;
            body.push_back(std::move(g));
        }
        PageSize ps;
        i64 ord = 0;
        for (const Gen &g : body) {
            const i64 id = nextBlock++;
            ord += kGap;
            sqlite3_bind_int64(insBlock, 1, id);
            sqlite3_bind_int64(insBlock, 2, p.id);
            sqlite3_bind_int64(insBlock, 3, ord);
            sqlite3_bind_int(insBlock, 4, g.kind);
            sqlite3_bind_text(insBlock, 5, g.lang.data(), int(g.lang.size()), SQLITE_STATIC);
            sqlite3_bind_text(insBlock, 6, g.text.data(), int(g.text.size()), SQLITE_STATIC);
            sqlite3_bind_int64(insBlock, 7, g.target);
            step(insBlock);
            hash.bytes("B", 1);
            hash.num(id);
            hash.num(p.id);
            hash.num(ord);
            hash.num(g.kind);
            hash.str(g.lang);
            hash.str(g.text);
            hash.num(g.target);
            ++st.blocks;
            ++st.byKind[g.kind];
            st.textBytes += i64(g.text.size());
            ++ps.blocks;
            ps.textBytes += i64(g.text.size());
            if (g.kind == 3)
                ++ps.links;
        }
        if (tracked.count(p.role))
            sizes[p.id] = ps;
        if (pi % 5000 == 4999) {
            exec(db, "COMMIT");
            exec(db, "BEGIN");
        }
    }
    exec(db, "COMMIT");
    sqlite3_finalize(insPage);
    sqlite3_finalize(insBlock);

    char hashHex[32];
    std::snprintf(hashHex, sizeof hashHex, "fnv1a64:%016llx", static_cast<unsigned long long>(hash.h));
    {
        sqlite3_stmt *kv = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO kv(key, value) VALUES(?1, ?2)", -1, &kv, nullptr);
        const std::string seedStr = std::to_string(opt.seed);
        const std::pair<const char *, std::string> rows[] = {
            {"fixture.generator", "folia_fixturegen/" + std::to_string(kGeneratorVersion)},
            {"fixture.seed", seedStr},
            {"fixture.content_hash", hashHex}};
        for (const auto &row : rows) {
            sqlite3_bind_text(kv, 1, row.first, -1, SQLITE_STATIC);
            sqlite3_bind_text(kv, 2, row.second.data(), int(row.second.size()), SQLITE_STATIC);
            step(kv);
        }
        sqlite3_finalize(kv);
    }
    exec(db, "PRAGMA journal_mode=WAL");
    sqlite3_close(db);
    const double writeMs = wt.nsecsElapsed() / 1e6;
    const double genMs = total.nsecsElapsed() / 1e6;
    const qint64 dbBytes = QFileInfo(path).size();

    // 4. Manifest.
    QJsonObject m;
    m.insert(QStringLiteral("generator"), QStringLiteral("folia_fixturegen"));
    m.insert(QStringLiteral("generator_version"), kGeneratorVersion);
    if (opt.seed < (u64(1) << 53))
        m.insert(QStringLiteral("seed"), double(opt.seed));
    else
        m.insert(QStringLiteral("seed"), QString::number(opt.seed));
    m.insert(QStringLiteral("schema_version"), schemaVersion);
    m.insert(QStringLiteral("db_path"), path);
    m.insert(QStringLiteral("db_bytes"), double(dbBytes));
    m.insert(QStringLiteral("pages"), double(pages.size()));
    m.insert(QStringLiteral("blocks"), double(st.blocks));
    QJsonObject kinds;
    kinds.insert(QStringLiteral("text"), double(st.byKind[0]));
    kinds.insert(QStringLiteral("code"), double(st.byKind[1]));
    kinds.insert(QStringLiteral("math"), double(st.byKind[2]));
    kinds.insert(QStringLiteral("page_link"), double(st.byKind[3]));
    m.insert(QStringLiteral("block_kinds"), kinds);
    m.insert(QStringLiteral("text_bytes_utf8"), double(st.textBytes));
    m.insert(QStringLiteral("top_level_pages"), int(topLevel.size()));
    m.insert(QStringLiteral("max_depth"), double(st.maxDepth));
    m.insert(QStringLiteral("page"), double(ordinary)); // default for scenarios taking a single page
    m.insert(QStringLiteral("ordinary_page"), double(ordinary));
    m.insert(QStringLiteral("stress_page"), double(stress));
    m.insert(QStringLiteral("code_page"), double(code));
    m.insert(QStringLiteral("math_page"), double(math));
    m.insert(QStringLiteral("fixtures_parent"), double(fixtures));
    m.insert(QStringLiteral("broad_parent"), double(broad));
    m.insert(QStringLiteral("broad_children"), double(broadChildren));
    m.insert(QStringLiteral("deep_root"), double(deepRoot));
    m.insert(QStringLiteral("deep_leaf"), double(deepLeaf));
    m.insert(QStringLiteral("deep_leaf_depth"), double(pages[std::size_t(deepLeaf - 1)].depth));
    QJsonArray sampleArr;
    for (i64 id : samples)
        sampleArr.append(double(id));
    m.insert(QStringLiteral("sample_pages"), sampleArr);
    auto sizeObj = [&](i64 id) {
        const PageSize &ps = sizes[id];
        QJsonObject o;
        o.insert(QStringLiteral("id"), double(id));
        o.insert(QStringLiteral("blocks"), double(ps.blocks));
        o.insert(QStringLiteral("text_bytes_utf8"), double(ps.textBytes));
        o.insert(QStringLiteral("page_links"), double(ps.links));
        return o;
    };
    QJsonObject ps;
    ps.insert(QStringLiteral("ordinary"), sizeObj(ordinary));
    ps.insert(QStringLiteral("stress"), sizeObj(stress));
    ps.insert(QStringLiteral("code"), sizeObj(code));
    ps.insert(QStringLiteral("math"), sizeObj(math));
    ps.insert(QStringLiteral("broad"), sizeObj(broad));
    i64 sMin = -1, sMax = 0, sSum = 0;
    bool samplesExact = true;
    for (i64 id : samples) {
        const PageSize &s = sizes[id];
        sMin = sMin < 0 ? s.textBytes : std::min(sMin, s.textBytes);
        sMax = std::max(sMax, s.textBytes);
        sSum += s.textBytes;
        samplesExact = samplesExact && s.blocks == 100;
    }
    QJsonObject so;
    so.insert(QStringLiteral("count"), double(samples.size()));
    so.insert(QStringLiteral("all_have_100_blocks"), samplesExact);
    so.insert(QStringLiteral("text_bytes_min"), double(sMin));
    so.insert(QStringLiteral("text_bytes_max"), double(sMax));
    so.insert(QStringLiteral("text_bytes_mean"), samples.empty() ? 0.0 : double(sSum) / double(samples.size()));
    ps.insert(QStringLiteral("samples"), so);
    m.insert(QStringLiteral("page_sizes"), ps);
    m.insert(QStringLiteral("content_hash"), QString::fromLatin1(hashHex));
    m.insert(QStringLiteral("hash_algorithm"),
             QStringLiteral("FNV-1a 64 over page rows then block rows in insertion order; ints as 8 LE bytes, "
                            "strings as 8-byte length + UTF-8 (tools/fixturegen.cpp)"));
    QJsonObject timing;
    timing.insert(QStringLiteral("tree_ms"), treeMs);
    timing.insert(QStringLiteral("write_ms"), writeMs);
    timing.insert(QStringLiteral("total_ms"), genMs);
    m.insert(QStringLiteral("generation"), timing);

    if (opt.verify) {
        std::map<i64, PageSize> expect;
        for (i64 id : {ordinary, stress, code, math, broad})
            expect[id] = sizes[id];
        for (std::size_t i = 0; i < samples.size() && i < 8; ++i)
            expect[samples[i]] = sizes[samples[i]];
        QElapsedTimer vt;
        vt.start();
        QJsonObject v = verifyFixture(path, m, expect);
        v.insert(QStringLiteral("verify_ms"), vt.nsecsElapsed() / 1e6);
        m.insert(QStringLiteral("verify"), v);
        // The verifier opened the store (WAL); the file size may change slightly.
        m.insert(QStringLiteral("db_bytes_after_verify"), double(QFileInfo(path).size()));
    }

    const QByteArray json = QJsonDocument(m).toJson(QJsonDocument::Indented);
    if (!opt.manifest.isEmpty()) {
        QFile f(opt.manifest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            die("cannot write manifest %s", qPrintable(opt.manifest));
        f.write(json);
        std::fprintf(stderr, "folia_fixturegen: %lld pages, %lld blocks, %lld text bytes, db %lld bytes, %s, %.0f ms\n",
                     static_cast<long long>(pages.size()), static_cast<long long>(st.blocks),
                     static_cast<long long>(st.textBytes), static_cast<long long>(dbBytes), hashHex, genMs);
    } else {
        std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout);
    }
    if (opt.verify && !m.value(QStringLiteral("verify")).toObject().value(QStringLiteral("ok")).toBool()) {
        std::fprintf(stderr, "folia_fixturegen: verification FAILED\n");
        return 1;
    }
    return 0;
}
