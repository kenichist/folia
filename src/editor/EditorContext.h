// State shared by every open page's layout: typography/colors (set by the
// shell theme), the preview service and page-title lookup for link chips.
#pragma once

#include "code/Highlighter.h"
#include "core/Api.h"

#include <QColor>
#include <QFont>
#include <QObject>
#include <functional>

namespace folia {

class AttachmentStore;
class MediaService;

struct EditorTheme {
    QFont body;
    QFont mono;
    bool monoLigatures = false;    // theme `mono.ligatures`: always shape code text
    qreal h1Scale = 1.75;
    qreal h2Scale = 1.4;
    qreal h3Scale = 1.18;
    qreal lineSpacing = 1.0;       // extra line height factor (>= 1)
    qreal paragraphSpacing = 7;    // px after a paragraph line
    qreal maxColumnWidth = 760;    // readable measure; 0 = full width
    qreal sideMargin = 32;
    qreal topMargin = 18;
    QColor background{255, 255, 255};
    QColor text{28, 28, 30};
    QColor dim{142, 142, 147};
    QColor accent{0, 102, 204};
    QColor codeBackground{245, 245, 247};
    QColor inlineCodeBackground{238, 238, 241};
    QColor mathSource{120, 70, 160};
    QColor mathError{196, 43, 28};
    QColor linkBackground{238, 243, 252};
    QColor selection{179, 215, 255};
    QColor tokens[int(code::Token::Count)];

    EditorTheme();
    static EditorTheme light();
    static EditorTheme dark();
};

class EditorContext : public QObject {
    Q_OBJECT
public:
    explicit EditorContext(QObject *parent = nullptr) : QObject(parent) { deriveFonts(); }

    const EditorTheme &theme() const { return m_theme; }
    void setTheme(const EditorTheme &t)
    {
        m_theme = t;
        deriveFonts();
        ++m_serial;
        emit themeChanged();
    }
    quint64 themeSerial() const { return m_serial; }

    // The code font for a run of text. ASCII in a fixed-pitch font without
    // ligatures has nothing to shape (no kerning, no contextual forms), so it
    // is laid out with QFont::PreferNoShaping: Qt then maps characters
    // straight to glyphs, and the font's layout tables are never copied into
    // the heap for HarfBuzz. Anything else (other scripts, emoji sequences, a
    // proportional font picked as the code font) keeps full shaping, as does
    // everything when the theme asks for ligatures.
    const QFont &codeFont(bool ascii) const;
    static bool isAscii(QStringView text)
    {
        for (const QChar c : text)
            if (c.unicode() > 0x7e)
                return false;
        return true;
    }

    IPreviewService *previews = nullptr;
    AttachmentStore *attachments = nullptr; // image metadata, imports, pins
    MediaService *media = nullptr;          // decoded bitmaps (budgeted cache + scheduler)
    std::function<QString(PageId)> titleOf;
    bool highlightEnabled = true;
    // Benchmarks only (A/B, docs/BENCHMARKS.md §6d).
    // Repaint as before 2026-11-10: everything below an edited line, the
    // whole viewport for the formula bubble and after a height correction,
    // and every line in the update region's bounding rect.
    bool wideRepaints = false;
    // Shape ASCII code text like any other text.
    bool shapeCode = false;

signals:
    void themeChanged();
    void pageTitleChanged(folia::PageId id);

private:
    void deriveFonts();

    EditorTheme m_theme;
    QFont m_monoUnshaped;
    // Whether the code font is fixed pitch: asked once per theme, when code
    // is first laid out (it loads the font).
    mutable bool m_monoChecked = false;
    mutable bool m_monoFixed = false;
    quint64 m_serial = 1;
};

} // namespace folia
