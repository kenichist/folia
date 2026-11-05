# Code block highlighting: languages and design

Library: `folia_code` (static, links only `Qt6::Core`). Public contract:
`src/code/Highlighter.h`. Tests: `tests/code/code_tests.cpp`.

```cpp
const Language *lang = folia::code::languageFor(u"py");            // nullptr => plain text
int state = 0;                                                     // 0 at the first line of a block
QVector<Span> spans;
state = folia::code::highlightLine(lang, line, state, spans);      // returns the state for the next line
```

Editor protocol: re-highlight the edited line with the previous line's outgoing
state, then keep going down the block only while a line's outgoing state differs
from the one stored last time. Highlighting is a pure function of
`(language, line text, incoming state)`, so the moment an outgoing state equals
the stored one, everything below is already correct.

## Supported languages

19 languages. Names are matched case-insensitively after trimming blanks; anything
else (including the empty string) returns `nullptr`.

| Canonical | Aliases | State carried across lines |
|---|---|---|
| `bash` | `sh` `zsh` `shell` `console` `shellscript` `ksh` `shellsession` | open `"..."`, `'...'`, `$'...'`; heredoc body (delimiter fingerprint, `<<` and `<<-`, quoted or bare delimiter); trailing-backslash continuation |
| `c` | `h` | `/* */`; `//` comment and `#define` line ending in `\`; string ending in `\` |
| `cpp` | `c++` `cc` `cxx` `hpp` `hh` `hxx` `h++` `cplusplus` | as `c`, plus raw strings `R"delim( ... )delim"` |
| `csharp` | `cs` `c#` `dotnet` | `/* */`; verbatim `@"..."` (multi-line, `""` escape); raw `"""..."""` |
| `css` | | `/* */`; block nesting for `@media`/`@supports`/`@layer`/`@container`...; a declaration value continued on the next line |
| `diff` | `patch` `udiff` | none (stateless, state is always 0) |
| `go` | `golang` | `/* */`; raw strings `` `...` `` |
| `html` | `htm` `xml` `svg` `xhtml` `xsl` `xslt` `plist` `rss` `atom` | `<!-- -->`; a tag or a quoted attribute value spanning lines; `<![CDATA[ ]]>`; `<!DOCTYPE>` / `<? ?>` spanning lines; `<script>` / `<style>` bodies (lexed by the javascript / css lexers, their state rides in the payload) |
| `java` | | `/* */`; text blocks `"""..."""` |
| `javascript` | `js` `jsx` `mjs` `cjs` `node` `nodejs` | `/* */`; template literals including nested `${ ... `...${ }... }` (a bit-stack of open `${`/`{` contexts) |
| `json` | `jsonc` `json5` | `/* */` (comments are tolerated) |
| `lua` | | long strings and comments `[==[ ]==]` / `--[==[ ]==]` (level in the state); `\z` / `\`-newline in a string |
| `python` | `py` `py3` `python3` `pyw` | `"""` and `'''` strings (both kinds are distinct states); string continued by `\` |
| `rust` | `rs` | nested `/* /* */ */` (depth); raw strings `r#"..."#` (up to 255 hashes, also `br`/`cr`); ordinary `"..."` strings, which may span lines |
| `sql` | `mysql` `postgres` `postgresql` `pgsql` `plsql` `tsql` `sqlite` | `/* */`; `'...'` strings (may span lines, `''` escape); `$$ ... $$` and `$tag$ ... $tag$` (tag fingerprint) |
| `swift` | | nested `/* */` (depth); `"""` strings; raw `#"..."#`, `#"""..."""#` (hash count) |
| `toml` | | `"""` and `'''` strings; bracket depth of multi-line arrays |
| `typescript` | `ts` `tsx` `mts` `cts` | as `javascript` |
| `yaml` | `yml` | `\|` and `>` block scalars (parent indentation column) |

`supportedLanguages()` returns these 19 names sorted; `canonicalName(lang)` maps a
`Language*` back to its canonical name.

## Token conventions

Spans are sorted, non-overlapping, non-empty, inside `[0, line.size())`, and never
split a UTF-16 surrogate pair. Uncovered characters are `Plain`; no `Plain` span is
ever emitted. Adjacent spans with the same token are merged (`);` is one
`Punctuation` span).

| Token | Used for |
|---|---|
| `Keyword` | language keywords; shell keywords and `[[`; CSS at-rules and `!important`; diff `---`/`+++` file lines |
| `Type` | primitive and declared types; names after `class`/`struct`/`enum`/`interface`/`trait`/`type`/`new`/...; CamelCase identifiers; Rust lifetimes; CSS `.class`; YAML `!tags` |
| `Builtin` | standard library names (`printf`, `len`, `console`, `echo`...); CSS pseudo-classes/elements |
| `Function` | identifier directly followed by `(`; the name after `def`/`fn`/`func`/`function`; Rust `name!(`; shell command names; CSS function names; the context text of a diff `@@` line |
| `String` | strings, chars, raw/multi-line strings, JS regex literals, template text, shell heredoc bodies, YAML block scalar text, CDATA, HTML attribute values, diff added lines |
| `Number` | decimal, `0x` `0b` `0o`, floats, exponents, `_` and `'` separators, suffixes (`u8`, `UL`, `f`, `n`, `j`, `m`), CSS dimensions and `#hex` colors, TOML dates |
| `Comment` | comments (also diff `\ No newline`, git header lines) |
| `Operator` | operator characters, `=` in key/value formats |
| `Punctuation` | brackets, commas, semicolons, dots, `<` `>` `/>` of tags |
| `Preprocessor` | C/C++/C# `#directive`, Swift `#if`, HTML doctype and `<? ?>`, YAML `%DIRECTIVE`, diff `@@ ... @@` |
| `Variable` | shell `$VAR` `${...}` and `NAME=`; SQL `:name` `$1` `@v` and `` `quoted` ``; Swift `$0`; CSS `--custom-property`; YAML `&anchor` `*alias` |
| `Constant` | `true`/`false`/`null`/`nil`/`None`...; ALL_CAPS identifiers; CSS value keywords and `#id`; HTML entities; YAML `true`/`~`; heredoc delimiter |
| `Tag` | HTML/XML tag names; CSS element selectors; TOML `[table]` headers; diff removed lines |
| `Attribute` | decorators/annotations/attributes (`@dataclass`, `@Override`, `#[derive]`, `[[nodiscard]]`); HTML attribute names; JSON/YAML/TOML keys; CSS property names and `[attr]` selectors; shell options (`-x`, `--long`) |

Other conventions: after `.` a word is never a keyword (`obj.delete(x)` is a
call); soft keywords (`type`, `of`, `from`, `as`, `async`, `readonly`, Python
`match`/`case`...) are keywords only where the following token makes that
plausible, so `{ type: "x" }` stays plain; an identifier is a `Constant` when it
is ALL_CAPS and a `Type` when it is CamelCase (ALL_CAPS: c, cpp, csharp, java, rust,
swift, python, javascript, typescript; CamelCase: the same minus c, plus go).

## Carry states

An `int`: low 4 bits are a mode, the rest is the payload; `0` means "outside any
multi-line construct". States are canonical (the same lexical situation always
yields the same integer, so stabilization is detected as early as possible) and
always non-negative. Examples: a C block comment is `1`; a Python `"""` string
and a Python `'''` string differ; a Rust `r##"` string differs from `r#"`;
a Lua level-2 long bracket differs from level-0. Delimiters that do not fit are
stored as a 22-bit fingerprint plus a length (C++ raw strings, SQL dollar quotes,
shell heredocs), so a collision can only ever mis-detect the end of such a
string, never crash. An incoming state a language does not understand (negative,
huge, from another language) is treated as best effort and never crashes.

## Laziness and threading

* `languageFor()` resolves aliases without touching any table (stack buffer, no
  allocation). The keyword table of a language is built by the first call that
  resolves to it (`std::call_once` per language; the fast path is one acquire
  load). `loadedLanguageCount()` counts the built languages.
* `html` builds `javascript` / `css` only when it meets a `<script>` / `<style>`
  body.
* `Language` objects are immutable and intentionally never freed;
  `highlightLine()` uses no global mutable state and can run on any number of
  threads concurrently.

## Complexity and limits

`highlightLine` is a single left-to-right pass: O(line length) time, no
allocation other than appending to `out` (verified by an allocation-counting test
over all languages). Every look-ahead is bounded or is made at most once per line
(JS regex scan, C++ `[[` attribute scan, unmatched `${`/`$(`...). The test suite
feeds every language 1 MiB of random UTF-16 and 256 KiB of ~80 pathological
repetitions (`"`, `/*`, `[=[`, `<<E`, `${`, `<!--`...); the slowest case measured
for a 1 MiB line was about 16 ms. Depth counters are clamped
(block-comment nesting 2^20, template/CSS stacks about 20 levels, TOML depth 2^20).

## Known gaps (best effort)

* Heredocs: only the first heredoc of a line is tracked; `<<` inside `$(( ))` is
  not a heredoc; a body is plain string text (no `$var` highlighting).
* Shell `console` sessions: `$ cmd` prompts are recognised, but prompt output text
  is lexed as shell, so an apostrophe in program output opens a string.
  `$(...)` inside double quotes is not highlighted as a command.
* Template literals nest up to about 20 levels of `${`/`{`; deeper levels stop
  returning to the outer template.
* JS/TS: regex-versus-division is a heuristic on the previous token (`) /re/`
  and `} /re/` are read as division); JSX / TSX markup is not lexed; there are no
  string-interpolation sub-spans in Python f-strings, Swift `\( )` or C# `$"{}"`.
* C++ raw strings: delimiters longer than 16 characters, or containing
  characters the standard forbids, are not raw strings. Rust raw strings with more
  than 255 hashes close at 255.
* Rust: multi-line attributes `#[...]` are only coloured on their first line;
  `'a'` versus a lifetime uses a bounded look-ahead.
* Python soft keywords `match`/`case` need the line to end with `:`; `type` is
  only ever a type name, never a statement.
* CSS: no nesting-aware selector/declaration disambiguation inside declaration
  blocks (`&:hover {` is read as a property); `url()` bodies are plain strings.
* HTML: unknown raw-text elements (`<textarea>`, `<pre>`, `<title>`) are lexed as
  normal markup; `<script type="...">` of any type is lexed as JavaScript.
* YAML: multi-line plain scalars, multi-line quoted scalars and multi-line flow
  collections are lexed line by line (a `: ` inside a continuation line looks
  like a key). A diff line starting with `--- ` is a file header even when it is a
  removed line whose text starts with `-- `.
* TOML: `"""` closed by more than three quotes (`""""`) is not special-cased.
* Identifiers: every non-ASCII code unit is an identifier character (no Unicode
  category tables).

## Measured performance

Release build (`-O3`, Apple M1, AppleClang 21, Qt 6.12), `code_tests benchmark`,
1 MiB (2^20 characters) synthetic C++ and Python sources, warm caches, best of
five whole-file passes (`MB/s` = 2^20 source characters per second):

| Source | MB/s | 1 MiB pass | per line, mean / p50 / p95 / p99 | 60 visible lines |
|---|---|---|---|---|
| C++ (29 728 lines, 35 chars/line) | 300-320 | 3.1-3.3 ms | 0.105 / 0.055 / 0.34 / 0.42 us | 6.7 us median (8 us first) |
| Python (35 652 lines, 29 chars/line) | 295-300 | 3.3-3.4 ms | 0.091 / 0.055 / 0.27 / 0.42 us | 5.9 us median (7 us first) |

The 60 visible lines are measured after the carry states of the first 90% of the
file have been computed (about 3 ms once, for either file); per-line figures are
the mean of 16 back-to-back calls per line because the monotonic clock ticks at
41.7 ns. As a desynchronisation check, about 1 900 C/C++ headers (libc++, Qt, Lua,
SQLite, the macOS SDK), 800 CPython modules, 600 JS files, 300 Go files, 300 Lua
files, 100 Swift interface files and the available CSS/HTML/SQL/Java/C#/JSON/shell
files all end in state `0` when run top to bottom (YAML files may legitimately end
inside a block scalar; deliberately invalid TOML fixtures end unterminated), at
130-600 MB/s depending on the language.
`libfolia_code.a` text is about 72 KiB (`size`: `__TEXT` 73 409 bytes over the six
objects).

## Adding a language

1. Add the id to `enum class Lang` (alphabetical) and the canonical name and
   aliases in `Highlighter.cpp`.
2. In `Languages.cpp` add a `setupXxx()` filling `Features` and the keyword lists
   (`word:T` marks "next identifier is a type", `word:F` "...a function", `word~`
   a soft keyword), or point `lex` at a new lexer.
3. Add token expectations, multi-line state cases and a pool of line fragments
   (`pool()`) to `tests/code/code_tests.cpp`; the fuzz, Unicode, stabilization,
   no-allocation and huge-line tests then cover it automatically.
