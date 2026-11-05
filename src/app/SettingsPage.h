// Settings page (File ▸ Settings…, Mod+,): shown inside the main window in
// place of the layout, with a section list (General, Appearance, Editor,
// Layout) and a way back to the notes (the back button, Esc or Mod+W). It
// is native Qt and does not depend on the Lua layout, so a broken or unusual
// layout can never hide or break it. Appearance and editor values are
// overrides layered over the active layout's theme
// (AppController::setThemeOverrides); the layout file itself is never
// edited. Created when opened and destroyed when closed.
#pragma once

#include <QComboBox>
#include <QTimer>
#include <QVariantMap>
#include <QWidget>
#include <functional>
#include <vector>

class QCheckBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace folia {

class AppController;

// Font family list filled on first popup: enumerating every installed family
// costs time and memory, and most visits never open the list. Names are
// drawn in the UI font (no per-family previews, which would load every font).
class FontFamilyCombo : public QComboBox {
public:
    explicit FontFamilyCombo(bool monospaceOnly, QWidget *parent = nullptr);
    void setDefaultLabel(const QString &label); // item 0 = "use the layout's font"
    void setFamily(const QString &family);      // empty selects item 0
    QString family() const;
    bool populated() const { return m_populated; }
    qint64 populate(); // microseconds spent; 0 when already filled
    void showPopup() override;

private:
    bool m_mono;
    bool m_populated = false;
};

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    SettingsPage(AppController *app, QWidget *parent);

    int sectionCount() const;
    int section() const;
    void setSection(int index);

    // Bench/test hooks.
    int paintCount() const { return m_paints; } // the page's or its section list's paints
    qint64 populateFontLists();
    void applyPending(); // apply a throttled change now

protected:
    void paintEvent(QPaintEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    struct Reset {
        QToolButton *button;
        QString path;
    };

    QWidget *buildGeneral();
    QWidget *buildAppearance();
    QWidget *buildEditor();
    QWidget *buildLayout();
    QWidget *withReset(QWidget *control, const QString &path, const QString &what);
    void addRow(QFormLayout *form, const QString &label, QWidget *control, QWidget *row = nullptr);
    QWidget *doubleSpin(const QString &path, double min, double max, double step, int decimals,
                        const QString &suffix, std::function<double()> effective, const QString &what);
    QWidget *intSpin(const QString &path, int min, int max, int step, const QString &suffix,
                     std::function<double()> effective, const QString &what, const QString &special = {});
    QVariant over(const QString &path) const;
    void setOver(const QString &path, const QVariant &value);
    void clearOver(const QStringList &paths);
    void scheduleApply();
    void refresh();
    void refreshLayout();
    void pickAccent();
    void confirmResetLayout();
    void runCommand(const QString &name, const QVariantMap &args = {});

    AppController *m_app;
    QVariantMap m_over; // the page's copy of the overrides (source of truth while open)
    QTimer m_applyTimer;
    bool m_refreshing = false;
    int m_paints = 0;
    std::vector<std::function<void()>> m_refreshers;
    std::vector<Reset> m_resets;

    QToolButton *m_back = nullptr;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_sections = nullptr;
    QComboBox *m_mode = nullptr;
    QLabel *m_modeNote = nullptr;
    QToolButton *m_accent = nullptr;
    FontFamilyCombo *m_bodyFamily = nullptr;
    FontFamilyCombo *m_monoFamily = nullptr;
    QPushButton *m_resetAppearance = nullptr;
    QPushButton *m_resetEditor = nullptr;
    QCheckBox *m_math = nullptr;
    QLabel *m_mathNote = nullptr;
    QComboBox *m_layout = nullptr;
    QLabel *m_layoutPath = nullptr;
    QLabel *m_layoutStatus = nullptr;
    QCheckBox *m_live = nullptr;
};

} // namespace folia
