// The upload benchmark (docs/BENCHMARKS.md §6e):
//
//   --bench import [--bench-arg md_mib=8] [--bench-arg docx_paragraphs=40000]
//                  [--bench-arg idle_seconds=10]
//
// For a long Markdown file and a long Word file (written by
// fixtures::makeUploadSet), each uploaded on its own:
//   1. time from the upload to the new tab's page being editable;
//   2. the conversion's time on the import worker and the main thread's
//      share (creating the page and queuing its save);
//   3. the peak footprint growth from the upload to the page shown;
//   4. footprint and idle CPU and wakeups 10 s later, then after the tab is
//      closed (its page leaves the document cache).
#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "app/DocumentImport.h"
#include "app/MediaFixtures.h"
#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"

#include <QDir>
#include <QElapsedTimer>
#include <QTimer>

namespace folia {

void BenchDriver::scenarioImport()
{
    const int mdMiB = qBound(1, m_args.value(QStringLiteral("md_mib"), 8).toInt(), 15);
    const int paragraphs = qMax(100, m_args.value(QStringLiteral("docx_paragraphs"), 40000).toInt());
    const int idleSeconds = m_args.value(QStringLiteral("idle_seconds"), 10).toInt();
    const fixtures::UploadSet set =
        fixtures::makeUploadSet(QDir(m_app->homeDir()).filePath(QStringLiteral("bench-upload")), mdMiB, paragraphs);
    if (set.markdown.isEmpty() || set.docx.isEmpty()) {
        m_notes.append(QStringLiteral("could not write the upload fixtures"));
        return;
    }
    platform::setAppNapAllowed(false);
    NoteEditor *ed = m_app->editorPane()->editor();
    instr::setSamplingEnabled(true);
    // The first tab gets a small page of its own, as a user's would. A fresh
    // home has an empty tab, which an upload would fill instead of opening a
    // new tab (and §6e's first runs closed the upload's tab beside it).
    if (!m_app->activeSession() || m_app->tabs().current().page <= 0) {
        QTimer::singleShot(0, m_app, [this] { m_app->newPage(0, -1, QStringLiteral("Before the upload"), false); });
        const bool created = waitFor([&] {
            PageSession *s = m_app->activeSession();
            return s && m_app->tabs().current().page == s->pageId() && ed->session() == s && !ed->isReadOnly()
                && !s->isUncommitted() && !s->hasUnsnapshotted();
        }, 10000);
        m_extra.insert(QStringLiteral("first_page_created"), created);
    }

    for (const auto &[kind, file] : {std::pair{QStringLiteral("markdown"), set.markdown},
                                     std::pair{QStringLiteral("docx"), set.docx}}) {
        settle(2000);
        const int tabsBefore = m_app->tabs().count();
        platform::intervalPeakFootprint(); // resets the interval maximum
        const qint64 before = platform::sampleSelf().footprintBytes;
        platform::intervalPeakFootprint();
        QElapsedTimer t;
        t.start();
        m_app->uploadFiles({file});
        const bool ok = waitFor([&] {
            return m_app->tabs().count() > tabsBefore && ed->session()
                && ed->session()->pageId() == m_app->tabs().current().page && !ed->isReadOnly();
        }, 60000);
        const double toEditable = t.nsecsElapsed() / 1e3;
        const qint64 peak = platform::intervalPeakFootprint();
        if (!ok) {
            m_notes.append(kind + QStringLiteral(": the page did not open"));
            continue;
        }
        add(kind + QStringLiteral("_upload_to_editable_us"), toEditable);
        m_extra.insert(kind + QStringLiteral("_peak_growth_bytes"), double(qMax<qint64>(0, peak - before)));
        m_extra.insert(kind + QStringLiteral("_blocks"), double(m_app->activeSession()->document()->blockCount()));
        // 10 s later: idle with the imported page shown.
        settle(2000);
        const platform::ProcessSample first = platform::sampleSelf();
        QElapsedTimer idle;
        idle.start();
        settle(idleSeconds * 1000);
        const platform::ProcessSample last = platform::sampleSelf();
        const double wall = idle.nsecsElapsed() / 1e9;
        m_extra.insert(kind + QStringLiteral("_shown_footprint_bytes"), double(last.footprintBytes));
        m_extra.insert(kind + QStringLiteral("_shown_idle_cpu_percent"),
                       ((last.userSeconds + last.systemSeconds) - (first.userSeconds + first.systemSeconds)) / wall * 100);
        m_extra.insert(kind + QStringLiteral("_shown_idle_wakeups_per_s"),
                       double((last.idleWakeups + last.interruptWakeups) - (first.idleWakeups + first.interruptWakeups)) / wall);
        // Closed: back to the tab before it. From the event loop, as a click
        // on × does: deleteLater() inside the bench's own nested loops would
        // wait until the bench returns.
        QTimer::singleShot(0, m_app, [this] { m_app->closeTab(m_app->tabs().activeIndex()); });
        // When the memory comes back: footprint at 0.5, 1, 2, 3 s after the close.
        for (int ms : {500, 500, 1000, 1000}) {
            settle(ms);
            add(kind + QStringLiteral("_after_close_footprint"), double(platform::sampleSelf().footprintBytes),
                QStringLiteral("bytes"));
        }
        m_extra.insert(kind + QStringLiteral("_closed_footprint_bytes"), double(platform::sampleSelf().footprintBytes));
        m_extra.insert(kind + QStringLiteral("_closed_doc_cache_entries"), m_app->docCacheEntries());
        m_extra.insert(kind + QStringLiteral("_closed_sessions_alive"),
                       int(m_app->findChildren<PageSession *>(Qt::FindDirectChildrenOnly).size()));
        // What the allocator would hand back if asked (malloc_zone_pressure_relief).
        QElapsedTimer rt;
        rt.start();
        const qint64 released = platform::releaseFreedMemory();
        m_extra.insert(kind + QStringLiteral("_relief_us"), double(rt.nsecsElapsed() / 1000));
        m_extra.insert(kind + QStringLiteral("_relief_released_bytes"), double(released));
        for (int s = 0; s < 4; ++s) {
            settle(5000);
            add(kind + QStringLiteral("_later_footprint"), double(platform::sampleSelf().footprintBytes),
                QStringLiteral("bytes")); // 5, 10, 15, 20 s after the relief
        }
        m_extra.insert(kind + QStringLiteral("_after_relief_footprint_bytes"), double(platform::sampleSelf().footprintBytes));
    }
    addInstrSamples(); // document_import_worker_us, document_import_main_us, session_build_us…
}

} // namespace folia
