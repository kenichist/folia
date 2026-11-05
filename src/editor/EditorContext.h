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

struct EditorTheme {
    QFont body;
    QFont mono;
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
    explicit EditorContext(QObject *parent = nullptr) : QObject(parent) {}

    const EditorTheme &theme() const { return m_theme; }
    void setTheme(const EditorTheme &t)
    {
        m_theme = t;
        ++m_serial;
        emit themeChanged();
    }
    quint64 themeSerial() const { return m_serial; }

    IPreviewService *previews = nullptr;
    std::function<QString(PageId)> titleOf;
    bool highlightEnabled = true;

signals:
    void themeChanged();
    void pageTitleChanged(folia::PageId id);

private:
    EditorTheme m_theme;
    quint64 m_serial = 1;
};

} // namespace folia
