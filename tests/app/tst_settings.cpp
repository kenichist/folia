// Settings tests (integrator-owned): a real AppController on a temporary
// data folder, driven through the native menu, the command registry and the
// Settings page's own controls; plus the dark-mode repaint regression.

#include "app/AppController.h"
#include "app/Preferences.h"
#include "app/SettingsPage.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "shell/CommandRegistry.h"
#include "shell/ShellHost.h"
#include "storage/Store.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QShortcut>
#include <memory>

using namespace folia;

namespace {

const bool s_offscreen = [] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

std::unique_ptr<AppController> launch(const QString &home)
{
    AppController::Options o;
    o.homeDir = home;
    auto ctl = std::make_unique<AppController>(o);
    QString err;
    if (!ctl->start(&err))
        qFatal("AppController::start failed: %s", qPrintable(err));
    ctl->window()->show();
    return ctl;
}

QAction *preferencesAction(QMainWindow *w)
{
    for (QAction *a : w->menuBar()->findChildren<QAction *>())
        if (a->menuRole() == QAction::PreferencesRole)
            return a;
    return nullptr;
}

// What a click on a combo item does (activated is user-only).
void choose(QComboBox *c, const QVariant &data)
{
    const int i = c->findData(data);
    QVERIFY2(i >= 0, qPrintable(data.toString()));
    c->setCurrentIndex(i);
    emit c->activated(i);
}

template <typename T>
T *control(SettingsPage *w, const char *name)
{
    T *c = w->findChild<T *>(QString::fromLatin1(name));
    if (!c)
        qFatal("no control %s", name);
    return c;
}

} // namespace

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    void menuOpensSettingsInsideMainWindow()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        QVERIFY(!app->settingsPage()); // nothing exists until asked for
        const auto windows = [] {
            int n = 0;
            for (QWidget *w : QApplication::topLevelWidgets())
                n += w->isVisible() ? 1 : 0;
            return n;
        };
        const int before = windows();
        const QString title = app->window()->windowTitle();
        QShortcut *newPage = nullptr; // a layout binding (default preset: Mod+N)
        for (QShortcut *sc : app->window()->findChildren<QShortcut *>())
            if (sc->key() == QKeySequence(QStringLiteral("Ctrl+N")))
                newPage = sc;
        QVERIFY(newPage && newPage->isEnabled());

        QAction *a = preferencesAction(app->window());
        QVERIFY(a);
        QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("Ctrl+,")));
        a->trigger();
        QTRY_VERIFY(app->settingsOpen());
        SettingsPage *page = app->settingsPage();
        QVERIFY(page->isVisible());
        QCOMPARE(page->window(), static_cast<QWidget *>(app->window())); // inside the main window
        QCOMPARE(windows(), before);                                     // no new window
        QVERIFY(!app->editorPane()->isVisible());                        // it covers the layout
        QCOMPARE(app->window()->windowTitle(), QStringLiteral("Settings — Folia"));
        QVERIFY(!newPage->isEnabled()); // layout keys would act on the hidden page
        QVERIFY(!app->commands()->dispatchCommand(QStringLiteral("editor.undo"), {}).ok);
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("app.settings"), {}).ok);
        QTest::qWait(20);
        QCOMPARE(app->settingsPage(), page); // focused, not duplicated

        QTest::keyClick(page, Qt::Key_Escape); // back to the notes
        QVERIFY(!app->settingsOpen());
        QTRY_VERIFY(!app->settingsPage()); // deleted while closed
        QVERIFY(app->editorPane()->isVisible());
        QVERIFY(newPage->isEnabled());
        QCOMPARE(app->window()->windowTitle(), title);

        app->showSettings();
        control<QToolButton>(app->settingsPage(), "settings.back")->click();
        QVERIFY(!app->settingsOpen());

        // Opening or creating a page (menu, command, link) returns to the notes.
        app->showSettings();
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("page.new"), {}).ok);
        QTRY_VERIFY(!app->settingsOpen());
        QVERIFY(app->editorPane()->isVisible());
    }

    void sectionsAreListed()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        app->showSettings();
        SettingsPage *page = app->settingsPage();
        QCOMPARE(page->sectionCount(), 4);
        page->setSection(3);
        QCOMPARE(page->section(), 3);
        QVERIFY(control<QComboBox>(page, "settings.layout")->isVisible());
        app->closeSettings();
        app->showSettings(); // reopens on the section used last
        QCOMPARE(app->settingsPage()->section(), 3);
        app->settingsPage()->setSection(0);
    }

    // Changes made while Settings is closed show up when it reopens.
    void reopenedWindowShowsCurrentValues()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        app->showSettings();
        SettingsPage *w = app->settingsPage();
        auto *mode = control<QComboBox>(w, "settings.mode");
        QCOMPARE(mode->currentIndex(), 0);
        app->closeSettings();
        app->setThemeOverrides({{QStringLiteral("mode"), QStringLiteral("dark")}});
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("shell.use"), {{QStringLiteral("name"), QStringLiteral("right-nav")}}).ok);
        QTRY_COMPARE(app->activeLayout(), QStringLiteral("right-nav"));
        app->showSettings();
        w = app->settingsPage(); // a new page
        mode = control<QComboBox>(w, "settings.mode");
        QCOMPARE(mode->currentData().toString(), QStringLiteral("dark"));
        QTRY_COMPARE(control<QComboBox>(w, "settings.layout")->currentData().toString(), QStringLiteral("right-nav"));
        QVERIFY(control<QLabel>(w, "settings.layout_status")->text().contains(app->shell()->activeName()));
    }

    void appearanceAppliesLiveAndResets()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        app->showSettings();
        SettingsPage *w = app->settingsPage();
        const EditorContext *ectx = app->editorContext();
        QCOMPARE(ectx->theme().background, EditorTheme::light().background);
        const qreal layoutSize = ectx->theme().body.pointSizeF();

        choose(control<QComboBox>(w, "settings.mode"), QStringLiteral("dark"));
        w->applyPending();
        QCOMPARE(ectx->theme().background, EditorTheme::dark().background);
        QCOMPARE(app->shell()->layoutTheme().value(QStringLiteral("mode")).toString(), QStringLiteral("light"));

        auto *size = control<QDoubleSpinBox>(w, "settings.font.size");
        auto *sizeReset = control<QToolButton>(w, "settings.font.size.reset");
        QVERIFY(sizeReset->isHidden()); // nothing to reset yet
        QCOMPARE(size->text(), size->locale().toString(qRound(layoutSize)) + QStringLiteral(" pt")); // no ".0"
        size->setValue(layoutSize + 4);
        w->applyPending();
        QCOMPARE(ectx->theme().body.pointSizeF(), layoutSize + 4);
        QVERIFY(!sizeReset->isHidden());

        auto *width = control<QSpinBox>(w, "settings.editor.max_width");
        width->setValue(0);
        w->applyPending();
        QCOMPARE(ectx->theme().maxColumnWidth, 0.0);
        QCOMPARE(width->text(), QStringLiteral("Full width"));

        // Persisted as one JSON value, layered over (not written into) the layout.
        app->flushPreferences();
        QTRY_VERIFY(app->store()->setting(prefs::kThemeOverrides).contains(QLatin1String("dark")));
        const QVariantMap stored = prefs::parseThemeOverrides(app->store()->setting(prefs::kThemeOverrides));
        QCOMPARE(stored.value(QStringLiteral("font")).toMap().value(QStringLiteral("size")).toDouble(), layoutSize + 4);

        sizeReset->click();
        w->applyPending();
        QCOMPARE(ectx->theme().body.pointSizeF(), layoutSize);
        QVERIFY(sizeReset->isHidden());
        QCOMPARE(ectx->theme().background, EditorTheme::dark().background); // other overrides stay

        auto *resetAppearance = control<QPushButton>(w, "settings.reset_appearance");
        QVERIFY(resetAppearance->isEnabled());
        resetAppearance->click();
        w->applyPending();
        QCOMPARE(ectx->theme().background, EditorTheme::light().background);
        QVERIFY(!resetAppearance->isEnabled());
        QCOMPARE(app->themeOverrides().size(), 1); // editor width is still overridden
        control<QPushButton>(w, "settings.reset_editor")->click();
        w->applyPending();
        QVERIFY(app->themeOverrides().isEmpty());
        app->closeSettings(); // writes the pending change
        QTRY_COMPARE(app->store()->setting(prefs::kThemeOverrides), QString());
    }

    void overridesSurviveRelaunch()
    {
        QTemporaryDir dir;
        {
            auto app = launch(dir.path());
            app->setThemeOverrides({{QStringLiteral("mode"), QStringLiteral("dark")},
                                    {QStringLiteral("editor"), QVariantMap{{QStringLiteral("margin"), 120}}}});
            QVERIFY(app->shutdown(false)); // flushes the coalesced write
        }
        auto app = launch(dir.path());
        QCOMPARE(app->editorContext()->theme().background, EditorTheme::dark().background);
        QCOMPARE(app->editorContext()->theme().sideMargin, 120.0);
        // Switching layouts keeps them.
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("shell.use"), {{QStringLiteral("name"), QStringLiteral("keyboard")}}).ok);
        QTRY_COMPARE(app->activeLayout(), QStringLiteral("keyboard"));
        QCOMPARE(app->editorContext()->theme().background, EditorTheme::dark().background);
        QCOMPARE(app->editorContext()->theme().sideMargin, 120.0);
    }

    void restoreLastPageOption()
    {
        QTemporaryDir dir;
        PageId root = 0, child = 0;
        {
            auto app = launch(dir.path());
            QTRY_VERIFY(app->activeSession());
            root = app->currentPage();
            child = app->newPage(root, -1, QStringLiteral("Child"), false);
            QCOMPARE(app->currentPage(), child);
            app->showSettings();
            auto *restore = control<QCheckBox>(app->settingsPage(), "settings.restore_last_page");
            QVERIFY(restore->isChecked());
            restore->setChecked(false);
            QVERIFY(app->shutdown(false));
        }
        {
            auto app = launch(dir.path());
            QTRY_VERIFY(app->activeSession());
            QCOMPARE(app->currentPage(), root); // first page, not the last one
            app->store()->setSetting(QStringLiteral("app.last_page"), QString::number(child));
            app->store()->setSetting(prefs::kRestoreLastPage, prefs::flagValue(true));
            QVERIFY(app->shutdown(false));
        }
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        QCOMPARE(app->currentPage(), child);
    }

    void layoutTabSwitchesAndReports()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        app->showSettings();
        SettingsPage *w = app->settingsPage();
        auto *layout = control<QComboBox>(w, "settings.layout");
        QCOMPARE(layout->currentData().toString(), QStringLiteral("default"));
        for (const char *n : {"default", "keyboard", "right-nav"})
            QVERIFY2(layout->findData(QString::fromLatin1(n)) >= 0, n);
        choose(layout, QStringLiteral("keyboard"));
        QTRY_COMPARE(app->activeLayout(), QStringLiteral("keyboard"));
        QTRY_COMPARE(app->store()->setting(QStringLiteral("shell.active")), QStringLiteral("keyboard"));
        QTRY_COMPARE(layout->currentData().toString(), QStringLiteral("keyboard"));
        auto *status = control<QLabel>(w, "settings.layout_status");
        QTRY_VERIFY2(status->text().contains(app->shell()->activeName()), qPrintable(status->text()));

        auto *live = control<QCheckBox>(w, "settings.live_reload");
        QVERIFY(live->isChecked());
        live->setChecked(false);
        QCOMPARE(app->store()->setting(prefs::kLiveReload), QStringLiteral("0"));
        QVERIFY(!app->liveReload());

        auto *math = control<QCheckBox>(w, "settings.math");
#ifdef FOLIA_WITH_MATH
        QVERIFY(math->isEnabled());
        math->setChecked(false);
        QCOMPARE(app->store()->setting(prefs::kMath), QStringLiteral("0"));
        QVERIFY(app->previews()); // still loaded: applies on the next launch
#else
        QVERIFY(!math->isEnabled());
#endif
    }

    void fontListIsFilledOnDemand()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        app->showSettings();
        SettingsPage *w = app->settingsPage();
        auto *body = control<QComboBox>(w, "settings.font.family");
        auto *mono = control<QComboBox>(w, "settings.mono.family");
        QCOMPARE(body->count(), 1); // only "Layout's font (…)" until the list is opened
        QCOMPARE(mono->count(), 1);
        QVERIFY(body->itemText(0).startsWith(QLatin1String("Layout's font")));
        w->populateFontLists();
        QVERIFY(body->count() > 1);
        QCOMPARE(body->currentIndex(), 0); // still the layout's font
        QCOMPARE(w->populateFontLists(), qint64(0)); // once only
        // Choosing a family overrides only the family.
        const QString family = body->itemData(1).toString();
        choose(body, family);
        w->applyPending();
        QCOMPARE(app->editorContext()->theme().body.family(), family);
        QVERIFY(!app->themeOverrides().value(QStringLiteral("font")).toMap().contains(QStringLiteral("size")));
    }

    // Switching to dark with a page open repaints the editor dark. The
    // layout's style sheets used to re-polish the editor and restore its
    // previous (light) palette: a white body with light text, a dark title.
    void darkModeRepaintsEditor()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        QTest::qWait(100);
        NoteEditor *ed = app->editorPane()->editor();
        QLineEdit *title = app->editorPane()->titleField();
        const auto bodyPixel = [ed] {
            const QImage img = ed->viewport()->grab().toImage();
            return img.pixelColor(img.width() / 2, img.height() - 1); // below the text, inside the body
        };
        const auto check = [&](const EditorTheme &want) {
            QTRY_COMPARE(ed->viewport()->palette().color(QPalette::Base), want.background);
            QCOMPARE(bodyPixel(), want.background);
            QCOMPARE(ed->palette().color(QPalette::Text), want.text);
            QCOMPARE(title->palette().color(QPalette::Text), want.text);
        };
        check(EditorTheme::light());
        app->setThemeOverrides({{QStringLiteral("mode"), QStringLiteral("dark")}});
        check(EditorTheme::dark());
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("shell.reload"), {}).ok); // re-polish again
        QTest::qWait(50);
        check(EditorTheme::dark());
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("shell.use"), {{QStringLiteral("name"), QStringLiteral("keyboard")}}).ok);
        QTRY_COMPARE(app->activeLayout(), QStringLiteral("keyboard"));
        check(EditorTheme::dark());
        app->setThemeOverrides({});
        check(EditorTheme::light());
    }

    void settingsShortcutNotTakenByLayout()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QVERIFY2(app->shell()->warnings().isEmpty(), qPrintable(app->shell()->warnings().join(QLatin1Char('\n'))));
        QVERIFY(app->shell()->widgetById(QStringLiteral("open_settings")));
    }
};

QTEST_MAIN(SettingsTest)
#include "tst_settings.moc"
