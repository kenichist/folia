// Lazy, line-incremental syntax highlighting for code blocks.
//
// Contract (frozen v1):
//  * Tokenization is per line with an integer carry state, so the editor can
//    re-highlight only the edited line and propagate forward only while the
//    outgoing state changes ("until it stabilizes").
//  * Language tables are constructed on first use of that language
//    (languageFor() is the only way in); unknown names return nullptr and the
//    editor falls back to plain text.
//  * Thread-safe: languageFor() may be called from any thread; Language
//    objects are immutable after construction and never freed.
//  * highlightLine() is bounded: work is linear in the line length and it
//    never allocates beyond `out`.
// The language set is documented in docs/CODE_LANGUAGES.md.
#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

namespace folia::code {

enum class Token : quint8 {
    Plain = 0,
    Keyword,
    Type,
    Builtin,
    Function,
    String,
    Number,
    Comment,
    Operator,
    Punctuation,
    Preprocessor,
    Variable,
    Constant,
    Tag,
    Attribute,
    Count
};

struct Span {
    int start = 0;
    int length = 0;
    Token token = Token::Plain;
};

class Language;

// Accepts canonical names and common aliases ("c++", "py", "sh", "js"...).
// Case-insensitive. Returns nullptr for unknown/empty names.
const Language *languageFor(QStringView name);

// Canonical names of all shipped languages, sorted.
QStringList supportedLanguages();
QString canonicalName(const Language *lang);

// Number of languages whose tables have been constructed so far (for
// instrumentation: proves lazy loading).
int loadedLanguageCount();

// Tokenize one line. `stateIn` is the carry state from the previous line of
// the same code block (0 at the first line). Appends spans (sorted, non
// overlapping; uncovered characters are Plain) to `out` and returns the
// outgoing state. `lang == nullptr` produces no spans and returns 0.
int highlightLine(const Language *lang, QStringView line, int stateIn, QVector<Span> &out);

} // namespace folia::code
