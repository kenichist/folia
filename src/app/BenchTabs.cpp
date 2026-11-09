// The note tabs benchmark (docs/BENCHMARKS.md §6d):
//
//   --bench tabs [--bench-arg tabs=30] [--bench-arg switches=150]
//                [--bench-arg idle_seconds=20] [--bench-arg page_lines=100]
//                [--bench-arg resize_steps=60] [--bench-arg strip=0]
//
// On `tabs` pages of `page_lines` paragraphs each (an "ordinary page"):
//   1. idle with one tab, the baseline;
//   2. `tabs`-1 pages opened in background tabs: main-thread time per tab,
//      shell renders and pages loaded (expected 0), footprint;
//   3. idle with all tabs open (expected: the baseline);
//   4. `switches` switches with tab.next: time to the painted page (cached
//      and not), shell renders and strip paints per switch, footprint after
//      each round (the document cache keeps it bounded);
//   5. typing in the note: strip paints (expected 0);
//   6. the window resized `resize_steps` times by 8 px: main-thread time per
//      step (the whole window's relayout, the strip included);
//   7. all tabs but one closed: footprint, pages left in the cache.
// strip=0 runs the same with the default layout minus ui.tabs{} (the tabs
// still exist, nothing draws them): the strip's own cost is the difference.
#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "shell/CommandRegistry.h"
#include "shell/ShellHost.h"
#include "shell/TabStrip.h"
#include "storage/Store.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QTimer>

namespace folia {

void BenchDriver::scenarioTabs()
{
    const int tabs = qBound(2, m_args.value(QStringLiteral("tabs"), 30).toInt(), TabList::kMaxTabs);
    const int switches = qMax(1, m_args.value(QStringLiteral("switches"), 150).toInt());
    const int idleSeconds = m_args.value(QStringLiteral("idle_seconds"), 20).toInt();
    const int pageLines = qMax(1, m_args.value(QStringLiteral("page_lines"), 100).toInt());
    const int resizeSteps = qMax(2, m_args.value(QStringLiteral("resize_steps"), 60).toInt());
    const bool strip = m_args.value(QStringLiteral("strip"), 1).toInt() != 0;
    m_extra.insert(QStringLiteral("tabs"), tabs);
    m_extra.insert(QStringLiteral("strip"), strip);
    platform::setAppNapAllowed(false);
    TabStrip *ts = m_app->tabStrip();

    if (!strip) {
        // The default layout without its ui.tabs{} line.
        QFile f(m_app->presetPath(QStringLiteral("default")));
        QByteArray code;
        if (f.open(QIODevice::ReadOnly))
            code = f.readAll();
        const int at = int(code.indexOf("ui.tabs{"));
        if (at < 0) {
            m_notes.append(QStringLiteral("the default layout has no ui.tabs{} line"));
            return;
        }
        const int eol = int(code.indexOf('\n', at));
        code.remove(at, eol - at);
        QString err;
        if (!m_app->shell()->loadSource(code, QStringLiteral("default-without-tabs.lua"), &err)) {
            m_notes.append(QStringLiteral("layout without tabs failed: ") + err);
            return;
        }
        settle(500);
    }

    // The pages: `tabs` ordinary pages, saved before the run.
    QVector<PageId> pages;
    Store *store = m_app->store();
    for (int p = 0; p < tabs; ++p) {
        const PageId id = store->createPage(0, -1, QStringLiteral("Tab bench %1").arg(p + 1));
        SaveBatch b;
        b.page = id;
        b.rev = 1;
        b.hasTitle = true;
        b.title = QStringLiteral("Tab bench %1").arg(p + 1);
        b.hasOrder = true;
        for (int i = 0; i < pageLines; ++i) {
            BlockRecord r;
            r.id = store->allocateBlockId();
            r.text = i % 10 == 0 ? QStringLiteral("## Section %1").arg(i / 10 + 1)
                                 : QStringLiteral("Line %1 of page %2: notes with **bold**, *italic*, `code` and a "
                                                  "formula $e^{i\\pi} + 1 = 0$ to lay out.").arg(i + 1).arg(p + 1);
            b.upserts << r;
            b.order << r.id;
        }
        store->commit(b);
        pages << id;
    }
    store->flush(10000);
    {
        // Editable, not painted: a window macOS stops painting must not abort the run.
        NoteEditor *ed = m_app->editorPane()->editor();
        const PageId first = pages.first();
        m_app->openPage(first, true);
        if (!waitFor([&] { return ed->session() && ed->session()->pageId() == first && !ed->isReadOnly(); }, 10000)) {
            m_notes.append(QStringLiteral("first page did not open"));
            return;
        }
    }

    auto idleWindow = [&](const QString &prefix) {
        settle(2000);
        const platform::ProcessSample first = platform::sampleSelf();
        QElapsedTimer t;
        t.start();
        settle(idleSeconds * 1000);
        const platform::ProcessSample last = platform::sampleSelf();
        const double wall = t.nsecsElapsed() / 1e9;
        m_extra.insert(prefix + QStringLiteral("_mean_cpu_percent"),
                       ((last.userSeconds + last.systemSeconds) - (first.userSeconds + first.systemSeconds)) / wall * 100);
        m_extra.insert(prefix + QStringLiteral("_mean_wakeups_per_s"),
                       double((last.idleWakeups + last.interruptWakeups) - (first.idleWakeups + first.interruptWakeups)) / wall);
        m_extra.insert(prefix + QStringLiteral("_footprint_bytes"), double(last.footprintBytes));
    };

    // 1. One tab.
    idleWindow(QStringLiteral("idle_one_tab"));

    // 2. Background tabs.
    {
        const qint64 renders = instr::get(instr::LuaRenders);
        const int cacheBefore = m_app->docCacheEntries();
        const qint64 fp = platform::sampleSelf().footprintBytes;
        for (int p = 1; p < pages.size(); ++p) {
            QElapsedTimer t;
            t.start();
            m_app->openInNewTab(pages[p], true);
            add(QStringLiteral("tab_open_background_us"), t.nsecsElapsed() / 1e3);
        }
        settle(500);
        m_extra.insert(QStringLiteral("background_open_lua_renders"), double(instr::get(instr::LuaRenders) - renders));
        m_extra.insert(QStringLiteral("background_open_pages_loaded"), m_app->docCacheEntries() - cacheBefore);
        m_extra.insert(QStringLiteral("background_open_footprint_delta"),
                       double(platform::sampleSelf().footprintBytes - fp));
    }

    // 3. All tabs open, the first shown.
    idleWindow(QStringLiteral("idle_all_tabs"));

    // 4. Switching through them.
    {
        const qint64 renders = instr::get(instr::LuaRenders);
        const int paints = ts ? ts->paintCount() : 0;
        int done = 0;
        int missingPaints = 0;
        NoteEditor *ed = m_app->editorPane()->editor();
        for (int round = 0; done < switches; ++round) {
            for (int k = 0; k < m_app->tabs().count() && done < switches; ++k, ++done) {
                const int next = (m_app->tabs().activeIndex() + 1) % m_app->tabs().count();
                const PageId target = m_app->tabs().at(next).page;
                bool painted = false, wasCached = false;
                qint64 paintUs = -1;
                auto c1 = connect(m_app, &AppController::pagePainted, this, [&](PageId p, qint64 t) {
                    if (p == target) {
                        painted = true;
                        paintUs = t;
                    }
                });
                auto c2 = connect(m_app, &AppController::pageActivated, this, [&](PageId p, qint64, bool c) {
                    if (p == target)
                        wasCached = c;
                });
                QElapsedTimer t;
                t.start();
                m_app->commands()->dispatchCommand(QStringLiteral("tab.next"), {});
                add(QStringLiteral("tab_switch_dispatch_us"), t.nsecsElapsed() / 1e3);
                // Editable: the editor shows the tab's page (no paint needed,
                // so a window macOS stops painting cannot stall the run).
                waitFor([&] { return ed->session() && ed->session()->pageId() == target && !ed->isReadOnly(); }, 5000);
                const double editableUs = t.nsecsElapsed() / 1e3;
                const QString kind = wasCached ? QStringLiteral("cached") : QStringLiteral("uncached");
                add(QStringLiteral("tab_switch_to_editable_%1_us").arg(kind), editableUs);
                if (waitFor([&] { return painted; }, 1000))
                    add(QStringLiteral("tab_switch_to_paint_%1_us").arg(kind), double(paintUs));
                else
                    ++missingPaints;
                disconnect(c1);
                disconnect(c2);
                settle(30);
            }
            settle(500);
            add(QStringLiteral("footprint_after_round"), double(platform::sampleSelf().footprintBytes),
                QStringLiteral("bytes"));
        }
        m_extra.insert(QStringLiteral("switch_paints_missing"), missingPaints);
        m_extra.insert(QStringLiteral("switch_lua_renders_per_switch"),
                       double(instr::get(instr::LuaRenders) - renders) / switches);
        if (ts)
            m_extra.insert(QStringLiteral("switch_strip_paints_per_switch"), double(ts->paintCount() - paints) / switches);
        m_extra.insert(QStringLiteral("switch_doc_cache_entries"), m_app->docCacheEntries());
    }

    // 5. Typing beside the strip.
    if (ts) {
        settle(300);
        const int paints = ts->paintCount();
        typeText(QStringLiteral("typing next to the tab strip "), 200, false);
        settle(300);
        m_extra.insert(QStringLiteral("typing_strip_paints"), ts->paintCount() - paints);
    }

    // 6. Window resizes.
    {
        QMainWindow *w = m_app->window();
        const QSize start = w->size();
        for (int s = 0; s < resizeSteps; ++s) {
            const int dw = (s < resizeSteps / 2 ? -8 : 8);
            QElapsedTimer t;
            t.start();
            w->resize(w->width() + dw, w->height());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            w->repaint();
            add(QStringLiteral("window_resize_step_us"), t.nsecsElapsed() / 1e3);
        }
        w->resize(start);
        settle(300);
    }

    // 7. Closing all but the shown tab.
    {
        // From the event loop, as clicks on × are: deleteLater() inside the
        // bench's own nested loops would wait until the bench returns.
        QTimer::singleShot(0, m_app, [this] {
            while (m_app->tabs().count() > 1)
                m_app->closeTab(m_app->tabs().activeIndex() == 0 ? 1 : 0);
        });
        settle(2000);
        m_extra.insert(QStringLiteral("closed_footprint_bytes"), double(platform::sampleSelf().footprintBytes));
        m_extra.insert(QStringLiteral("closed_doc_cache_entries"), m_app->docCacheEntries());
    }
    addInstrSamples();
}

} // namespace folia
