// Per-QTextBlock (one editor line) state. Owned by the QTextDocument; deleted
// when the line is removed. Identity rules (docs/ARCHITECTURE.md §2):
//  * Splitting a line keeps the id on the part before the split (Qt keeps the
//    user data on the original block); joining keeps the first line's id.
//  * A stored block (segment) takes the id of its first line: a paragraph is
//    one line; a code/math block spans fence..fence and uses the opener's id.
#pragma once

#include "core/Types.h"

#include <QTextBlockUserData>
#include <QTextFormat>
#include <memory>

namespace folia {

class MathDisplayList;
namespace code { class Language; }

// Line roles derived from text (fence state machine). Stored in
// QTextBlock::userState() so propagation can compare old vs new cheaply.
enum class LineRole : int {
    Unknown = -1,
    Para = 0,
    CodeOpen = 1,
    CodeLine = 2,
    CodeClose = 3,
    MathOpen = 4,
    MathLine = 5,
    MathClose = 6,
    MathSingle = 7,
    PageLink = 8,
};

inline bool roleInCode(LineRole r) { return r == LineRole::CodeOpen || r == LineRole::CodeLine; }
inline bool roleInMath(LineRole r) { return r == LineRole::MathOpen || r == LineRole::MathLine; }
inline bool roleIsCodeSegment(LineRole r)
{
    return r == LineRole::CodeOpen || r == LineRole::CodeLine || r == LineRole::CodeClose;
}
inline bool roleIsMathSegment(LineRole r)
{
    return r == LineRole::MathOpen || r == LineRole::MathLine || r == LineRole::MathClose;
}

// Page links are a single U+FFFC object character carrying this format.
constexpr int kPageLinkObjectType = QTextFormat::UserObject + 1;
constexpr int kPageLinkTargetProperty = QTextFormat::UserProperty + 1;
// Marker on formats the layout adds (so IME preedit formats are preserved).
constexpr int kLayoutFormatMarker = QTextFormat::UserProperty + 2;

struct BlockTracker; // per-document bookkeeping, see PageSession

class BlockData : public QTextBlockUserData {
public:
    BlockData(BlockId id, BlockTracker *tracker);
    ~BlockData() override;

    BlockId id = 0;
    bool dirty = true;

    // Syntax highlight carry state (code lines).
    const code::Language *lang = nullptr;
    int hlIn = 0;
    int hlOut = 0;
    bool hlValid = false;

    // Display math preview (on MathClose / MathSingle lines).
    quint64 previewHash = 0;           // hash of the source currently wanted
    std::shared_ptr<const MathDisplayList> preview;
    QString previewError;
    bool previewPending = false;
    float previewHeight = 0;

    BlockTracker *tracker = nullptr;
};

struct BlockTracker {
    bool structureChanged = false;
    qint64 liveBlocks = 0;
    bool detached = false; // document being torn down: ignore deletions
};

} // namespace folia
