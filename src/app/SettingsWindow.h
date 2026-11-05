// Settings window (File ▸ Settings…, Mod+,). A native window that does not
// depend on the Lua layout, so a broken or unusual layout can never hide or
// break it. Appearance and editor values are overrides layered over the
// active layout's theme (AppController::setThemeOverrides); the layout file
// itself is never edited. Created on the first open; closing hides it and it
// does no work while hidden (see the constructor for why it is not deleted).
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
class QPushButton;
class QTabWidget;
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

class SettingsWindow : public QWidget {
    Q_OBJECT
public:
    SettingsWindow(AppController *app, QWidget *parent);

    // Hide, keeping the native window (title-bar close, Mod+W and Esc do this).
    void dismiss();

    // Bench/test hooks.
    int paintCount() const { return m_paints; }
    qint64 populateFontLists();
    void applyPending(); // apply a throttled change now
    QTabWidget *tabs() const { return m_tabs; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
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
    QVariantMap m_over; // the window's copy of the overrides (source of truth while open)
    QTimer m_applyTimer;
    bool m_refreshing = false;
    int m_paints = 0;
    std::vector<std::function<void()>> m_refreshers;
    std::vector<Reset> m_resets;

    QTabWidget *m_tabs = nullptr;
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
