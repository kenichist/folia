// Tests for the folia_code highlighter (see src/code/Highlighter.h and
// docs/CODE_LANGUAGES.md).
//
// Slot order matters for lazyLoading(): it must run first, in a process where
// no language has been constructed yet.
#include <QtTest>

#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>

#include "code/Highlighter.h"

using namespace folia::code;

// ---------------------------------------------------------------------------
// Allocation tracking (to prove highlightLine allocates nothing beyond `out`).
// ---------------------------------------------------------------------------
static std::atomic<bool> g_track{false};
static std::atomic<long> g_allocs{0};

#ifdef __APPLE__
// Qt containers allocate with malloc/realloc directly (not operator new), so on
// macOS also count raw allocations through the malloc stack-logging hook.
extern "C" {
extern void (*malloc_logger)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                             uint32_t num_hot_frames_to_skip);
}
static void countingMallocLogger(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    if ((type & 2u) && g_track.load(std::memory_order_relaxed)) // stack_logging_type_alloc
        g_allocs.fetch_add(1, std::memory_order_relaxed);
}
#endif

void *operator new(std::size_t n)
{
    if (g_track.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    void *p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

static const char *tokenName(Token t)
{
    static const char *names[] = {"Plain",  "Keyword",  "Type",     "Builtin",  "Function", "String",
                                  "Number", "Comment",  "Operator", "Punctuation", "Preprocessor",
                                  "Variable", "Constant", "Tag",    "Attribute"};
    const int i = int(t);
    return i >= 0 && i < int(Token::Count) ? names[i] : "<mixed/not-found>";
}

namespace QTest {
template <> inline char *toString(const Token &t) { return qstrdup(tokenName(t)); }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

constexpr Token kMixed = Token::Count;

// A highlighted multi-line block, with the per-line incoming/outgoing state that
// an editor would store.
struct Block {
    const Language *lang = nullptr;
    QStringList lines;
    QVector<QVector<Span>> spans;
    QVector<int> in;
    QVector<int> out;
};

Block runBlock(const QString &langName, const QString &text)
{
    Block b;
    b.lang = languageFor(langName);
    b.lines = text.split(QLatin1Char('\n'));
    int st = 0;
    for (const QString &ln : std::as_const(b.lines)) {
        QVector<Span> sp;
        const int o = highlightLine(b.lang, ln, st, sp);
        b.in.append(st);
        b.out.append(o);
        b.spans.append(sp);
        st = o;
    }
    return b;
}

// Token covering every character of `needle` (first occurrence at/after the
// `occ`-th), Plain if uncovered, kMixed if the range is not uniformly one token
// or the needle is missing.
Token tokOf(const Block &b, int li, const QString &needle, int occ = 0)
{
    if (li < 0 || li >= b.lines.size())
        return kMixed;
    const QString &ln = b.lines[li];
    int pos = -1;
    for (int k = 0; k <= occ; ++k) {
        pos = ln.indexOf(needle, pos + 1);
        if (pos < 0)
            return kMixed;
    }
    QVector<Token> per(ln.size(), Token::Plain);
    for (const Span &s : b.spans[li])
        for (int k = s.start; k < s.start + s.length; ++k)
            per[k] = s.token;
    const Token t = per[pos];
    for (int k = pos; k < pos + needle.size(); ++k)
        if (per[k] != t)
            return kMixed;
    return t;
}

int countTok(const Block &b, int li, Token tok)
{
    int n = 0;
    for (const Span &s : b.spans[li])
        n += s.token == tok;
    return n;
}

#define EXPECT_TOK(block, line, needle, tok) QCOMPARE(tokOf((block), (line), QStringLiteral(needle)), (tok))
#define EXPECT_TOK_N(block, line, needle, occ, tok) \
    QCOMPARE(tokOf((block), (line), QStringLiteral(needle), (occ)), (tok))

// Invariants required by the contract. Returns an empty string if OK.
QString checkSpans(const QString &line, const QVector<Span> &spans, int from = 0)
{
    int prevEnd = 0;
    for (int i = from; i < spans.size(); ++i) {
        const Span &s = spans[i];
        if (s.length <= 0)
            return QStringLiteral("empty span #%1").arg(i);
        if (s.start < prevEnd)
            return QStringLiteral("unsorted/overlapping span #%1 (%2 < %3)").arg(i).arg(s.start).arg(prevEnd);
        if (s.start + s.length > line.size())
            return QStringLiteral("span #%1 beyond end (%2+%3 > %4)").arg(i).arg(s.start).arg(s.length).arg(line.size());
        if (int(s.token) <= 0 || int(s.token) >= int(Token::Count))
            return QStringLiteral("span #%1 has bad token %2").arg(i).arg(int(s.token));
        if (s.start > 0 && line[s.start].isLowSurrogate() && line[s.start - 1].isHighSurrogate())
            return QStringLiteral("span #%1 starts inside a surrogate pair").arg(i);
        const int e = s.start + s.length;
        if (e < line.size() && line[e].isLowSurrogate() && line[e - 1].isHighSurrogate())
            return QStringLiteral("span #%1 ends inside a surrogate pair").arg(i);
        prevEnd = e;
    }
    return {};
}

int fullState(const Language *lang, const QStringList &lines, QVector<int> *outs = nullptr)
{
    int st = 0;
    if (outs)
        outs->clear();
    for (const QString &ln : lines) {
        QVector<Span> sp;
        st = highlightLine(lang, ln, st, sp);
        if (outs)
            outs->append(st);
    }
    return st;
}

// Emulates the editor protocol: re-highlight the edited line, then follow-on
// lines only while the incoming state differs from what was used before.
// Returns the number of lines re-highlighted.
int rehighlightFrom(Block &b, int first)
{
    int count = 0;
    for (int j = first; j < b.lines.size(); ++j) {
        const int stIn = j == 0 ? 0 : b.out[j - 1];
        if (j > first && stIn == b.in[j])
            break;
        QVector<Span> sp;
        const int o = highlightLine(b.lang, b.lines[j], stIn, sp);
        b.in[j] = stIn;
        b.out[j] = o;
        b.spans[j] = sp;
        ++count;
    }
    return count;
}

bool sameSpans(const QVector<Span> &a, const QVector<Span> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].length != b[i].length || a[i].token != b[i].token)
            return false;
    return true;
}

const QStringList kAllLanguages = {QStringLiteral("bash"),   QStringLiteral("c"),      QStringLiteral("cpp"),
                                   QStringLiteral("csharp"), QStringLiteral("css"),    QStringLiteral("diff"),
                                   QStringLiteral("go"),     QStringLiteral("html"),   QStringLiteral("java"),
                                   QStringLiteral("javascript"), QStringLiteral("json"), QStringLiteral("lua"),
                                   QStringLiteral("python"), QStringLiteral("rust"),   QStringLiteral("sql"),
                                   QStringLiteral("swift"),  QStringLiteral("toml"),   QStringLiteral("typescript"),
                                   QStringLiteral("yaml")};

// Line fragments that open / close / contain the multi-line constructs of each
// language. Used for random documents (stabilization) and fuzzing.
QStringList pool(const QString &lang)
{
    QStringList p = {QString(), QStringLiteral("x = 1;"), QStringLiteral("  foo(bar, 42)"), QStringLiteral("}")};
    auto add = [&](std::initializer_list<const char *> l) {
        for (const char *s : l)
            p.append(QString::fromUtf8(s));
    };
    if (lang == QLatin1String("c") || lang == QLatin1String("cpp")) {
        add({"#include <stdio.h>", "#define M(x) \\", "  ((x) + 1)", "/* start", "end */", "// line \\", "int x = 0x1F + 1.5e3;",
             "char *s = \"a\\\"b\";", "char c = '\\'';", "\"unterminated", "str = \"cont \\", "ok\";",
             "if (a) { b(); } else c;", "a /* inline */ b", "auto r = R\"x(raw", "more )x\" ;", "R\"(a)\"",
             "template<class T> struct A {};", "u8\"s\" L'c'"});
    } else if (lang == QLatin1String("csharp")) {
        add({"string v = @\"line", "two\"\"q\"; ", "var r = \"\"\"", "raw\"\"\";", "#region X", "/* c", "*/",
             "[Attr] public class A {}", "var s = $\"{a}\";"});
    } else if (lang == QLatin1String("java")) {
        add({"String t = \"\"\"", "  text \\", "\"\"\";", "@Override public void f() {}", "/** doc", " */", "char c = '\\n';"});
    } else if (lang == QLatin1String("go")) {
        add({"s := `raw", "text`", "x := 'a'", "func (r *T) M() error {", "/* c", "*/", "// c"});
    } else if (lang == QLatin1String("rust")) {
        add({"let s = r#\"a", "b\"#;", "/* a /* b */", "c */", "let c = 'x'; let l: &'a str;", "#[derive(Debug)]",
             "println!(\"{}\", x);", "let s = \"multi", "line\";", "r##\"x\"# y\"##", "let b = br\"raw\";"});
    } else if (lang == QLatin1String("swift")) {
        add({"let m = \"\"\"", "text\"\"\"", "let r = #\"\"\"", "\"\"\"#", "/* a /* b */", "*/", "@available(iOS 1, *)", "#if DEBUG",
             "let q = #\"raw\"#"});
    } else if (lang == QLatin1String("lua")) {
        add({"local s = [==[", "]==] x", "--[[ c", "]] y", "--[==[", "]==]", "x = \"a\\", "b\"", "local t = {1, 2}", "-- c"});
    } else if (lang == QLatin1String("python")) {
        add({"s = '''abc", "def'''", "d = \"\"\"", "\"\"\" + x", "@dec.x(1)", "class A(B):", "x = f\"{a}\" + r'\\d'", "t = \"line \\",
             "cont\"", "# c", "match x:"});
    } else if (lang == QLatin1String("javascript") || lang == QLatin1String("typescript")) {
        add({"const t = `a ${b} c", "line ${ {x: 1}.x } `;", "x = `${`${a}`}`;", "/* c", "*/", "var re = /a[/]b/g;",
             "y = a / b / c;", "// c", "class A extends B {}", "t = `", "`", "type A = { b: string };", "x = `a${"});
    } else if (lang == QLatin1String("sql")) {
        add({"SELECT * FROM t -- c", "/* c", "*/", "x = 'multi", "line';", "$$ body", "more $$;", "$f$ a $f$",
             "WHERE a = :b AND c = $1"});
    } else if (lang == QLatin1String("bash")) {
        add({"echo \"multi", "line\" done", "cat <<EOF", "text $x", "EOF", "cat <<-'X'", "\tX", "x=$'a'", "echo 'a", "b'", "foo \\",
             "bar", "if [[ -f x ]]; then echo $(date); fi", "# c", "echo ${a:-b} $((1 << 2))"});
    } else if (lang == QLatin1String("css")) {
        add({"a { color: red; }", "@media (min-width: 1px) {", "}", "/* c", "*/", "b {", "  margin: 0 auto;", "  transition: a 1s,",
             "    b 2s;", "x { y: url(a.png) }"});
    } else if (lang == QLatin1String("html")) {
        add({"<div class=\"a\">", "<!-- c", "-->", "<script>", "var x = `a", "b`;", "</script>", "<style>", "a { color: red; /* c", "*/ }",
             "</style>", "<a href='x", "y'>text</a>", "<![CDATA[", "]]>", "<p>&amp; text</p>", "<img", "src=\"a\" />", "<?xml?>"});
    } else if (lang == QLatin1String("yaml")) {
        add({"key: value", "list:", "  - a", "  - b: |", "      literal", "", "other: >-", "    folded", "x: [1, 2", "# c", "a: &x 1", "b: *x"});
    } else if (lang == QLatin1String("toml")) {
        add({"[table]", "a = 1", "s = \"\"\"", "ml", "\"\"\"", "l = '''", "'''", "arr = [", "  1,", "]", "# c", "[[x]]"});
    } else if (lang == QLatin1String("json")) {
        add({"{", "\"a\": 1,", "/* c", "*/", "\"b\": [1, 2],", "}"});
    } else if (lang == QLatin1String("diff")) {
        add({"+added", "-removed", "@@ -1 +1 @@", " ctx", "--- a/x", "+++ b/x", "diff --git a b"});
    }
    return p;
}

char16_t randomUnit(std::mt19937 &rng)
{
    switch (rng() % 8) {
    case 0: return char16_t(0xD800 + rng() % 0x400);          // lone high surrogate
    case 1: return char16_t(0xDC00 + rng() % 0x400);          // lone low surrogate
    case 2: return char16_t(rng() % 0x20);                    // control chars incl. NUL
    case 3: return char16_t(0x80 + rng() % 0x1000);           // BMP non-ASCII
    case 4: return char16_t(rng() & 0xFFFF);                  // anything
    default: return char16_t(0x20 + rng() % 0x5F);            // printable ASCII
    }
}

QString randomLine(std::mt19937 &rng, const QStringList &p)
{
    QString s;
    const int parts = 1 + int(rng() % 5);
    for (int i = 0; i < parts; ++i) {
        switch (rng() % 4) {
        case 0:
            s += p[int(rng() % p.size())];
            break;
        case 1:
            s += p[int(rng() % p.size())];
            s += QLatin1Char(' ');
            break;
        case 2: {
            const int n = int(rng() % 6);
            for (int k = 0; k < n; ++k)
                s += QChar(randomUnit(rng));
            break;
        }
        default: {
            // valid emoji / CJK
            static const char16_t emoji[] = {0xD83D, 0xDE00, 0x4E2D, 0x6587, 0x0301, 0xD83D, 0xDC4D, 0xD83C, 0xDFFD};
            const int k = int(rng() % 4);
            for (int m = 0; m < k; ++m) {
                const int idx = int(rng() % 4) * 2;
                s += QChar(emoji[idx]);
                if (emoji[idx] >= 0xD800 && emoji[idx] < 0xDC00)
                    s += QChar(emoji[idx + 1]);
            }
            break;
        }
        }
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
class CodeTests : public QObject {
    Q_OBJECT

private slots:
    // --- registry ---------------------------------------------------------
    void lazyLoading();
    void aliases_data();
    void aliases();
    void unknownIsNull();
    void supportedAndCanonical();
    void threadSafeLazyInit();

    // --- per language token expectations ----------------------------------
    void lang_c();
    void lang_cpp();
    void lang_csharp();
    void lang_java();
    void lang_go();
    void lang_rust();
    void lang_swift();
    void lang_lua();
    void lang_python();
    void lang_javascript();
    void lang_typescript();
    void lang_sql();
    void lang_bash();
    void lang_css();
    void lang_html();
    void lang_json();
    void lang_yaml();
    void lang_toml();
    void lang_diff();

    // --- multi-line state -------------------------------------------------
    void state_cBlockComment();
    void state_pythonTriple();
    void state_jsTemplates();
    void state_rustRawAndNested();
    void state_luaLong();
    void state_cppRaw();
    void state_swiftMultiline();
    void state_goRaw();
    void state_htmlSpanning();
    void state_sqlBlock();
    void state_bashHeredoc();
    void state_misc();
    void stabilization_converges();
    void stabilization_incrementalEqualsFull_data();
    void stabilization_incrementalEqualsFull();
    void statesAreCanonical();

    // --- robustness -------------------------------------------------------
    void contract_appendAndNullLang();
    void contract_noAllocation();
    void unicode_safety_data();
    void unicode_safety();
    void fuzz_data();
    void fuzz();
    void hugeLines_data();
    void hugeLines();
    void parallelHighlighting();

    // --- performance ------------------------------------------------------
    void benchmark();
};

// ===========================================================================
// registry
// ===========================================================================
void CodeTests::lazyLoading()
{
    QCOMPARE(loadedLanguageCount(), 0); // nothing is built at startup
    QCOMPARE(supportedLanguages().size(), 19);
    QCOMPARE(loadedLanguageCount(), 0); // listing does not load
    QVERIFY(canonicalName(nullptr).isEmpty());
    QVERIFY(languageFor(u"nope") == nullptr);
    QVERIFY(languageFor(u"") == nullptr);
    QCOMPARE(loadedLanguageCount(), 0); // unknown names do not load

    // Cold-start race: many threads ask for not-yet-built languages (by name and
    // by alias) at the same moment. Every language must be built exactly once and
    // every thread must see the same object.
    {
        constexpr int kThreads = 16;
        const QStringList names = {QStringLiteral("go"), QStringLiteral("golang"), QStringLiteral("lua"), QStringLiteral("LUA"),
                                   QStringLiteral("swift")};
        std::vector<std::vector<const Language *>> seen(kThreads, std::vector<const Language *>(size_t(names.size()), nullptr));
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t)
            th.emplace_back([&, t] {
                ready.fetch_add(1);
                while (!go.load())
                    std::this_thread::yield();
                for (int k = 0; k < names.size(); ++k)
                    seen[size_t(t)][size_t((k + t) % names.size())] = languageFor(names[(k + t) % names.size()]);
            });
        while (ready.load() < kThreads)
            std::this_thread::yield();
        go = true;
        for (auto &t : th)
            t.join();
        QCOMPARE(loadedLanguageCount(), 3); // go, lua, swift: once each
        for (int t = 1; t < kThreads; ++t)
            QVERIFY(seen[size_t(t)] == seen[0]);
        QVERIFY(seen[0][0] == seen[0][1]); // go == golang
        QVERIFY(seen[0][2] == seen[0][3]); // lua == LUA
        QVERIFY(seen[0][0] != seen[0][2]);
        QVERIFY(seen[0][0] && seen[0][2] && seen[0][4]);
    }
    const int base = loadedLanguageCount();
    QCOMPARE(base, 3);

    const Language *py = languageFor(u"python");
    QVERIFY(py);
    QCOMPARE(loadedLanguageCount(), base + 1);
    QVERIFY(languageFor(u"py") == py); // alias, same object, no new load
    QVERIFY(languageFor(u"PYTHON") == py);
    QVERIFY(languageFor(u"python3") == py);
    QCOMPARE(loadedLanguageCount(), base + 1);

    const Language *rs = languageFor(u"rust");
    QVERIFY(rs);
    QVERIFY(rs != py);
    QCOMPARE(loadedLanguageCount(), base + 2);
    QVERIFY(languageFor(u"rs") == rs);
    QCOMPARE(loadedLanguageCount(), base + 2);

    // highlighting an already loaded language never loads another one
    QVector<Span> sp;
    highlightLine(py, u"print('x')", 0, sp);
    QCOMPARE(loadedLanguageCount(), base + 2);

    // plain html does not load the embedded languages; <script> / <style> do
    const Language *html = languageFor(u"html");
    QCOMPARE(loadedLanguageCount(), base + 3);
    sp.clear();
    highlightLine(html, u"<p class=\"a\">text &amp; more</p>", 0, sp);
    QCOMPARE(loadedLanguageCount(), base + 3);
    sp.clear();
    int st = highlightLine(html, u"<style>", 0, sp);
    highlightLine(html, u"a { color: red }", st, sp);
    QCOMPARE(loadedLanguageCount(), base + 4); // css, on demand
    QVERIFY(languageFor(u"css"));
    QCOMPARE(loadedLanguageCount(), base + 4);
}

void CodeTests::aliases_data()
{
    QTest::addColumn<QString>("alias");
    QTest::addColumn<QString>("canonical");
    const char *rows[][2] = {
        {"bash", "bash"}, {"sh", "bash"}, {"zsh", "bash"}, {"shell", "bash"}, {"console", "bash"}, {"c", "c"}, {"h", "c"},
        {"cpp", "cpp"}, {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"hpp", "cpp"}, {"csharp", "csharp"}, {"cs", "csharp"},
        {"c#", "csharp"}, {"css", "css"}, {"diff", "diff"}, {"patch", "diff"}, {"go", "go"}, {"golang", "go"}, {"html", "html"},
        {"htm", "html"}, {"xml", "html"}, {"svg", "html"}, {"java", "java"}, {"javascript", "javascript"}, {"js", "javascript"},
        {"jsx", "javascript"}, {"mjs", "javascript"}, {"json", "json"}, {"jsonc", "json"}, {"lua", "lua"}, {"python", "python"},
        {"py", "python"}, {"rust", "rust"}, {"rs", "rust"}, {"sql", "sql"}, {"swift", "swift"}, {"toml", "toml"},
        {"typescript", "typescript"}, {"ts", "typescript"}, {"tsx", "typescript"}, {"yaml", "yaml"}, {"yml", "yaml"},
        {"Python", "python"}, {"PY", "python"}, {"C++", "cpp"}, {"JavaScript", "javascript"}, {"  rust  ", "rust"},
        {"TSX", "typescript"}, {"Yml", "yaml"}, {"XML", "html"}, {"C#", "csharp"}, {"Sh", "bash"},
    };
    for (const auto &r : rows)
        QTest::newRow(r[0]) << QString::fromLatin1(r[0]) << QString::fromLatin1(r[1]);
}

void CodeTests::aliases()
{
    QFETCH(QString, alias);
    QFETCH(QString, canonical);
    const Language *l = languageFor(alias);
    QVERIFY2(l, qPrintable(alias));
    QCOMPARE(canonicalName(l), canonical);
    QVERIFY(languageFor(canonical) == l);
}

void CodeTests::unknownIsNull()
{
    for (const char *n : {"", " ", "nope", "pythonx", "c--", "ruby", "perl", "markdown", "text", "plaintext", "kotlin", "py thon",
                          "averyveryverylongunknownlanguagename"})
        QVERIFY2(languageFor(QString::fromLatin1(n)) == nullptr, n);
    QVERIFY(languageFor(QString::fromUtf8("pyth\xC3\xB6n")) == nullptr);
    QVERIFY(languageFor(QStringView()) == nullptr);
    // a null language produces no spans and state 0
    QVector<Span> sp;
    QCOMPARE(highlightLine(nullptr, u"anything /* x", 12345, sp), 0);
    QVERIFY(sp.isEmpty());
}

void CodeTests::supportedAndCanonical()
{
    const QStringList names = supportedLanguages();
    QStringList sorted = names;
    sorted.sort();
    QCOMPARE(names, sorted);
    const QStringList expect = {"bash", "c", "cpp", "csharp", "css", "diff", "go", "html", "java", "javascript",
                                "json", "lua", "python", "rust", "sql", "swift", "toml", "typescript", "yaml"};
    QCOMPARE(names, expect);
    for (const QString &n : names) {
        const Language *l = languageFor(n);
        QVERIFY2(l, qPrintable(n));
        QCOMPARE(canonicalName(l), n);
    }
    QCOMPARE(loadedLanguageCount(), 19);
}

void CodeTests::threadSafeLazyInit()
{
    // Everything is loaded by now (supportedAndCanonical); the cold-start race is
    // part of lazyLoading(). This checks warm concurrent lookups.
    std::vector<const Language *> res(8, nullptr);
    std::vector<std::thread> th;
    std::atomic<bool> go{false};
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            while (!go.load())
                std::this_thread::yield();
            const Language *last = nullptr;
            for (int k = 0; k < 2000; ++k)
                last = languageFor(kAllLanguages[(k + t) % kAllLanguages.size()]);
            res[t] = last;
        });
    go = true;
    for (auto &t : th)
        t.join();
    QCOMPARE(loadedLanguageCount(), 19);
    for (int t = 0; t < 8; ++t)
        QVERIFY(res[t] == languageFor(kAllLanguages[(1999 + t) % kAllLanguages.size()]));
}

// ===========================================================================
// per language
// ===========================================================================
void CodeTests::lang_c()
{
    const Block b = runBlock("c", QStringLiteral("#include <stdio.h>\n"
                                                 "#define SQ(x) ((x)*(x))\n"
                                                 "int main(void) { return 0x1F + 077 + 1.5e-3f; }\n"
                                                 "char *s = \"hi\\n\"; /* c */ // t\n"
                                                 "struct Node { unsigned long n; } *p = NULL;"));
    EXPECT_TOK(b, 0, "#include", Token::Preprocessor);
    EXPECT_TOK(b, 0, "<stdio.h>", Token::String);
    EXPECT_TOK(b, 1, "#define", Token::Preprocessor);
    EXPECT_TOK(b, 1, "SQ", Token::Function);
    EXPECT_TOK(b, 2, "int", Token::Type);
    EXPECT_TOK(b, 2, "main", Token::Function);
    EXPECT_TOK(b, 2, "void", Token::Type);
    EXPECT_TOK(b, 2, "return", Token::Keyword);
    EXPECT_TOK(b, 2, "0x1F", Token::Number);
    EXPECT_TOK(b, 2, "077", Token::Number);
    EXPECT_TOK(b, 2, "1.5e-3f", Token::Number);
    EXPECT_TOK(b, 2, "+", Token::Operator);
    EXPECT_TOK(b, 2, "{", Token::Punctuation);
    EXPECT_TOK(b, 3, "\"hi\\n\"", Token::String);
    EXPECT_TOK(b, 3, "/* c */", Token::Comment);
    EXPECT_TOK(b, 3, "// t", Token::Comment);
    EXPECT_TOK(b, 4, "struct", Token::Keyword);
    EXPECT_TOK(b, 4, "Node", Token::Type);
    EXPECT_TOK(b, 4, "unsigned", Token::Type);
    EXPECT_TOK(b, 4, "NULL", Token::Constant);
    // line continuation of a preprocessor line and of a // comment
    const Block c = runBlock("c", QStringLiteral("#define A \\\n  1 + 2\n// c \\\nstill comment\nint x;"));
    QVERIFY(c.out[0] != 0);
    QCOMPARE(c.out[1], 0);
    EXPECT_TOK(c, 1, "1", Token::Number);
    QVERIFY(c.out[2] != 0);
    EXPECT_TOK(c, 3, "still comment", Token::Comment);
    EXPECT_TOK(c, 4, "int", Token::Type);
}

void CodeTests::lang_cpp()
{
    const Block b = runBlock("cpp", QStringLiteral("template <typename T> class Foo : public Base<T> {\n"
                                                   "  std::vector<int> v; auto n = 1'000'000 + 0b101;\n"
                                                   "  void run() const noexcept override { throw std::runtime_error(\"x\"); }\n"
                                                   "  bool ok = nullptr == p; char c = u8'a'; auto w = L\"wide\";\n"
                                                   "};"));
    EXPECT_TOK(b, 0, "template", Token::Keyword);
    EXPECT_TOK(b, 0, "typename", Token::Keyword);
    EXPECT_TOK(b, 0, "class", Token::Keyword);
    EXPECT_TOK(b, 0, "Foo", Token::Type);
    EXPECT_TOK(b, 0, "public", Token::Keyword);
    EXPECT_TOK(b, 0, "Base", Token::Type);
    EXPECT_TOK(b, 1, "std", Token::Builtin);
    EXPECT_TOK(b, 1, "vector", Token::Type);
    EXPECT_TOK(b, 1, "int", Token::Type);
    EXPECT_TOK(b, 1, "1'000'000", Token::Number);
    EXPECT_TOK(b, 1, "0b101", Token::Number);
    EXPECT_TOK(b, 2, "run", Token::Function);
    EXPECT_TOK(b, 2, "const", Token::Keyword);
    EXPECT_TOK(b, 2, "noexcept", Token::Keyword);
    EXPECT_TOK(b, 2, "override", Token::Keyword);
    EXPECT_TOK(b, 2, "throw", Token::Keyword);
    EXPECT_TOK(b, 2, "runtime_error", Token::Function);
    EXPECT_TOK(b, 2, "\"x\"", Token::String);
    EXPECT_TOK(b, 3, "nullptr", Token::Constant);
    EXPECT_TOK(b, 3, "u8'a'", Token::String);
    EXPECT_TOK(b, 3, "L\"wide\"", Token::String);
    const Block at = runBlock("cpp", QStringLiteral("[[nodiscard]] int f(int a[2][3]); auto g = v[w[0]];"));
    EXPECT_TOK(at, 0, "[[nodiscard]]", Token::Attribute);
    EXPECT_TOK(at, 0, "int", Token::Type);
    EXPECT_TOK(at, 0, "f", Token::Function);
    // member names that collide with keywords are not keywords
    const Block m = runBlock("cpp", QStringLiteral("obj.delete(1); p->new_node(); x.class_id;"));
    EXPECT_TOK(m, 0, "delete", Token::Function);
}

void CodeTests::lang_csharp()
{
    const Block b = runBlock("csharp", QStringLiteral("#region Core\n"
                                                      "using System.Linq;\n"
                                                      "public sealed class Foo : IDisposable {\n"
                                                      "  string s = $\"a {b}\"; string v = @\"c:\\dir\"; int n = 0x1_0 + 3m;\n"
                                                      "  async Task<int> Run() => await Task.FromResult(1);\n"
                                                      "}"));
    EXPECT_TOK(b, 0, "#region", Token::Preprocessor);
    EXPECT_TOK(b, 1, "using", Token::Keyword);
    EXPECT_TOK(b, 2, "public", Token::Keyword);
    EXPECT_TOK(b, 2, "class", Token::Keyword);
    EXPECT_TOK(b, 2, "Foo", Token::Type);
    EXPECT_TOK(b, 2, "IDisposable", Token::Type);
    EXPECT_TOK(b, 3, "string", Token::Type);
    EXPECT_TOK(b, 3, "$\"a {b}\"", Token::String);
    EXPECT_TOK(b, 3, "@\"c:\\dir\"", Token::String);
    EXPECT_TOK(b, 3, "0x1_0", Token::Number);
    EXPECT_TOK(b, 3, "3m", Token::Number);
    EXPECT_TOK(b, 4, "async", Token::Keyword);
    EXPECT_TOK(b, 4, "await", Token::Keyword);
    EXPECT_TOK(b, 4, "Run", Token::Function);
    EXPECT_TOK(b, 4, "FromResult", Token::Function);
}

void CodeTests::lang_java()
{
    const Block b = runBlock("java", QStringLiteral("package a.b;\n"
                                                    "@Override\n"
                                                    "public static void main(String[] args) throws IOException {\n"
                                                    "  int x = 0xFFL + 1_000; boolean t = true; Object o = null; char c = '\\n';\n"
                                                    "  System.out.println(\"hi\"); // end\n"
                                                    "}"));
    EXPECT_TOK(b, 0, "package", Token::Keyword);
    EXPECT_TOK(b, 1, "@Override", Token::Attribute);
    EXPECT_TOK(b, 2, "public", Token::Keyword);
    EXPECT_TOK(b, 2, "static", Token::Keyword);
    EXPECT_TOK(b, 2, "void", Token::Type);
    EXPECT_TOK(b, 2, "main", Token::Function);
    EXPECT_TOK(b, 2, "String", Token::Type);
    EXPECT_TOK(b, 2, "throws", Token::Keyword);
    EXPECT_TOK(b, 3, "int", Token::Type);
    EXPECT_TOK(b, 3, "0xFFL", Token::Number);
    EXPECT_TOK(b, 3, "1_000", Token::Number);
    EXPECT_TOK(b, 3, "true", Token::Constant);
    EXPECT_TOK(b, 3, "null", Token::Constant);
    EXPECT_TOK(b, 3, "'\\n'", Token::String);
    EXPECT_TOK(b, 4, "System", Token::Builtin);
    EXPECT_TOK(b, 4, "println", Token::Function);
    EXPECT_TOK(b, 4, "// end", Token::Comment);
}

void CodeTests::lang_go()
{
    const Block b = runBlock("go", QStringLiteral("package main\n"
                                                  "type Server struct { Addr string }\n"
                                                  "func (s *Server) Handle(w http.ResponseWriter) error {\n"
                                                  "\tx := []int{1, 0b11, 0x1p-2, 1_000}; r := 'a'\n"
                                                  "\tfmt.Println(len(x), nil, \"s\\t\")\n"
                                                  "}"));
    EXPECT_TOK(b, 0, "package", Token::Keyword);
    EXPECT_TOK(b, 1, "type", Token::Keyword);
    EXPECT_TOK(b, 1, "Server", Token::Type);
    EXPECT_TOK(b, 1, "struct", Token::Keyword);
    EXPECT_TOK(b, 1, "string", Token::Type);
    EXPECT_TOK(b, 2, "func", Token::Keyword);
    EXPECT_TOK(b, 2, "Handle", Token::Function);
    EXPECT_TOK(b, 2, "error", Token::Type);
    EXPECT_TOK(b, 3, "0b11", Token::Number);
    EXPECT_TOK(b, 3, "0x1p-2", Token::Number);
    EXPECT_TOK(b, 3, "'a'", Token::String);
    EXPECT_TOK(b, 4, "Println", Token::Function);
    EXPECT_TOK(b, 4, "len", Token::Builtin);
    EXPECT_TOK(b, 4, "nil", Token::Constant);
    EXPECT_TOK(b, 4, "\"s\\t\"", Token::String);
}

void CodeTests::lang_rust()
{
    const Block b = runBlock("rust", QStringLiteral("#[derive(Debug, Clone)]\n"
                                                    "pub struct Foo<'a> { x: &'a str, y: u8 }\n"
                                                    "fn main() -> Option<i32> {\n"
                                                    "    let c = 'x'; let n = b'\\n'; let s: &'static str = \"hi\";\n"
                                                    "    println!(\"{}\", 0xFFu8 + 1_000i64 + 2.5e3);\n"
                                                    "    let v = vec![1, 2]; None\n"
                                                    "}"));
    EXPECT_TOK(b, 0, "#[derive(Debug, Clone)]", Token::Attribute);
    EXPECT_TOK(b, 1, "pub", Token::Keyword);
    EXPECT_TOK(b, 1, "struct", Token::Keyword);
    EXPECT_TOK(b, 1, "Foo", Token::Type);
    EXPECT_TOK(b, 1, "'a", Token::Type);
    EXPECT_TOK_N(b, 1, "str", 1, Token::Type); // 0 is inside "struct"
    EXPECT_TOK(b, 1, "u8", Token::Type);
    EXPECT_TOK(b, 2, "fn", Token::Keyword);
    EXPECT_TOK(b, 2, "main", Token::Function);
    EXPECT_TOK(b, 2, "Option", Token::Type);
    EXPECT_TOK(b, 3, "let", Token::Keyword);
    EXPECT_TOK(b, 3, "'x'", Token::String);
    EXPECT_TOK(b, 3, "b'\\n'", Token::String);
    EXPECT_TOK(b, 3, "'static", Token::Type);
    EXPECT_TOK(b, 3, "\"hi\"", Token::String);
    EXPECT_TOK(b, 4, "println!", Token::Function);
    EXPECT_TOK(b, 4, "0xFFu8", Token::Number);
    EXPECT_TOK(b, 4, "1_000i64", Token::Number);
    EXPECT_TOK(b, 4, "2.5e3", Token::Number);
    EXPECT_TOK(b, 5, "vec!", Token::Function);
    EXPECT_TOK(b, 5, "None", Token::Constant);
}

void CodeTests::lang_swift()
{
    const Block b = runBlock("swift", QStringLiteral("@available(iOS 13, *)\n"
                                                     "struct Foo: Codable { let x: Int = 0b101; var y = \"a\\(x)b\" }\n"
                                                     "#if os(macOS)\n"
                                                     "func f() -> Int? { $0 + 1 }\n"
                                                     "let ok = true && x != nil // c"));
    EXPECT_TOK(b, 0, "@available", Token::Attribute);
    EXPECT_TOK(b, 1, "struct", Token::Keyword);
    EXPECT_TOK(b, 1, "Foo", Token::Type);
    EXPECT_TOK(b, 1, "Codable", Token::Type);
    EXPECT_TOK(b, 1, "let", Token::Keyword);
    EXPECT_TOK(b, 1, "Int", Token::Type);
    EXPECT_TOK(b, 1, "0b101", Token::Number);
    EXPECT_TOK(b, 1, "var", Token::Keyword);
    EXPECT_TOK(b, 1, "\"a\\(x)b\"", Token::String);
    EXPECT_TOK(b, 2, "#if", Token::Preprocessor);
    EXPECT_TOK(b, 3, "func", Token::Keyword);
    EXPECT_TOK_N(b, 3, "f", 1, Token::Function);
    EXPECT_TOK(b, 3, "$0", Token::Variable);
    EXPECT_TOK(b, 4, "true", Token::Constant);
    EXPECT_TOK(b, 4, "nil", Token::Constant);
    EXPECT_TOK(b, 4, "// c", Token::Comment);
}

void CodeTests::lang_lua()
{
    const Block b = runBlock("lua", QStringLiteral("local t = {1, 0xA.8p0, 3e2, n = #arr}\n"
                                                   "function M.foo(a, ...) return a ~= nil and 'x' .. \"y\" end\n"
                                                   "-- comment\n"
                                                   "for i, v in ipairs(t) do print(i) end"));
    EXPECT_TOK(b, 0, "local", Token::Keyword);
    EXPECT_TOK(b, 0, "1", Token::Number);
    EXPECT_TOK(b, 0, "0xA.8p0", Token::Number);
    EXPECT_TOK(b, 0, "3e2", Token::Number);
    EXPECT_TOK(b, 0, "#", Token::Operator); // length operator, not a comment
    EXPECT_TOK(b, 1, "function", Token::Keyword);
    EXPECT_TOK(b, 1, "foo", Token::Function);
    EXPECT_TOK(b, 1, "return", Token::Keyword);
    EXPECT_TOK(b, 1, "~=", Token::Operator);
    EXPECT_TOK(b, 1, "nil", Token::Constant);
    EXPECT_TOK(b, 1, "'x'", Token::String);
    EXPECT_TOK(b, 1, "..", Token::Punctuation);
    EXPECT_TOK(b, 1, "\"y\"", Token::String);
    EXPECT_TOK(b, 2, "-- comment", Token::Comment);
    EXPECT_TOK(b, 3, "ipairs", Token::Builtin);
    EXPECT_TOK(b, 3, "print", Token::Builtin);
    EXPECT_TOK(b, 3, "do", Token::Keyword);
}

void CodeTests::lang_python()
{
    const Block b = runBlock("python", QStringLiteral("@dataclass(frozen=True)\n"
                                                      "class Point(Base):\n"
                                                      "    def run(self, n: int = 0x_FF) -> None:\n"
                                                      "        s = f'{n}' + r\"\\d+\" + b'\\x00'  # c\n"
                                                      "        return Foo(1_000 + 1e-3 + 2j) if n is not None else MAX_SIZE"));
    EXPECT_TOK(b, 0, "@dataclass", Token::Attribute);
    EXPECT_TOK(b, 0, "True", Token::Constant);
    EXPECT_TOK(b, 1, "class", Token::Keyword);
    EXPECT_TOK(b, 1, "Point", Token::Type);
    EXPECT_TOK(b, 1, "Base", Token::Type);
    EXPECT_TOK(b, 2, "def", Token::Keyword);
    EXPECT_TOK(b, 2, "run", Token::Function);
    EXPECT_TOK(b, 2, "self", Token::Builtin);
    EXPECT_TOK(b, 2, "int", Token::Type);
    EXPECT_TOK(b, 2, "0x_FF", Token::Number);
    EXPECT_TOK(b, 2, "None", Token::Constant);
    EXPECT_TOK(b, 3, "f'{n}'", Token::String);
    EXPECT_TOK(b, 3, "r\"\\d+\"", Token::String);
    EXPECT_TOK(b, 3, "b'\\x00'", Token::String);
    EXPECT_TOK(b, 3, "# c", Token::Comment);
    EXPECT_TOK(b, 4, "return", Token::Keyword);
    EXPECT_TOK(b, 4, "Foo", Token::Function);
    EXPECT_TOK(b, 4, "1_000", Token::Number);
    EXPECT_TOK(b, 4, "1e-3", Token::Number);
    EXPECT_TOK(b, 4, "2j", Token::Number);
    EXPECT_TOK(b, 4, "is", Token::Keyword);
    EXPECT_TOK(b, 4, "MAX_SIZE", Token::Constant);
    // soft keywords: only as the head of a line that ends in ':'
    const Block m = runBlock("python", QStringLiteral("match cmd:\n    case [a, b]:\n        pass\nmatch = re.match(r'x', s)"));
    EXPECT_TOK(m, 0, "match", Token::Keyword);
    EXPECT_TOK(m, 1, "case", Token::Keyword);
    EXPECT_TOK(m, 3, "match", Token::Plain);
    EXPECT_TOK(m, 3, "match", Token::Plain);
    // a method called like a keyword is a call, not a keyword
    const Block d = runBlock("python", QStringLiteral("x.print(1); obj.class_()"));
    EXPECT_TOK(d, 0, "print", Token::Function);
}

void CodeTests::lang_javascript()
{
    const Block b = runBlock("javascript", QStringLiteral("import { a as b } from './mod.js';\n"
                                                          "const re = /ab+c[/]\\d/gi, d = x / 2 / y;\n"
                                                          "class Foo extends Bar { #p = 1; async get(x) { return this.#p / 2; } }\n"
                                                          "const o = { type: 'x', of: 1 }; // done\n"
                                                          "let u = a?.b ?? c; const big = 10n + 0o17 + .5e3;\n"
                                                          "@Component({selector: 'x'}) console.log(`t`, undefined);"));
    EXPECT_TOK(b, 0, "import", Token::Keyword);
    EXPECT_TOK(b, 0, "as", Token::Keyword);
    EXPECT_TOK(b, 0, "from", Token::Keyword);
    EXPECT_TOK(b, 0, "'./mod.js'", Token::String);
    EXPECT_TOK(b, 1, "const", Token::Keyword);
    EXPECT_TOK(b, 1, "/ab+c[/]\\d/gi", Token::String); // regex literal
    EXPECT_TOK_N(b, 1, "/", 3, Token::Operator);       // division after an identifier
    EXPECT_TOK(b, 2, "class", Token::Keyword);
    EXPECT_TOK(b, 2, "Foo", Token::Type);
    EXPECT_TOK(b, 2, "Bar", Token::Type);
    EXPECT_TOK(b, 2, "async", Token::Keyword);
    EXPECT_TOK(b, 2, "get", Token::Function);
    EXPECT_TOK(b, 2, "this", Token::Keyword);
    EXPECT_TOK(b, 3, "type", Token::Plain); // object key, not the TS keyword
    EXPECT_TOK(b, 3, "of", Token::Plain);
    EXPECT_TOK(b, 3, "'x'", Token::String);
    EXPECT_TOK(b, 3, "// done", Token::Comment);
    EXPECT_TOK(b, 4, "10n", Token::Number);
    EXPECT_TOK(b, 4, "0o17", Token::Number);
    EXPECT_TOK(b, 4, ".5e3", Token::Number);
    EXPECT_TOK(b, 5, "@Component", Token::Attribute);
    EXPECT_TOK(b, 5, "console", Token::Builtin);
    EXPECT_TOK(b, 5, "log", Token::Function);
    EXPECT_TOK(b, 5, "`t`", Token::String);
    EXPECT_TOK(b, 5, "undefined", Token::Constant);
}

void CodeTests::lang_typescript()
{
    const Block b = runBlock("typescript", QStringLiteral("type Foo = { type: string };\n"
                                                          "interface Bar<T> extends Baz { readonly x: number; }\n"
                                                          "declare module \"x\";\n"
                                                          "let v = y as unknown as string;\n"
                                                          "enum Color { Red = 1 }"));
    EXPECT_TOK(b, 0, "type", Token::Keyword);
    EXPECT_TOK(b, 0, "Foo", Token::Type);
    EXPECT_TOK_N(b, 0, "type", 1, Token::Plain);
    EXPECT_TOK(b, 0, "string", Token::Type);
    EXPECT_TOK(b, 1, "interface", Token::Keyword);
    EXPECT_TOK(b, 1, "Bar", Token::Type);
    EXPECT_TOK(b, 1, "extends", Token::Keyword);
    EXPECT_TOK(b, 1, "Baz", Token::Type);
    EXPECT_TOK(b, 1, "readonly", Token::Keyword);
    EXPECT_TOK(b, 1, "number", Token::Type);
    EXPECT_TOK(b, 2, "declare", Token::Keyword);
    EXPECT_TOK(b, 2, "module", Token::Keyword);
    EXPECT_TOK(b, 3, "as", Token::Keyword);
    EXPECT_TOK(b, 3, "unknown", Token::Type);
    EXPECT_TOK(b, 4, "enum", Token::Keyword);
    EXPECT_TOK(b, 4, "Color", Token::Type);
}

void CodeTests::lang_sql()
{
    const Block b = runBlock("sql", QStringLiteral("SELECT u.id, COUNT(*) AS n, 'it''s', \"col\", `x` FROM users u -- c\n"
                                                   "where u.age >= 18 and name like '%a' /* b */ ;\n"
                                                   "INSERT INTO t VALUES (1, 2.5e3, :name, $1, @v, NULL);\n"
                                                   "create table Foo (id serial primary key, x numeric(10,2) default 1);"));
    EXPECT_TOK(b, 0, "SELECT", Token::Keyword);
    EXPECT_TOK(b, 0, "COUNT", Token::Builtin);
    EXPECT_TOK(b, 0, "AS", Token::Keyword);
    EXPECT_TOK(b, 0, "'it''s'", Token::String);
    EXPECT_TOK(b, 0, "\"col\"", Token::String);
    EXPECT_TOK(b, 0, "`x`", Token::Variable);
    EXPECT_TOK(b, 0, "FROM", Token::Keyword);
    EXPECT_TOK(b, 0, "-- c", Token::Comment);
    EXPECT_TOK(b, 1, "where", Token::Keyword); // case-insensitive
    EXPECT_TOK(b, 1, "and", Token::Keyword);
    EXPECT_TOK(b, 1, "like", Token::Keyword);
    EXPECT_TOK(b, 1, "'%a'", Token::String);
    EXPECT_TOK(b, 1, "/* b */", Token::Comment);
    EXPECT_TOK(b, 2, "INSERT", Token::Keyword);
    EXPECT_TOK(b, 2, "2.5e3", Token::Number);
    EXPECT_TOK(b, 2, ":name", Token::Variable);
    EXPECT_TOK(b, 2, "$1", Token::Variable);
    EXPECT_TOK(b, 2, "@v", Token::Variable);
    EXPECT_TOK(b, 2, "NULL", Token::Constant);
    EXPECT_TOK(b, 3, "create", Token::Keyword);
    EXPECT_TOK(b, 3, "table", Token::Keyword);
    EXPECT_TOK(b, 3, "Foo", Token::Type);
    EXPECT_TOK(b, 3, "serial", Token::Type);
    EXPECT_TOK(b, 3, "numeric", Token::Type);
}

void CodeTests::lang_bash()
{
    const Block b = runBlock("bash", QStringLiteral("#!/bin/bash\n"
                                                    "NAME=\"World $USER ${HOME:-/tmp}\"; n=$((1 + 2))\n"
                                                    "if [[ -f \"$NAME\" && $# -gt 0 ]]; then echo \"hi\" | grep -v 'x y' > out.txt; fi\n"
                                                    "for f in *.txt; do cat \"$f\"; done # tail\n"
                                                    "function foo() { local a=1; return 0; }\n"
                                                    "ls --color=auto -la ./dir && ./run.sh 42 $(date +%s)"));
    EXPECT_TOK(b, 0, "#!/bin/bash", Token::Comment);
    EXPECT_TOK(b, 1, "NAME", Token::Variable);
    EXPECT_TOK(b, 1, "\"World ", Token::String);
    EXPECT_TOK(b, 1, "$USER", Token::Variable);
    EXPECT_TOK(b, 1, "${HOME:-/tmp}", Token::Variable);
    EXPECT_TOK(b, 2, "if", Token::Keyword);
    EXPECT_TOK(b, 2, "[[", Token::Keyword);
    EXPECT_TOK(b, 2, "-f", Token::Attribute);
    EXPECT_TOK(b, 2, "$NAME", Token::Variable);
    EXPECT_TOK(b, 2, "$#", Token::Variable);
    EXPECT_TOK(b, 2, "-gt", Token::Attribute);
    EXPECT_TOK(b, 2, "]]", Token::Keyword);
    EXPECT_TOK(b, 2, "then", Token::Keyword);
    EXPECT_TOK(b, 2, "echo", Token::Builtin);
    EXPECT_TOK(b, 2, "grep", Token::Function);
    EXPECT_TOK(b, 2, "'x y'", Token::String);
    EXPECT_TOK(b, 2, "fi", Token::Keyword);
    EXPECT_TOK(b, 3, "for", Token::Keyword);
    EXPECT_TOK(b, 3, "in", Token::Keyword);
    EXPECT_TOK(b, 3, "cat", Token::Function);
    EXPECT_TOK(b, 3, "done", Token::Keyword);
    EXPECT_TOK(b, 3, "# tail", Token::Comment);
    EXPECT_TOK(b, 4, "function", Token::Keyword);
    EXPECT_TOK(b, 4, "foo", Token::Function);
    EXPECT_TOK(b, 4, "local", Token::Builtin);
    EXPECT_TOK(b, 5, "ls", Token::Function);
    EXPECT_TOK(b, 5, "--color", Token::Attribute);
    EXPECT_TOK(b, 5, "-la", Token::Attribute);
    EXPECT_TOK(b, 5, "./run.sh", Token::Function);
    EXPECT_TOK(b, 5, "42", Token::Number);
    EXPECT_TOK(b, 5, "date", Token::Function);
    // console prompt, arithmetic for-loops and case branches
    const Block k = runBlock("bash", QStringLiteral("$ ls -la\nfor ((i=0;i<3;i++)); do echo $i; done\ncase \"$x\" in a) echo A;; *) echo other;; esac"));
    EXPECT_TOK(k, 0, "$", Token::Operator);
    EXPECT_TOK(k, 0, "ls", Token::Function);
    EXPECT_TOK(k, 0, "-la", Token::Attribute);
    QCOMPARE(countTok(k, 1, Token::Function), 0); // i<3 / i++ are not commands
    EXPECT_TOK(k, 1, "echo", Token::Builtin);
    EXPECT_TOK(k, 2, "in", Token::Keyword);
    EXPECT_TOK_N(k, 2, "echo", 0, Token::Builtin);
    EXPECT_TOK_N(k, 2, "echo", 1, Token::Builtin);
    EXPECT_TOK(k, 2, "esac", Token::Keyword);
    // '#' inside a word or an expansion is not a comment
    const Block c = runBlock("bash", QStringLiteral("echo a#b ${#arr[@]} $#"));
    EXPECT_TOK(c, 0, "a#b", Token::Plain);
    QVERIFY(c.spans[0].size() >= 2);
    for (const Span &s : c.spans[0])
        QVERIFY(s.token != Token::Comment);
}

void CodeTests::lang_css()
{
    const Block b = runBlock("css", QStringLiteral("@charset \"utf-8\";\n"
                                                   ":root { --main: #06c; }\n"
                                                   "a.link:hover > span#id::before, [data-x=\"y\"] {\n"
                                                   "  color: red !important; margin: -0.5em 10px;\n"
                                                   "  background: url(img/a.png) no-repeat, var(--main);\n"
                                                   "}\n"
                                                   "@media (min-width: 600px) and (max-width: 1e3px) { .a { font: 12px/1.5 sans-serif; } }"));
    EXPECT_TOK(b, 0, "@charset", Token::Keyword);
    EXPECT_TOK(b, 0, "\"utf-8\"", Token::String);
    EXPECT_TOK(b, 1, ":root", Token::Builtin);
    EXPECT_TOK(b, 1, "--main", Token::Variable);
    EXPECT_TOK(b, 1, "#06c", Token::Number);
    EXPECT_TOK(b, 2, "a", Token::Tag);
    EXPECT_TOK(b, 2, ".link", Token::Type);
    EXPECT_TOK(b, 2, ":hover", Token::Builtin);
    EXPECT_TOK(b, 2, "span", Token::Tag);
    EXPECT_TOK(b, 2, "#id", Token::Constant);
    EXPECT_TOK(b, 2, "::before", Token::Builtin);
    EXPECT_TOK(b, 2, "data-x", Token::Attribute);
    EXPECT_TOK(b, 2, "\"y\"", Token::String);
    EXPECT_TOK(b, 3, "color", Token::Attribute);
    EXPECT_TOK(b, 3, "red", Token::Constant);
    EXPECT_TOK(b, 3, "!important", Token::Keyword);
    EXPECT_TOK(b, 3, "margin", Token::Attribute);
    EXPECT_TOK(b, 3, "-0.5em", Token::Number);
    EXPECT_TOK(b, 3, "10px", Token::Number);
    EXPECT_TOK(b, 4, "background", Token::Attribute);
    EXPECT_TOK(b, 4, "url", Token::Function);
    EXPECT_TOK(b, 4, "img/a.png", Token::String);
    EXPECT_TOK(b, 4, "no-repeat", Token::Constant);
    EXPECT_TOK(b, 4, "var", Token::Function);
    EXPECT_TOK(b, 4, "--main", Token::Variable);
    EXPECT_TOK(b, 6, "@media", Token::Keyword);
    EXPECT_TOK(b, 6, "min-width", Token::Attribute);
    EXPECT_TOK(b, 6, "600px", Token::Number);
    EXPECT_TOK(b, 6, "and", Token::Keyword);
    EXPECT_TOK(b, 6, ".a", Token::Type);   // selector inside the @media block
    EXPECT_TOK(b, 6, "font", Token::Attribute);
    EXPECT_TOK(b, 6, "12px", Token::Number);
    QCOMPARE(b.out.last(), 0);
}

void CodeTests::lang_html()
{
    const Block b = runBlock("html", QStringLiteral("<!DOCTYPE html>\n"
                                                    "<html lang=\"en\"><body class='a' data-x=unq disabled>\n"
                                                    "  <p>Hello &amp; welcome</p><br/>\n"
                                                    "  <svg:rect xlink:href=\"#a\" />\n"
                                                    "</body></html>"));
    EXPECT_TOK(b, 0, "<!DOCTYPE html>", Token::Preprocessor);
    EXPECT_TOK(b, 1, "html", Token::Tag);
    EXPECT_TOK(b, 1, "lang", Token::Attribute);
    EXPECT_TOK(b, 1, "=", Token::Operator);
    EXPECT_TOK(b, 1, "\"en\"", Token::String);
    EXPECT_TOK(b, 1, "body", Token::Tag);
    EXPECT_TOK(b, 1, "'a'", Token::String);
    EXPECT_TOK(b, 1, "data-x", Token::Attribute);
    EXPECT_TOK(b, 1, "unq", Token::String);
    EXPECT_TOK(b, 1, "disabled", Token::Attribute);
    EXPECT_TOK(b, 2, "p", Token::Tag);
    EXPECT_TOK(b, 2, "Hello", Token::Plain);
    EXPECT_TOK(b, 2, "&amp;", Token::Constant);
    EXPECT_TOK(b, 2, "br", Token::Tag);
    EXPECT_TOK(b, 2, "/>", Token::Punctuation);
    EXPECT_TOK(b, 3, "svg:rect", Token::Tag);
    EXPECT_TOK(b, 3, "xlink:href", Token::Attribute);
    EXPECT_TOK(b, 4, "</", Token::Punctuation);
    EXPECT_TOK(b, 4, "body", Token::Tag);
    // xml
    const Block x = runBlock("xml", QStringLiteral("<?xml version=\"1.0\"?>\n<a b=\"c\"><![CDATA[ <raw> ]]></a>"));
    EXPECT_TOK(x, 0, "<?xml version=\"1.0\"?>", Token::Preprocessor);
    EXPECT_TOK(x, 1, "<![CDATA[ <raw> ]]>", Token::String);
    // embedded script and style
    const Block e = runBlock("html", QStringLiteral("<script>\n  const x = `a${1}`; // js\n</script>\n<style>\n  a { color: red; }\n</style>"));
    EXPECT_TOK(e, 1, "const", Token::Keyword);
    EXPECT_TOK(e, 1, "`a", Token::String);
    EXPECT_TOK(e, 1, "// js", Token::Comment);
    EXPECT_TOK(e, 2, "script", Token::Tag);
    EXPECT_TOK(e, 4, "color", Token::Attribute);
    EXPECT_TOK(e, 4, "red", Token::Constant);
    EXPECT_TOK(e, 5, "style", Token::Tag);
    QCOMPARE(e.out.last(), 0);
}

void CodeTests::lang_json()
{
    const Block b = runBlock("json", QStringLiteral("{\n"
                                                    "  \"name\": \"folia\", \"n\": -1.5e+10, \"ok\": true, \"z\": null,\n"
                                                    "  \"list\": [1, 2, \"x\\\"y\"], // c\n"
                                                    "  \"k\" : {\"a\": []}\n"
                                                    "}"));
    EXPECT_TOK(b, 1, "\"name\"", Token::Attribute);
    EXPECT_TOK(b, 1, "\"folia\"", Token::String);
    EXPECT_TOK(b, 1, "\"n\"", Token::Attribute);
    EXPECT_TOK(b, 1, "-1.5e+10", Token::Number);
    EXPECT_TOK(b, 1, "true", Token::Constant);
    EXPECT_TOK(b, 1, "null", Token::Constant);
    EXPECT_TOK(b, 2, "1", Token::Number);
    EXPECT_TOK(b, 2, "\"x\\\"y\"", Token::String);
    EXPECT_TOK(b, 2, "// c", Token::Comment);
    EXPECT_TOK(b, 3, "\"k\"", Token::Attribute); // key followed by ' :'
    EXPECT_TOK(b, 3, "\"a\"", Token::Attribute);
    EXPECT_TOK(b, 0, "{", Token::Punctuation);
}

void CodeTests::lang_yaml()
{
    const Block b = runBlock("yaml", QStringLiteral("%YAML 1.2\n"
                                                    "---\n"
                                                    "# comment\n"
                                                    "name: Folia   # trailing\n"
                                                    "version: 1.2.3\n"
                                                    "count: 0x1F\n"
                                                    "enabled: true\n"
                                                    "base: &anchor\n"
                                                    "  key: \"quoted: value\"\n"
                                                    "  list: [1, 2.5, three, {a: b}]\n"
                                                    "ref: *anchor\n"
                                                    "tagged: !!str 12\n"
                                                    "url: http://example.com/a#b\n"
                                                    "items:\n"
                                                    "  - id: 1\n"
                                                    "  - plain scalar\n"
                                                    "  - 'single ''q'''\n"
                                                    "nothing: ~"));
    EXPECT_TOK(b, 0, "%YAML 1.2", Token::Preprocessor);
    EXPECT_TOK(b, 1, "---", Token::Punctuation);
    EXPECT_TOK(b, 2, "# comment", Token::Comment);
    EXPECT_TOK(b, 3, "name", Token::Attribute);
    EXPECT_TOK(b, 3, "# trailing", Token::Comment);
    EXPECT_TOK(b, 4, "version", Token::Attribute);
    EXPECT_TOK(b, 4, "1.2.3", Token::Number);
    EXPECT_TOK(b, 5, "0x1F", Token::Number);
    EXPECT_TOK(b, 6, "true", Token::Constant);
    EXPECT_TOK(b, 7, "&anchor", Token::Variable);
    EXPECT_TOK(b, 8, "key", Token::Attribute);
    EXPECT_TOK(b, 8, "\"quoted: value\"", Token::String);
    EXPECT_TOK(b, 9, "list", Token::Attribute);
    EXPECT_TOK(b, 9, "1", Token::Number);
    EXPECT_TOK(b, 9, "2.5", Token::Number);
    EXPECT_TOK(b, 9, "a", Token::Attribute);
    EXPECT_TOK(b, 10, "*anchor", Token::Variable);
    EXPECT_TOK(b, 11, "!!str", Token::Type);
    EXPECT_TOK(b, 11, "12", Token::Number);
    EXPECT_TOK(b, 12, "url", Token::Attribute);
    EXPECT_TOK(b, 12, "http://example.com/a#b", Token::Plain); // '#' without a space is not a comment
    EXPECT_TOK(b, 14, "-", Token::Punctuation);
    EXPECT_TOK(b, 14, "id", Token::Attribute);
    EXPECT_TOK(b, 15, "plain scalar", Token::Plain);
    EXPECT_TOK(b, 16, "'single ''q'''", Token::String);
    EXPECT_TOK(b, 17, "~", Token::Constant);
    // block scalars
    const Block s = runBlock("yaml", QStringLiteral("a:\n  desc: |\n    literal # not a comment\n      indented\n\n    end\n  next: >-\n    folded\nz: 1"));
    EXPECT_TOK(s, 1, "|", Token::Operator);
    EXPECT_TOK(s, 2, "literal # not a comment", Token::String);
    EXPECT_TOK(s, 3, "indented", Token::String);
    QVERIFY(s.out[4] != 0); // blank line stays inside
    EXPECT_TOK(s, 5, "end", Token::String);
    EXPECT_TOK(s, 6, "next", Token::Attribute);
    EXPECT_TOK(s, 6, ">-", Token::Operator);
    EXPECT_TOK(s, 7, "folded", Token::String);
    EXPECT_TOK(s, 8, "z", Token::Attribute);
    QCOMPARE(s.out[8], 0);
}

void CodeTests::lang_toml()
{
    const Block b = runBlock("toml", QStringLiteral("# comment\n"
                                                    "title = \"TOML \\\"x\\\"\"\n"
                                                    "[owner.info]\n"
                                                    "name = 'Tom'  # t\n"
                                                    "dob = 1979-05-27T07:32:00-08:00\n"
                                                    "[[products]]\n"
                                                    "ports = [ 8000, 8001,\n"
                                                    "  8002 ]\n"
                                                    "a.b.c = { x = 1, \"y z\" = true }\n"
                                                    "f = -1_000.5e+3\n"
                                                    "h = 0xDEAD_BEEF\n"
                                                    "i = +inf"));
    EXPECT_TOK(b, 0, "# comment", Token::Comment);
    EXPECT_TOK(b, 1, "title", Token::Attribute);
    EXPECT_TOK(b, 1, "=", Token::Operator);
    EXPECT_TOK(b, 1, "\"TOML \\\"x\\\"\"", Token::String);
    EXPECT_TOK(b, 2, "[owner.info]", Token::Tag);
    EXPECT_TOK(b, 3, "name", Token::Attribute);
    EXPECT_TOK(b, 3, "'Tom'", Token::String);
    EXPECT_TOK(b, 3, "# t", Token::Comment);
    EXPECT_TOK(b, 4, "1979-05-27T07:32:00-08:00", Token::Number);
    EXPECT_TOK(b, 5, "[[products]]", Token::Tag);
    EXPECT_TOK(b, 6, "ports", Token::Attribute);
    EXPECT_TOK(b, 6, "8000", Token::Number);
    QVERIFY(b.out[6] != 0);
    EXPECT_TOK(b, 7, "8002", Token::Number);
    QCOMPARE(b.out[7], 0);
    EXPECT_TOK(b, 8, "a", Token::Attribute);
    EXPECT_TOK(b, 8, "b", Token::Attribute);
    EXPECT_TOK(b, 8, "c", Token::Attribute);
    EXPECT_TOK(b, 8, "x", Token::Attribute);
    EXPECT_TOK(b, 8, "\"y z\"", Token::Attribute);
    EXPECT_TOK(b, 8, "true", Token::Constant);
    EXPECT_TOK(b, 9, "-1_000.5e+3", Token::Number);
    EXPECT_TOK(b, 10, "0xDEAD_BEEF", Token::Number);
    EXPECT_TOK(b, 11, "inf", Token::Constant);
}

void CodeTests::lang_diff()
{
    const Block b = runBlock("diff", QStringLiteral("diff --git a/x.cpp b/x.cpp\n"
                                                    "index 83db48f..bf2a4c1 100644\n"
                                                    "--- a/x.cpp\n"
                                                    "+++ b/x.cpp\n"
                                                    "@@ -1,4 +1,5 @@ int main()\n"
                                                    " context\n"
                                                    "-removed line\n"
                                                    "+added line\n"
                                                    "\\ No newline at end of file"));
    EXPECT_TOK(b, 0, "diff --git a/x.cpp b/x.cpp", Token::Comment);
    EXPECT_TOK(b, 1, "index 83db48f..bf2a4c1 100644", Token::Comment);
    EXPECT_TOK(b, 2, "--- a/x.cpp", Token::Keyword);
    EXPECT_TOK(b, 3, "+++ b/x.cpp", Token::Keyword);
    EXPECT_TOK(b, 4, "@@ -1,4 +1,5 @@", Token::Preprocessor);
    EXPECT_TOK(b, 4, " int main()", Token::Function);
    EXPECT_TOK(b, 5, " context", Token::Plain);
    EXPECT_TOK(b, 6, "-removed line", Token::Tag);
    EXPECT_TOK(b, 7, "+added line", Token::String);
    EXPECT_TOK(b, 8, "\\ No newline at end of file", Token::Comment);
    for (int v : b.out)
        QCOMPARE(v, 0);
}

// ===========================================================================
// multi-line state
// ===========================================================================
void CodeTests::state_cBlockComment()
{
    for (const char *lang : {"c", "cpp", "csharp", "java", "go", "javascript", "typescript", "rust", "swift", "css", "sql", "json"}) {
        const Block b = runBlock(lang, QStringLiteral("a /* open\nstill inside\nclose */ b"));
        QVERIFY2(b.out[0] != 0, lang);
        QVERIFY2(b.out[1] == b.out[0], lang); // stable while inside
        QCOMPARE(b.out[2], 0);
        QCOMPARE(tokOf(b, 1, QStringLiteral("still inside")), Token::Comment);
        QCOMPARE(tokOf(b, 2, QStringLiteral("close */")), Token::Comment);
    }
    // not a comment: // inside a block comment, and block markers inside strings
    const Block s = runBlock("c", QStringLiteral("char *p = \"/* not\"; int x; /* c */ int y;"));
    QCOMPARE(s.out[0], 0);
    EXPECT_TOK(s, 0, "int", Token::Type);
    // an unterminated block comment hides everything until closed
    const Block u = runBlock("c", QStringLiteral("/*\nint x = 1;\n\n*/ int y;"));
    EXPECT_TOK(u, 1, "int", Token::Comment);
    QCOMPARE(u.out[2], u.out[1]);
    EXPECT_TOK(u, 3, "int", Token::Type);
    // c++: '/*/' does not close
    const Block t = runBlock("cpp", QStringLiteral("/*/ still\n*/ done"));
    QVERIFY(t.out[0] != 0);
    QCOMPARE(t.out[1], 0);
}

void CodeTests::state_pythonTriple()
{
    const Block b = runBlock("python", QStringLiteral("a = \"\"\"doc ' with ''' inside\nmiddle \\\"\"\" still\nend\"\"\" + 1\n"
                                                      "b = '''x\"\"\"y\n'''\nc = 2"));
    QVERIFY(b.out[0] != 0);
    QVERIFY(b.out[1] != 0);                 // \""" is escaped, string goes on
    QCOMPARE(b.out[1], b.out[0]);
    QCOMPARE(b.out[2], 0);
    EXPECT_TOK(b, 2, "end\"\"\"", Token::String);
    EXPECT_TOK(b, 2, "1", Token::Number);
    QVERIFY(b.out[3] != 0);                 // ''' string, """ inside does not close
    QVERIFY(b.out[3] != b.out[0]);          // different quote kind => different state
    QCOMPARE(b.out[4], 0);
    EXPECT_TOK(b, 5, "2", Token::Number);
    // empty strings and adjacent quotes are not triples
    const Block e = runBlock("python", QStringLiteral("x = '' + \"\" + ''\ny = 1"));
    QCOMPARE(e.out[0], 0);
    // prefixes
    const Block p = runBlock("python", QStringLiteral("x = rb'\\d' + Rb\"x\" + f\"\"\"a\n{b}\"\"\" + u'z'"));
    QCOMPARE(p.out[1], 0);
    EXPECT_TOK(p, 0, "rb'\\d'", Token::String);
    EXPECT_TOK(p, 0, "Rb\"x\"", Token::String);
    EXPECT_TOK(p, 1, "{b}\"\"\"", Token::String);
    EXPECT_TOK(p, 1, "u'z'", Token::String);
    // backslash continuation of a single-quoted string
    const Block c = runBlock("python", QStringLiteral("s = 'abc \\\ndef'\nz = 1"));
    QVERIFY(c.out[0] != 0);
    QCOMPARE(c.out[1], 0);
    EXPECT_TOK(c, 1, "def'", Token::String);
    // unterminated single-quoted strings end at the end of the line
    const Block u = runBlock("python", QStringLiteral("s = 'oops\nz = 1"));
    QCOMPARE(u.out[0], 0);
    EXPECT_TOK(u, 1, "1", Token::Number);
}

void CodeTests::state_jsTemplates()
{
    const Block b = runBlock("javascript", QStringLiteral("const t = `line one ${a + `inner ${b}`}\n"
                                                          "two ${ {x: 1}.x } and \\` escaped\n"
                                                          "three ${\n"
                                                          "  call(1)\n"
                                                          "} end` + 5;\n"
                                                          "done()"));
    QVERIFY(b.out[0] != 0);
    QVERIFY(b.out[1] != 0);
    QVERIFY(b.out[2] != 0);
    QVERIFY(b.out[3] != 0);
    QCOMPARE(b.out[4], 0);
    EXPECT_TOK(b, 0, "`line one ", Token::String);
    EXPECT_TOK(b, 0, "${", Token::Punctuation);
    EXPECT_TOK(b, 0, "`inner ", Token::String);
    EXPECT_TOK(b, 1, "two ", Token::String);
    EXPECT_TOK(b, 1, "1", Token::Number);
    EXPECT_TOK(b, 1, " and \\` escaped", Token::String);
    EXPECT_TOK(b, 2, "three ", Token::String);
    EXPECT_TOK(b, 3, "call", Token::Function);   // code inside ${ } across lines
    EXPECT_TOK(b, 3, "1", Token::Number);
    EXPECT_TOK(b, 4, " end`", Token::String);
    EXPECT_TOK(b, 4, "5", Token::Number);
    EXPECT_TOK(b, 5, "done", Token::Function);
    // a template with a code state that is not a string state
    QVERIFY(b.out[2] != b.out[1]);
    // braces inside ${ } do not end the interpolation early
    const Block c = runBlock("typescript", QStringLiteral("x = `${ {a: {b: 1}}.a }` + y"));
    QCOMPARE(c.out[0], 0);
    EXPECT_TOK(c, 0, "y", Token::Plain);
    EXPECT_TOK(c, 0, "+", Token::Operator);
    // comments and postfix ++/-- do not turn a division into a regex
    const Block d = runBlock("javascript", QStringLiteral("x = y /* c */ / z // end\ni++ / 2; w = a-- / b / c;"));
    QCOMPARE(countTok(d, 0, Token::String), 0);
    QCOMPARE(countTok(d, 1, Token::String), 0);
    EXPECT_TOK(d, 0, "// end", Token::Comment);
    // regex vs division
    const Block r = runBlock("javascript", QStringLiteral("x = a / b; y = /re\\/x/g.test(s); z = (a) / 2 / 3;"));
    EXPECT_TOK(r, 0, "/re\\/x/g", Token::String);
    EXPECT_TOK_N(r, 0, "/", 0, Token::Operator);
}

void CodeTests::state_rustRawAndNested()
{
    const Block b = runBlock("rust", QStringLiteral("let a = r#\"one \"quote\" \n"
                                                    "two \"# + 1; let b = r##\"x\"# y\n"
                                                    "z\"##; let c = r\"plain raw\n"
                                                    "second\"; let d = br#\"bytes\"#;"));
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 0, "r#\"one \"quote\" ", Token::String);
    EXPECT_TOK(b, 1, "two \"#", Token::String);   // closes the one-hash string
    EXPECT_TOK(b, 1, "1", Token::Number);
    QVERIFY(b.out[1] != 0);
    QVERIFY(b.out[1] != b.out[0]);                 // two hashes now
    EXPECT_TOK(b, 2, "z\"##", Token::String);     // "# inside r## does not close it, "## does
    QVERIFY(b.out[2] != 0);
    QVERIFY(b.out[2] != b.out[1]);
    QVERIFY(b.out[2] != b.out[0]);                 // zero hashes
    EXPECT_TOK(b, 3, "second\"", Token::String);
    EXPECT_TOK(b, 3, "br#\"bytes\"#", Token::String);
    QCOMPARE(b.out[3], 0);
    // ordinary rust strings may span lines
    const Block s = runBlock("rust", QStringLiteral("let s = \"multi\nline \\\" more\"; let n = 1;"));
    QVERIFY(s.out[0] != 0);
    EXPECT_TOK(s, 1, "line \\\" more\"", Token::String);
    EXPECT_TOK(s, 1, "1", Token::Number);
    QCOMPARE(s.out[1], 0);
    // nested block comments
    const Block n = runBlock("rust", QStringLiteral("/* a /* b\nc */ still\nd */ fn x() {}"));
    QVERIFY(n.out[0] != 0);
    QVERIFY(n.out[1] != 0);
    QVERIFY(n.out[0] != n.out[1]);                 // depth 2 vs depth 1
    EXPECT_TOK(n, 1, "c */ still", Token::Comment);
    EXPECT_TOK(n, 2, "d */", Token::Comment);
    EXPECT_TOK(n, 2, "fn", Token::Keyword);
    QCOMPARE(n.out[2], 0);
}

void CodeTests::state_luaLong()
{
    const Block b = runBlock("lua", QStringLiteral("local s = [==[ a ]] b ]=] c\n"
                                                   "d ]==] .. 'x'\n"
                                                   "--[[ c1\n"
                                                   "still ]] local y = 1\n"
                                                   "--[==[ c2 ]] ]=]\n"
                                                   "end ]==] print(1)\n"
                                                   "t[a[1]] = [[x]]"));
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 1, "d ]==]", Token::String);
    EXPECT_TOK(b, 1, "'x'", Token::String);
    QCOMPARE(b.out[1], 0);
    QVERIFY(b.out[2] != 0);
    EXPECT_TOK(b, 3, "still ]]", Token::Comment);
    EXPECT_TOK(b, 3, "local", Token::Keyword);
    QCOMPARE(b.out[3], 0);
    QVERIFY(b.out[4] != 0);
    QVERIFY(b.out[4] != b.out[2]);                       // level 2 vs level 0
    EXPECT_TOK(b, 5, "end ]==]", Token::Comment);
    EXPECT_TOK(b, 5, "print", Token::Builtin);
    QCOMPARE(b.out[5], 0);
    EXPECT_TOK(b, 6, "[[x]]", Token::String);
    EXPECT_TOK(b, 6, "1", Token::Number);
}

void CodeTests::state_cppRaw()
{
    const Block b = runBlock("cpp", QStringLiteral("auto a = R\"sql(select \")\" from\n"
                                                   "t)sql\"; auto b = R\"(x)\" + R\"abc(y\n"
                                                   ")ab)abc\"; int z;"));
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 0, "R\"sql(select \")\" from", Token::String);
    EXPECT_TOK(b, 1, "t)sql\"", Token::String);
    EXPECT_TOK(b, 1, "R\"(x)\"", Token::String);
    QVERIFY(b.out[1] != 0);
    QVERIFY(b.out[1] != b.out[0]);                          // different delimiter => different state
    EXPECT_TOK(b, 2, ")ab)abc\"", Token::String);
    EXPECT_TOK(b, 2, "int", Token::Type);
    QCOMPARE(b.out[2], 0);
}

void CodeTests::state_swiftMultiline()
{
    const Block b = runBlock("swift", QStringLiteral("let m = \"\"\"\n"
                                                     "  text \"quoted\" \\\"\"\" still\n"
                                                     "  \"\"\" + x\n"
                                                     "let r = #\"\"\"\n"
                                                     "  raw \\n \"\"\" not end\n"
                                                     "  \"\"\"# + y\n"
                                                     "/* a /* nested */ still */ let z = 1"));
    QVERIFY(b.out[0] != 0);
    QVERIFY(b.out[1] != 0);
    QCOMPARE(b.out[2], 0);
    EXPECT_TOK(b, 2, "  \"\"\"", Token::String);
    QVERIFY(b.out[3] != 0);
    QVERIFY(b.out[4] != 0);
    QCOMPARE(b.out[5], 0);
    EXPECT_TOK(b, 5, "  \"\"\"#", Token::String);
    EXPECT_TOK(b, 6, "/* a /* nested */ still */", Token::Comment);
    EXPECT_TOK(b, 6, "let", Token::Keyword);
}

void CodeTests::state_goRaw()
{
    const Block b = runBlock("go", QStringLiteral("q := `select *\n"
                                                  "from \"t\" \\n -- not a comment\n"
                                                  "`; x := 1"));
    QVERIFY(b.out[0] != 0);
    QVERIFY(b.out[1] != 0);
    QCOMPARE(b.out[2], 0);
    EXPECT_TOK(b, 1, "from \"t\" \\n -- not a comment", Token::String);
    EXPECT_TOK(b, 2, "`", Token::String);
    EXPECT_TOK(b, 2, "1", Token::Number);
}

void CodeTests::state_htmlSpanning()
{
    const Block b = runBlock("html", QStringLiteral("<!-- a\n"
                                                    "  <b>not a tag</b>\n"
                                                    "--><div\n"
                                                    "   id=\"x\"\n"
                                                    "   title='multi\n"
                                                    "line'\n"
                                                    "   class=a>text</div>"));
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 1, "<b>not a tag</b>", Token::Comment);
    QCOMPARE(b.out[1], b.out[0]);
    QCOMPARE(b.out[2] != 0, true);               // inside the <div tag
    EXPECT_TOK(b, 2, "-->", Token::Comment);
    EXPECT_TOK(b, 2, "div", Token::Tag);
    EXPECT_TOK(b, 3, "id", Token::Attribute);
    EXPECT_TOK(b, 3, "\"x\"", Token::String);
    QVERIFY(b.out[4] != 0);                      // unterminated attribute value
    QVERIFY(b.out[4] != b.out[3]);
    EXPECT_TOK(b, 5, "line'", Token::String);
    EXPECT_TOK(b, 6, "class", Token::Attribute);
    EXPECT_TOK_N(b, 6, "a", 1, Token::String); // unquoted value
    EXPECT_TOK(b, 6, ">", Token::Punctuation);
    EXPECT_TOK(b, 6, "div", Token::Tag);
    QCOMPARE(b.out[6], 0);
    // script bodies carry the javascript state (template literal spanning lines)
    const Block s = runBlock("html", QStringLiteral("<script>\nvar t = `a\nb ${c}`;\n</script>\n<p>x</p>"));
    QVERIFY(s.out[1] != s.out[0]);
    EXPECT_TOK(s, 2, "b ", Token::String);
    EXPECT_TOK(s, 3, "script", Token::Tag);
    QCOMPARE(s.out[3], 0);
    EXPECT_TOK(s, 4, "p", Token::Tag);
}

void CodeTests::state_sqlBlock()
{
    const Block b = runBlock("sql", QStringLiteral("select 1 /* a\n  b */ + 2; select 'multi\nline ''q'' end'; $$ body\n$$;"));
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 1, "b */", Token::Comment);
    EXPECT_TOK(b, 1, "2", Token::Number);
    QVERIFY(b.out[1] != 0);
    EXPECT_TOK(b, 2, "line ''q'' end'", Token::String);
    QVERIFY(b.out[2] != 0);
    QCOMPARE(b.out[3], 0);
}

void CodeTests::state_bashHeredoc()
{
    const Block b = runBlock("bash", QStringLiteral("cat <<EOF > out\n"
                                                    "  $notvar 'x' # not a comment\n"
                                                    "EOF\n"
                                                    "cat <<-'END' | wc\n"
                                                    "\tbody\n"
                                                    "\tEND\n"
                                                    "echo done\n"
                                                    "x=$((1 << 2)); echo \"multi\n"
                                                    "line\" ; echo 'a\nb'\n"
                                                    "echo a \\\n"
                                                    "  b"));
    EXPECT_TOK(b, 0, "<<", Token::Operator);
    EXPECT_TOK(b, 0, "EOF", Token::Constant);
    QVERIFY(b.out[0] != 0);
    EXPECT_TOK(b, 1, "  $notvar 'x' # not a comment", Token::String);
    QCOMPARE(b.out[1], b.out[0]);
    QCOMPARE(b.out[2], 0);
    QVERIFY(b.out[3] != 0);
    QVERIFY(b.out[3] != b.out[0]);                // different delimiter, different fingerprint
    QVERIFY(b.out[4] == b.out[3]);
    QCOMPARE(b.out[5], 0);                        // <<- strips leading tabs
    EXPECT_TOK(b, 6, "echo", Token::Builtin);
    QVERIFY(b.out[7] != 0);                       // `1 << 2` is not a heredoc; the open "..." is
    EXPECT_TOK(b, 8, "line\"", Token::String);
    QVERIFY(b.out[8] != 0);                       // open '...'
    EXPECT_TOK(b, 9, "b'", Token::String);
    QCOMPARE(b.out[9], 0);
    QVERIFY(b.out[10] != 0);                      // backslash continuation
    EXPECT_TOK(b, 11, "b", Token::Plain);         // argument position, not a command
    QCOMPARE(b.out[11], 0);
}

void CodeTests::state_misc()
{
    // Java text blocks, C# raw/verbatim strings, Python-style continuation in Lua strings
    const Block j = runBlock("java", QStringLiteral("String t = \"\"\"\n  hi \"x\" \\\"\"\"\n  \"\"\"; int n;"));
    QVERIFY(j.out[0] != 0);
    QVERIFY(j.out[1] != 0);
    QCOMPARE(j.out[2], 0);
    EXPECT_TOK(j, 2, "int", Token::Type);
    const Block c = runBlock("csharp", QStringLiteral("var v = @\"a\n  \"\"q\"\" b\"; var r = \"\"\"\n  raw \\n\n  \"\"\"; int n;"));
    QVERIFY(c.out[0] != 0);
    QCOMPARE(c.out[1] != 0, true);
    EXPECT_TOK(c, 1, "  \"\"q\"\" b\"", Token::String);
    QVERIFY(c.out[2] != 0);
    QCOMPARE(c.out[3], 0);
    EXPECT_TOK(c, 3, "int", Token::Type);
    // C: string continued by a trailing backslash
    const Block s = runBlock("c", QStringLiteral("char *s = \"a \\\n b\"; int x;"));
    QVERIFY(s.out[0] != 0);
    QCOMPARE(s.out[1], 0);
    EXPECT_TOK(s, 1, " b\"", Token::String);
    EXPECT_TOK(s, 1, "int", Token::Type);
    // toml multi-line strings and arrays
    const Block t = runBlock("toml", QStringLiteral("a = \"\"\"\nx \\\"\"\" y\n\"\"\"\nb = '''\n\\n '' \n'''\nc = [\n  [1, 2],\n  \"s\", # c\n]"));
    QVERIFY(t.out[0] != 0);
    QVERIFY(t.out[1] == t.out[0]);
    QCOMPARE(t.out[2], 0);
    QVERIFY(t.out[3] != 0 && t.out[3] != t.out[0]);
    QCOMPARE(t.out[5], 0);
    QVERIFY(t.out[6] != 0);
    QVERIFY(t.out[7] != 0);
    QVERIFY(t.out[8] != 0);
    QCOMPARE(t.out[9], 0);
    // css: comment and @media nesting carry across lines
    const Block css = runBlock("css", QStringLiteral("@media print {\n  /* c\n  */ a { color: red; }\n  b {\n    margin: 0\n      auto;\n  }\n}\nc { d: e }"));
    QVERIFY(css.out[0] != 0);
    QVERIFY(css.out[1] != css.out[0]);
    EXPECT_TOK(css, 2, "a", Token::Tag);
    EXPECT_TOK(css, 5, "auto", Token::Constant); // value continues on the next line
    QCOMPARE(css.out[7], 0);
    EXPECT_TOK(css, 8, "d", Token::Attribute);
}

void CodeTests::stabilization_converges()
{
    // C++: opening a block comment in the middle only affects the lines up to
    // the closing marker; states after it converge back to the old ones.
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines << QStringLiteral("int v%1 = %1; // line %1").arg(i);
    lines[25] = QStringLiteral("/* old close */ int q;");
    Block b = runBlock("cpp", lines.join(QLatin1Char('\n')));
    const QVector<int> oldOut = b.out;
    b.lines[10] = QStringLiteral("int v10 = /* opens");
    const int relexed = rehighlightFrom(b, 10);
    QCOMPARE(relexed, 16);              // lines 10..25 (the line with the closing marker converges)
    QVERIFY(b.out[10] != 0);
    QVERIFY(b.out[24] != 0);
    for (int i = 25; i < 40; ++i)
        QCOMPARE(b.out[i], oldOut[i]);
    QCOMPARE(tokOf(b, 12, QStringLiteral("int")), Token::Comment);
    // editing inside a block that does not change nesting stops after one line
    b = runBlock("cpp", lines.join(QLatin1Char('\n')));
    b.lines[20] = QStringLiteral("int renamed = 5;");
    QCOMPARE(rehighlightFrom(b, 20), 1);
    // python: removing a closing """ swallows following lines until the next """
    QStringList py;
    py << QStringLiteral("x = 1");
    py << QStringLiteral("s = \"\"\"doc");
    for (int i = 0; i < 6; ++i)
        py << QStringLiteral("text %1").arg(i);
    py << QStringLiteral("\"\"\"");
    for (int i = 0; i < 20; ++i)
        py << QStringLiteral("y%1 = %1").arg(i);
    Block p = runBlock("python", py.join(QLatin1Char('\n')));
    const QVector<int> pOld = p.out;
    p.lines[8] = QStringLiteral("end of doc");   // delete the closer: string runs to EOF
    QVERIFY(rehighlightFrom(p, 8) > 15);
    QVERIFY(p.out.last() != 0);
    p.lines[8] = QStringLiteral("\"\"\"");        // put it back: every swallowed line is re-lexed once
    QCOMPARE(rehighlightFrom(p, 8), p.lines.size() - 8);
    QCOMPARE(p.out, pOld);
    // ... but an edit that leaves the state unchanged stops immediately
    p.lines[3] = QStringLiteral("other text");
    QCOMPARE(rehighlightFrom(p, 3), 1);
    // rust: nested comment depth in the state
    Block r = runBlock("rust", QStringLiteral("/* a\n/* b\nc */\nd */\nfn x() {}"));
    QVERIFY(r.out[1] != r.out[0]);
    QCOMPARE(r.out[3], 0);
}

void CodeTests::stabilization_incrementalEqualsFull_data()
{
    QTest::addColumn<QString>("lang");
    for (const QString &l : kAllLanguages)
        QTest::newRow(qPrintable(l)) << l;
}

void CodeTests::stabilization_incrementalEqualsFull()
{
    QFETCH(QString, lang);
    const QStringList p = pool(lang);
    std::mt19937 rng(0xC0FFEEu ^ unsigned(qHash(lang)));
    for (int doc = 0; doc < 12; ++doc) {
        QStringList lines;
        const int count = 30 + int(rng() % 30);
        for (int i = 0; i < count; ++i)
            lines << p[int(rng() % p.size())];
        Block b;
        b.lang = languageFor(lang);
        b.lines = lines;
        b.spans.resize(lines.size());
        b.in = QVector<int>(lines.size(), -1);
        b.out = QVector<int>(lines.size(), -1);
        // full rebuild reference
        auto verify = [&](const char *what) {
            QVector<int> ref;
            int st = 0;
            for (int i = 0; i < b.lines.size(); ++i) {
                QVector<Span> sp;
                const int o = highlightLine(b.lang, b.lines[i], st, sp);
                ref.append(o);
                if (!sameSpans(sp, b.spans[i]) || o != b.out[i] || st != b.in[i]) {
                    QFAIL(qPrintable(QStringLiteral("%1: incremental != full at line %2 in %3 (doc %4)").arg(QLatin1String(what)).arg(i).arg(lang).arg(doc)));
                }
                st = o;
            }
        };
        rehighlightFrom(b, 0); // incoming states start at -1, so this lexes every line
        verify("initial");
        for (int edit = 0; edit < 40; ++edit) {
            const int at = int(rng() % b.lines.size());
            switch (rng() % 3) {
            case 0: // replace
                b.lines[at] = p[int(rng() % p.size())];
                rehighlightFrom(b, at);
                break;
            case 1: // insert
                b.lines.insert(at, p[int(rng() % p.size())]);
                b.spans.insert(at, {});
                b.in.insert(at, -1);
                b.out.insert(at, -1);
                rehighlightFrom(b, at);
                break;
            default: // delete
                if (b.lines.size() > 3) {
                    b.lines.removeAt(at);
                    b.spans.removeAt(at);
                    b.in.removeAt(at);
                    b.out.removeAt(at);
                    if (at < b.lines.size())
                        rehighlightFrom(b, at);
                    else
                        continue;
                }
                break;
            }
            verify("edit");
        }
    }
}

void CodeTests::statesAreCanonical()
{
    // Same lexical situation => same state, regardless of how we got there.
    struct Case {
        const char *lang;
        const char *a;
        const char *b;
    };
    const Case cases[] = {
        {"c", "int a; /* x", "/* y"},
        {"cpp", "foo(); auto s = R\"d(abc", "R\"d("},
        {"python", "x = 1; s = '''abc", "'''zzz"},
        {"javascript", "let a = 1; t = `abc", "`def"},
        {"javascript", "t = `a ${ b +", "`x${"},
        {"rust", "let x = 1; /* a /* b", "/* /* "},
        {"rust", "let s = r##\"abc", "r##\"x y"},
        {"lua", "x = 1 --[==[ foo", "--[==["},
        {"sql", "select 1 /* c", "/* d"},
        {"html", "<p>text <!-- c", "<!--"},
        {"bash", "echo \"abc", "\""},
        {"swift", "let a = 1 /* x /* y", "/* /* "},
    };
    for (const Case &c : cases) {
        const Block a = runBlock(c.lang, QString::fromLatin1(c.a));
        const Block b = runBlock(c.lang, QString::fromLatin1(c.b));
        QVERIFY2(a.out[0] != 0, c.a);
        QCOMPARE(a.out[0], b.out[0]);
    }
    // All the lines of an ordinary statement leave the state at 0.
    for (const QString &l : kAllLanguages) {
        const Block b = runBlock(l, QStringLiteral("x = 1;\ny = foo(2, 3)\n"));
        QCOMPARE(b.out[0], 0);
        QCOMPARE(b.out[1], 0);
    }
}

// ===========================================================================
// contract / robustness
// ===========================================================================
void CodeTests::contract_appendAndNullLang()
{
    const Language *c = languageFor(u"c");
    QVector<Span> sp;
    sp.append(Span{0, 99, Token::Comment}); // pre-existing span of another "line"
    highlightLine(c, u"int x;", 0, sp);
    QVERIFY(sp.size() > 1);
    QCOMPARE(sp[0].length, 99); // untouched
    QCOMPARE(sp[1].start, 0);   // not merged into the foreign span
    // empty line
    sp.clear();
    QCOMPARE(highlightLine(c, QStringView(), 0, sp), 0);
    QVERIFY(sp.isEmpty());
    QCOMPARE(highlightLine(c, u"", 7777, sp) >= 0, true);
    // negative / garbage incoming states must not crash
    for (const QString &l : kAllLanguages) {
        const Language *lg = languageFor(l);
        for (int st : {-1, -123456789, 0x7FFFFFFF, int(0x80000000u), 1, 2, 3, 15, 16, 255, 0x12345678}) {
            QVector<Span> out;
            const QString line = QStringLiteral("a = \"x\" /* y */ <b> ${z} ' ` \\ [[ ]] #");
            highlightLine(lg, line, st, out);
            QVERIFY2(checkSpans(line, out).isEmpty(), qPrintable(l));
        }
    }
}

void CodeTests::contract_noAllocation()
{
    // Warm everything (language tables, embedded languages) first.
    for (const QString &l : kAllLanguages)
        languageFor(l);
#ifdef __APPLE__
    malloc_logger = countingMallocLogger;
#endif
    // negative control: the counting hooks really see allocations
    g_allocs = 0;
    g_track = true;
    {
        static int *volatile probe; // volatile store: the allocation cannot be elided
        probe = new int(7);
        delete probe;
        QVector<int> v;
        v.append(1); // Qt containers allocate through malloc
    }
    g_track = false;
    // operator new must always be visible; the malloc-level hook is only active
    // with the system allocator (not under a sanitizer's allocator)
    QVERIFY2(g_allocs.load() >= 1, "the allocation counting hook does not see allocations");
    std::fprintf(stdout, "allocation hooks: %s\n", g_allocs.load() >= 3 ? "operator new + malloc" : "operator new only");
    long total = 0;
    for (const QString &l : kAllLanguages) {
        const QStringList p = pool(l);
        const Language *lg = languageFor(l);
        std::mt19937 rng(1234);
        QVector<Span> out;
        out.reserve(4096);
        int st = 0;
        QStringList lines;
        for (int i = 0; i < 300; ++i)
            lines << p[int(rng() % p.size())] + p[int(rng() % p.size())];
        g_allocs = 0;
        g_track = true;
        for (const QString &ln : std::as_const(lines)) {
            out.clear();
            st = highlightLine(lg, ln, st, out);
        }
        g_track = false;
        total += g_allocs;
        QVERIFY2(g_allocs == 0, qPrintable(QStringLiteral("%1 allocated %2 times").arg(l).arg(g_allocs.load())));
    }
    QCOMPARE(total, 0L);
#ifdef __APPLE__
    malloc_logger = nullptr;
#endif
}

void CodeTests::unicode_safety_data()
{
    QTest::addColumn<QString>("lang");
    for (const QString &l : kAllLanguages)
        QTest::newRow(qPrintable(l)) << l;
}

void CodeTests::unicode_safety()
{
    QFETCH(QString, lang);
    const QString smile = QString::fromUtf8("\xF0\x9F\x98\x80");          // U+1F600 (surrogate pair)
    const QString thumbs = QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"); // emoji + skin tone modifier
    const QString cjk = QString::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87");     // 中文
    const QString combining = QString::fromUtf8("e\xCC\x81");              // e + combining acute
    const QString zwj = QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"); // ZWJ sequence
    const QStringList uni = {smile, thumbs, cjk, combining, zwj, smile + cjk + smile, QString::fromUtf8("\xC3\xA9t\xC3\xA9")};
    const QStringList p = pool(lang);
    const Language *lg = languageFor(lang);

    // emoji/CJK placed in front of, between and after the usual line fragments
    std::mt19937 rng(77);
    for (int iter = 0; iter < 4000; ++iter) {
        QString line;
        const int parts = 1 + int(rng() % 5);
        for (int k = 0; k < parts; ++k) {
            line += p[int(rng() % p.size())];
            if (rng() % 2)
                line += uni[int(rng() % uni.size())];
            if (rng() % 3 == 0)
                line += QLatin1Char(" \"'`/*#<\\"[rng() % 10]);
        }
        const int stIn = (rng() % 4 == 0) ? int(rng() % 200) : 0;
        QVector<Span> out;
        highlightLine(lg, line, stIn, out);
        const QString err = checkSpans(line, out);
        if (!err.isEmpty())
            QFAIL(qPrintable(QStringLiteral("%1: %2 in line [%3]").arg(lang, err, line)));
    }

    // concrete expectations: string / comment spans contain the whole emoji
    struct Probe {
        const char *lang;
        const char *prefix;
        const char *suffix;
        Token tok;
    };
    const Probe probes[] = {
        {"c", "char *s = \"", "\";", Token::String},          {"cpp", "auto s = \"", "\";", Token::String},
        {"csharp", "var s = \"", "\";", Token::String},       {"java", "String s = \"", "\";", Token::String},
        {"go", "s := \"", "\"", Token::String},               {"rust", "let s = \"", "\";", Token::String},
        {"swift", "let s = \"", "\"", Token::String},         {"lua", "local s = '", "'", Token::String},
        {"python", "s = '", "'", Token::String},              {"javascript", "let s = `", "`;", Token::String},
        {"typescript", "let s = \"", "\";", Token::String},   {"sql", "select '", "';", Token::String},
        {"bash", "echo \"", "\"", Token::String},             {"json", "{\"k\": \"", "\"}", Token::String},
        {"yaml", "k: \"", "\"", Token::String},               {"toml", "k = \"", "\"", Token::String},
        {"c", "/* ", " */", Token::Comment},                  {"python", "# ", "", Token::Comment},
        {"javascript", "// ", "", Token::Comment},            {"html", "<!-- ", " -->", Token::Comment},
        {"yaml", "# ", "", Token::Comment},                   {"sql", "-- ", "", Token::Comment},
        {"css", "/* ", " */", Token::Comment},                {"bash", "# ", "", Token::Comment},
    };
    for (const Probe &pr : probes) {
        if (lang != QLatin1String(pr.lang))
            continue;
        for (const QString &u : uni) {
            const QString line = QString::fromLatin1(pr.prefix) + u + QString::fromLatin1(pr.suffix);
            QVector<Span> out;
            highlightLine(lg, line, 0, out);
            QVERIFY2(checkSpans(line, out).isEmpty(), qPrintable(line));
            const int at = QString::fromLatin1(pr.prefix).size();
            bool covered = false;
            for (const Span &s : out)
                if (s.token == pr.tok && s.start <= at && s.start + s.length >= at + u.size())
                    covered = true;
            QVERIFY2(covered, qPrintable(QStringLiteral("%1: [%2] not covered by %3").arg(lang, line, tokenName(pr.tok))));
        }
    }
    // identifiers made of non-ASCII letters before '(' are still function calls
    if (lang == QLatin1String("python") || lang == QLatin1String("javascript") || lang == QLatin1String("go")) {
        const QString line = cjk + QStringLiteral("(1) + ") + smile + QStringLiteral("x");
        QVector<Span> out;
        highlightLine(lg, line, 0, out);
        QVERIFY(checkSpans(line, out).isEmpty());
        QVERIFY(!out.isEmpty());
        QCOMPARE(out[0].token, Token::Function);
        QCOMPARE(out[0].length, cjk.size());
    }
}

void CodeTests::fuzz_data()
{
    QTest::addColumn<QString>("lang");
    for (const QString &l : kAllLanguages)
        QTest::newRow(qPrintable(l)) << l;
}

void CodeTests::fuzz()
{
    QFETCH(QString, lang);
    const Language *lg = languageFor(lang);
    const QStringList p = pool(lang);
    std::mt19937 rng(0xBADC0DEu ^ unsigned(qHash(lang)));
    int st = 0;
    QVector<Span> out;
    // 20k random lines (the 19 languages x 20k = 380k lines overall)
    for (int i = 0; i < 20000; ++i) {
        QString line = randomLine(rng, p);
        if (rng() % 17 == 0)
            for (int k = 0; k < 40; ++k)
                line += QChar(randomUnit(rng));
        // mostly chain states like an editor, sometimes a garbage state
        int in = st;
        const unsigned dice = rng() % 20;
        if (dice == 0)
            in = int(rng());
        else if (dice == 1)
            in = 0;
        out.clear();
        const int o1 = highlightLine(lg, line, in, out);
        const QString err = checkSpans(line, out);
        if (!err.isEmpty()) {
            QString dump;
            for (QChar c : line)
                dump += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            QFAIL(qPrintable(QStringLiteral("%1: %2 (state %3) line: %4").arg(lang, err).arg(in).arg(dump)));
        }
        QVERIFY2(o1 >= 0, qPrintable(QStringLiteral("negative state %1 from %2").arg(o1).arg(in)));
        // determinism
        QVector<Span> again;
        QCOMPARE(highlightLine(lg, line, in, again), o1);
        QVERIFY(sameSpans(out, again));
        st = o1;
    }
}

void CodeTests::hugeLines_data()
{
    QTest::addColumn<QString>("lang");
    for (const QString &l : kAllLanguages)
        QTest::newRow(qPrintable(l)) << l;
}

void CodeTests::hugeLines()
{
    QFETCH(QString, lang);
    const Language *lg = languageFor(lang);
    // 1 MiB of random units, then 256 KiB of each pathological repetition (a
    // quadratic algorithm anywhere would take minutes here).
    std::mt19937 rng(99);
    {
        QString line;
        line.reserve(1 << 20);
        while (line.size() < (1 << 20))
            line += QChar(randomUnit(rng));
        QElapsedTimer t;
        t.start();
        QVector<Span> out;
        highlightLine(lg, line, 0, out);
        QVERIFY2(checkSpans(line, out).isEmpty(), qPrintable(lang));
        QVERIFY2(t.elapsed() < 3000, qPrintable(QStringLiteral("%1: random 1MiB line took %2 ms").arg(lang).arg(t.elapsed())));
    }
    const char *pieces[] = {"\"", "'", "`", "/", "/*", "*/", "\\", "${", "}", "{", "[", "]", "[=", "]=", "[=[", "]=]", "=", "<", ">", "</",
                            "<!--", "-->", "(", ")", "R\"x(", "r#\"", "\"#", "#", "$(", "<<", "<<E", "$", ":", "- ", "@", "a.b", "0x", "1e",
                            "1.", "..", "'a", "\"\"\"", "'''", "#[", "#\"", "$$", "--", "::", "\t", " ", "&", "%", "!", "?", "~", "|", "^",
                            "a", "A", "1", ";", ",", ".", "\xF0\x9F\x98\x80", "e\xCC\x81", "&amp", "&#", "<a ", "<script>", "<style>",
                            "</script>", "---", "...", "|\n", "key: ", "[[", "]]", "\"\"", "''", "``"};
    for (const char *pc : pieces) {
        const QString piece = QString::fromUtf8(pc);
        QString line;
        line.reserve(256 * 1024 + 8);
        while (line.size() < 256 * 1024)
            line += piece;
        for (int variant = 0; variant < 2; ++variant) {
            const QString text = variant == 0 ? line : (QStringLiteral("x ") + line + QStringLiteral(" y"));
            QElapsedTimer t;
            t.start();
            QVector<Span> out;
            const int o = highlightLine(lg, text, 0, out);
            QVERIFY(o >= 0);
            QVERIFY2(checkSpans(text, out).isEmpty(), qPrintable(QStringLiteral("%1 / [%2]").arg(lang, piece)));
            QVERIFY2(t.elapsed() < 3000, qPrintable(QStringLiteral("%1: 256KiB of [%2] took %3 ms").arg(lang, piece).arg(t.elapsed())));
        }
    }
}

void CodeTests::parallelHighlighting()
{
    // Language objects are immutable: highlighting from several threads must give
    // exactly the single-threaded result.
    for (const QString &l : {QStringLiteral("cpp"), QStringLiteral("python"), QStringLiteral("html"), QStringLiteral("bash")}) {
        const Language *lg = languageFor(l);
        const QStringList p = pool(l);
        std::mt19937 rng(5);
        QStringList lines;
        for (int i = 0; i < 400; ++i)
            lines << randomLine(rng, p);
        QVector<int> ref;
        fullState(lg, lines, &ref);
        std::atomic<int> bad{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 6; ++t)
            th.emplace_back([&] {
                for (int round = 0; round < 20; ++round) {
                    QVector<int> mine;
                    fullState(lg, lines, &mine);
                    if (mine != ref)
                        ++bad;
                }
            });
        for (auto &t : th)
            t.join();
        QCOMPARE(bad.load(), 0);
    }
}

// ===========================================================================
// benchmark
// ===========================================================================
namespace {

QString syntheticCpp(int targetChars)
{
    static const char *blocks[] = {
        "#include <vector>\n#include <string>\n#include \"core/Types.h\"\n\n",
        "namespace folia::core {\n\n",
        "// Computes the checksum of a note body. Returns 0 for an empty buffer.\n",
        "/* A multi-line comment that explains the invariants of the following\n"
        "   function: the buffer is never null and the length is in bytes. */\n",
        "static inline uint32_t checksum(const std::string &body, uint32_t seed = 0x811C9DC5u)\n{\n"
        "    uint32_t h = seed;\n"
        "    for (unsigned char c : body) {\n"
        "        h ^= c;\n"
        "        h *= 16777619u;  // FNV prime\n"
        "    }\n"
        "    return h;\n}\n\n",
        "template <typename T, size_t N>\nclass RingBuffer : public Base<T> {\npublic:\n"
        "    explicit RingBuffer(size_t cap = N) : cap_(cap), head_(0), tail_(0) {}\n"
        "    bool push(const T &v) noexcept { if (full()) return false; data_[tail_++ % N] = v; return true; }\n"
        "    std::optional<T> pop() { if (empty()) return std::nullopt; return data_[head_++ % N]; }\n"
        "private:\n    std::array<T, N> data_{};\n    size_t cap_, head_, tail_;\n};\n\n",
        "#define FOLIA_CHECK(expr) \\\n    do { if (!(expr)) { log(\"check failed: \" #expr); abort(); } } while (0)\n",
        "void Store::load(const QString &path, std::vector<Note> &out) {\n"
        "    QFile f(path);\n"
        "    if (!f.open(QIODevice::ReadOnly)) { qWarning() << \"cannot open\" << path << 3.14159 << 'x'; return; }\n"
        "    while (!f.atEnd()) {\n"
        "        auto line = f.readLine().trimmed();\n"
        "        if (line.startsWith('#')) continue;  // skip headers\n"
        "        out.push_back(Note{line.toStdString(), static_cast<int>(out.size()), 0x1F + 1'000});\n"
        "    }\n}\n\n",
        "const char *kQuery = R\"sql(SELECT id, title FROM notes\n WHERE body LIKE '%x%' ORDER BY updated DESC)sql\";\n",
        "enum class Mode : uint8_t { Read, Write, Append = 1u << 3 };\n",
        "} // namespace folia::core\n\n",
    };
    QString s;
    s.reserve(targetChars + 4096);
    int i = 0;
    while (s.size() < targetChars) {
        s += QString::fromUtf8(blocks[i % (sizeof(blocks) / sizeof(blocks[0]))]);
        ++i;
    }
    s.truncate(targetChars);
    return s;
}

QString syntheticPython(int targetChars)
{
    static const char *blocks[] = {
        "import os\nimport sys\nfrom dataclasses import dataclass, field\nfrom typing import Optional, List\n\n",
        "# Configuration constants\nMAX_RETRIES = 5\nTIMEOUT = 0.25  # seconds\n\n",
        "@dataclass(frozen=True)\nclass Note:\n    \"\"\"A single note.\n\n    Attributes are immutable; use `replace` to change them.\n    \"\"\"\n"
        "    title: str\n    body: str = ''\n    tags: List[str] = field(default_factory=list)\n\n",
        "    def render(self, width: int = 80) -> str:\n"
        "        lines = [f'{self.title!r}: {len(self.body)} chars']\n"
        "        for i, word in enumerate(self.body.split()):\n"
        "            if i % width == 0 and word.startswith(\"#\"):\n"
        "                lines.append(r'\\d+' + word.upper())\n"
        "        return '\\n'.join(lines)\n\n",
        "def parse(path: str, *args, **kwargs) -> Optional[Note]:\n"
        "    try:\n"
        "        with open(path, 'rb') as fh:\n"
        "            data = fh.read().decode('utf-8')\n"
        "    except (OSError, ValueError) as exc:\n"
        "        print(\"failed:\", exc, file=sys.stderr)\n"
        "        return None\n"
        "    return Note(title=os.path.basename(path), body=data, tags=[t for t in data.split() if t.isalpha()])\n\n",
        "if __name__ == '__main__':\n    total = sum(x ** 2 for x in range(1_000) if x & 0x1 == 0) / 3.5e2\n    print(total, 0b1010, 1j, None, True)\n\n",
        "x = '''multi\nline string'''\n",
    };
    QString s;
    s.reserve(targetChars + 4096);
    int i = 0;
    while (s.size() < targetChars) {
        s += QString::fromUtf8(blocks[i % (sizeof(blocks) / sizeof(blocks[0]))]);
        ++i;
    }
    s.truncate(targetChars);
    return s;
}

struct BenchResult {
    double seconds = 0;
    qint64 chars = 0;
    qint64 lines = 0;
    qint64 spans = 0;
    double p50us = 0, p95us = 0, p99us = 0, maxus = 0, meanus = 0;
};

// Whole-file pass without per-line timers (what an editor pays on load).
BenchResult runBench(const Language *lg, const QStringList &lines)
{
    using clock = std::chrono::steady_clock;
    BenchResult r;
    QVector<Span> out;
    out.reserve(1 << 12);
    int st = 0;
    const auto t0 = clock::now();
    for (const QString &ln : lines) {
        out.clear();
        st = highlightLine(lg, ln, st, out);
        r.chars += ln.size() + 1; // + the newline
        r.spans += out.size();
    }
    r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    r.lines = lines.size();
    return r;
}

// Per-line latency. The steady clock ticks at ~41.7 ns on Apple silicon, so each
// line is timed as the mean of kRep back-to-back calls (same input, same state)
// to get below the tick; spans of the previous call are discarded each time.
void perLineStats(const Language *lg, const QStringList &lines, BenchResult &r)
{
    using clock = std::chrono::steady_clock;
    constexpr int kRep = 16;
    std::vector<double> perLine;
    perLine.reserve(size_t(lines.size()));
    QVector<Span> out;
    out.reserve(1 << 12);
    int st = 0;
    for (const QString &ln : lines) {
        int next = 0;
        const auto a = clock::now();
        for (int k = 0; k < kRep; ++k) {
            out.clear();
            next = highlightLine(lg, ln, st, out);
        }
        const auto b = clock::now();
        perLine.push_back(std::chrono::duration<double, std::micro>(b - a).count() / kRep);
        st = next;
    }
    std::sort(perLine.begin(), perLine.end());
    auto pct = [&](double q) { return perLine[size_t(q * double(perLine.size() - 1))]; };
    r.p50us = pct(0.50);
    r.p95us = pct(0.95);
    r.p99us = pct(0.99);
    r.maxus = perLine.back();
    double sum = 0;
    for (double v : perLine)
        sum += v;
    r.meanus = sum / double(perLine.size());
}

double clockTickUs()
{
    using clock = std::chrono::steady_clock;
    double best = 1e9;
    for (int i = 0; i < 20000; ++i) {
        const auto a = clock::now();
        auto b = clock::now();
        while (b == a)
            b = clock::now();
        best = std::min(best, std::chrono::duration<double, std::micro>(b - a).count());
    }
    return best;
}

double timerOverheadUs()
{
    using clock = std::chrono::steady_clock;
    const int n = 200000;
    const auto t0 = clock::now();
    double sink = 0;
    for (int i = 0; i < n; ++i) {
        const auto a = clock::now();
        const auto b = clock::now();
        sink += std::chrono::duration<double, std::micro>(b - a).count();
    }
    (void)t0;
    return sink / n;
}

} // namespace

void CodeTests::benchmark()
{
    const double ovh = timerOverheadUs();
    std::fprintf(stdout, "\n=== folia_code throughput (build: %s) ===\n",
#ifdef NDEBUG
                 "release"
#else
                 "NOT release (numbers are not representative)"
#endif
    );
    std::fprintf(stdout, "steady_clock: tick %.4f us, now() call %.4f us\n", clockTickUs(), ovh);

    struct Item {
        const char *name;
        const char *lang;
        QString text;
    };
    const int target = 1 << 20; // 1 MiB of characters
    Item items[] = {{"C++ 1 MiB", "cpp", syntheticCpp(target)}, {"Python 1 MiB", "python", syntheticPython(target)}};
    for (Item &it : items) {
        const Language *lg = languageFor(QString::fromLatin1(it.lang));
        const QStringList lines = it.text.split(QLatin1Char('\n'));
        runBench(lg, lines); // warm-up (caches, branch predictors)
        BenchResult best;
        best.seconds = 1e9;
        BenchResult all[5];
        for (int rep = 0; rep < 5; ++rep) {
            all[rep] = runBench(lg, lines);
            if (all[rep].seconds < best.seconds)
                best = all[rep];
        }
        std::vector<double> secs;
        for (const auto &r : all)
            secs.push_back(r.seconds);
        std::sort(secs.begin(), secs.end());
        const double median = secs[2];
        std::fprintf(stdout, "[%s] %lld chars, %lld lines, %lld spans\n", it.name, (long long)best.chars, (long long)best.lines,
                     (long long)best.spans);
        std::fprintf(stdout, "    throughput: best %.1f MB/s (%.2f ms), median %.1f MB/s (%.2f ms)  [MB = 2^20 source chars]\n",
                     double(best.chars) / (1 << 20) / best.seconds, best.seconds * 1e3, double(best.chars) / (1 << 20) / median,
                     median * 1e3);
        perLineStats(lg, lines, best);
        std::fprintf(stdout,
                     "    per line (us, mean of 16 back-to-back calls): mean %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.2f   (avg %.1f chars/line)\n",
                     best.meanus, best.p50us, best.p95us, best.p99us, best.maxus, double(best.chars) / double(best.lines));
        QVERIFY(best.spans > 0);

        // Visible-window cost: carry states up to line K (what an editor must do
        // once per document open / jump), then highlight 60 visible lines.
        const int K = int(lines.size() * 0.9);
        using clock = std::chrono::steady_clock;
        QVector<int> carry;
        carry.reserve(lines.size());
        QVector<Span> out;
        out.reserve(4096);
        const auto c0 = clock::now();
        int st = 0;
        for (int i = 0; i < K; ++i) {
            out.clear();
            carry.append(st);
            st = highlightLine(lg, lines[i], st, out);
        }
        const double carryMs = std::chrono::duration<double, std::milli>(clock::now() - c0).count();
        std::vector<double> win;
        double first = 0;
        for (int rep = 0; rep < 300; ++rep) {
            const auto w0 = clock::now();
            int s2 = st;
            for (int i = K; i < K + 60 && i < lines.size(); ++i) {
                out.clear();
                s2 = highlightLine(lg, lines[i], s2, out);
            }
            const double us = std::chrono::duration<double, std::micro>(clock::now() - w0).count();
            if (rep == 0)
                first = us;
            win.push_back(us);
        }
        std::sort(win.begin(), win.end());
        std::fprintf(stdout,
                     "    carry states for first %d lines (cold, once): %.2f ms | then 60 visible lines: first %.1f us, median %.1f us, min %.1f us\n",
                     K, carryMs, first, win[win.size() / 2], win.front());
    }
    std::fflush(stdout);
}

QTEST_GUILESS_MAIN(CodeTests)
#include "code_tests.moc"
