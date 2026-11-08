// The split view benchmark (docs/BENCHMARKS.md §6c):
//
//   --bench split [--bench-arg pdf_dir=<dir>] [--bench-arg idle_seconds=10]
//                 [--bench-arg drag_steps=120] [--bench-arg step_px=6]
//                 [--bench-arg note_lines=100]
//
// Fixtures as for `--bench pdf` (`folia_mediaprobe --make-pdf <dir>` first).
// On a note of `note_lines` paragraphs with the 200-page document's card:
//   1. idle with one tab (the note), the baseline;
//   2. the PDF opened beside it: time to a fully drawn viewport, then idle
//      with two tabs;
//   3. the divider dragged at frame rate, `drag_steps` moves of `step_px`
//      (right, then back), twice: with the viewer's resize settle (default)
//      and without it (every step asks for its own zoom). Per step: main-
//      thread time of the move (both panes' resize handling), renders and
//      tiles started during the drag and after it settled, work discarded,
//      editor blocks laid out, process and render CPU;
//   4. typing in the note beside the open PDF: viewer paints (expected 0),
//      input to paint;
//   5. the PDF closed: footprint, documents left open.
#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "app/MediaFixtures.h"
#include "app/PaneSplitter.h"
#include "app/PdfViewerPage.h"
#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorPane.h"
#include "editor/MediaKey.h"
#include "editor/MediaService.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PdfView.h"
#include "storage/AttachmentStore.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMouseEvent>
#include <QSplitterHandle>
#include <QTextBlock>
#include <QTextDocument>

#include <cstdio>

namespace folia {

void BenchDriver::scenarioSplit()
{
    MediaService *media = m_app->media();
    AttachmentStore *as = m_app->attachments();
    NoteEditor *ed = m_app->editorPane()->editor();
    if (!media || !as) {
        m_notes.append(QStringLiteral("no media service"));
        return;
    }
    const QString dir = m_args.value(QStringLiteral("pdf_dir"), QDir(m_app->homeDir()).filePath(QStringLiteral("bench-pdf"))).toString();
    const int idleSeconds = m_args.value(QStringLiteral("idle_seconds"), 10).toInt();
    const int steps = qMax(2, m_args.value(QStringLiteral("drag_steps"), 120).toInt());
    const int stepPx = m_args.value(QStringLiteral("step_px"), 6).toInt();
    const int noteLines = qMax(1, m_args.value(QStringLiteral("note_lines"), 100).toInt());
    const fixtures::PdfSet set = fixtures::makePdfSet(dir);
    if (set.long200.isEmpty()) {
        m_notes.append(QStringLiteral("this platform cannot write the PDF fixtures (CoreGraphics only)"));
        return;
    }
    platform::setAppNapAllowed(false);

    // The note: 100 paragraphs, then the card.
    m_app->newPage(0, -1, QStringLiteral("Split bench"), false);
    waitFor([&] { return m_app->activeSession() && ed->session() == m_app->activeSession(); }, 5000);
    {
        QString body;
        for (int i = 0; i < noteLines; ++i)
            body += QStringLiteral("%1. Notes next to the documents: what to read first, what each one is for, "
                                   "and what to look up again later.\n").arg(i + 1);
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        c.insertText(body);
        ed->setTextCursor(c);
        AttachmentStore::Source s;
        s.path = set.long200;
        s.name = QFileInfo(set.long200).fileName();
        ed->importImages({s});
        as->waitForImports();
        waitFor([&] { return !m_app->activeSession()->hasPendingImports(); }, 10000);
        c.movePosition(QTextCursor::Start);
        ed->setTextCursor(c);
    }
    QString hash;
    for (QTextBlock b = m_app->activeSession()->document()->firstBlock(); b.isValid(); b = b.next())
        if (PageSession::roleOf(b) == LineRole::Pdf)
            hash = PageSession::imageHash(b);
    if (hash.isEmpty()) {
        m_notes.append(QStringLiteral("PDF import failed"));
        return;
    }
    media->waitIdle();

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
    struct Counts {
        qint64 renders, tiles, dropped, discarded, cpuDiscarded, blocks;
        static Counts now()
        {
            return {instr::get(instr::PdfRenders), instr::get(instr::PdfTiles), instr::get(instr::MediaJobsDroppedQueued),
                    instr::get(instr::MediaJobsDiscarded), instr::get(instr::MediaCpuUsDiscarded),
                    instr::get(instr::BlocksLaidOut)};
        }
    };
    auto putCounts = [&](const QString &prefix, const Counts &a, const Counts &b) {
        m_extra.insert(prefix + QStringLiteral("_pdf_renders"), double(b.renders - a.renders));
        m_extra.insert(prefix + QStringLiteral("_pdf_tiles"), double(b.tiles - a.tiles));
        m_extra.insert(prefix + QStringLiteral("_jobs_dropped_queued"), double(b.dropped - a.dropped));
        m_extra.insert(prefix + QStringLiteral("_jobs_discarded"), double(b.discarded - a.discarded));
        m_extra.insert(prefix + QStringLiteral("_cpu_s_discarded"), (b.cpuDiscarded - a.cpuDiscarded) / 1e6);
        m_extra.insert(prefix + QStringLiteral("_editor_blocks_laid_out"), double(b.blocks - a.blocks));
    };
    auto renderCpu = [](const QHash<QString, QVector<qint64>> &samples) {
        qint64 cpu = 0;
        for (qint64 us : samples.value(QStringLiteral("pdf_render_cpu_us")))
            cpu += us;
        return cpu / 1e6;
    };

    if (const int hold = m_args.value(QStringLiteral("demo_hold_ms"), 0).toInt(); hold > 0) {
        // Screenshots only: the PDF on the right, then on the left at 40 %.
        m_app->showPdf(hash, QStringLiteral("long-200.pdf"));
        settle(400);
        media->waitIdle();
        settle(300);
        fprintf(stderr, "demo: right\n");
        settle(hold);
        m_app->swapPanes();
        m_app->splitter()->setRatio(0.4);
        settle(400);
        media->waitIdle();
        settle(300);
        fprintf(stderr, "demo: left\n");
        settle(hold);
        m_app->swapPanes(); // leave the scratch workspace as it was
        m_app->splitter()->setRatio(0.5);
        m_app->closePdf();
        return;
    }

    // 1. One tab.
    idleWindow(QStringLiteral("one_tab"));

    // 2. The PDF beside the note.
    {
        QElapsedTimer t;
        t.start();
        m_app->showPdf(hash, QStringLiteral("long-200.pdf"));
        PdfView *v = m_app->pdfViewer()->view();
        const bool full = waitFor([&] { return v->paintCount() > 0 && v->viewportCoverage().blank == 0
                                               && v->viewportCoverage().scaled == 0; }, 10000);
        m_extra.insert(QStringLiteral("open_to_full_viewport_ms"), full ? t.nsecsElapsed() / 1e6 : -1.0);
        m_extra.insert(QStringLiteral("pdf_pane_width"), double(m_app->pdfViewer()->width()));
        m_extra.insert(QStringLiteral("note_pane_width"), double(m_app->editorPane()->width()));
        media->waitIdle();
        idleWindow(QStringLiteral("two_tabs"));
        m_extra.insert(QStringLiteral("two_tabs_media_cache_bytes"), double(media->cacheBytes()));
    }

    // 3. The divider, at frame rate, with and without the resize settle.
    PaneSplitter *split = m_app->splitter();
    PdfView *v = m_app->pdfViewer()->view();
    for (const bool settleOn : {true, false}) {
        const QString prefix = settleOn ? QStringLiteral("drag_settle") : QStringLiteral("drag_nosettle");
        v->setSettleResize(settleOn);
        v->fitWidth();
        media->waitIdle();
        settle(500);
        instr::takeSamples();
        QSplitterHandle *h = split->handle(1);
        const QPoint g = h->mapToGlobal(h->rect().center());
        auto send = [&](QEvent::Type type, int off, Qt::MouseButton b, Qt::MouseButtons bs) {
            const QPoint gp = g + QPoint(off, 0);
            QMouseEvent e(type, QPointF(h->mapFromGlobal(gp)), QPointF(gp), b, bs, Qt::NoModifier);
            QApplication::sendEvent(h, &e);
        };
        // Results by kind (thumbnail in the note, viewer page, viewer tile).
        int arrivedKinds[4] = {0, 0, 0, 0};
        const QMetaObject::Connection kinds = connect(media, &MediaService::arrived, this, [&](const MediaKey &k) {
            if (int(k.kind) < 4)
                ++arrivedKinds[int(k.kind)];
        });
        const Counts c0 = Counts::now();
        const platform::ProcessSample s0 = platform::sampleSelf();
        const int viewPaints0 = v->paintCount();
        const int editorPaints0 = m_paints;
        QElapsedTimer wall;
        wall.start();
        send(QEvent::MouseButtonPress, 0, Qt::LeftButton, Qt::LeftButton);
        int off = 0;
        for (int i = 0; i < steps; ++i) {
            off += (i < steps / 2 ? stepPx : -stepPx);
            QElapsedTimer st;
            st.start();
            send(QEvent::MouseMove, off, Qt::NoButton, Qt::LeftButton);
            add(prefix + QStringLiteral("_step_us"), st.nsecsElapsed() / 1000.0, QStringLiteral("us"),
                QStringLiteral("one divider move: both panes' resize handling on the main thread (paint excluded)"));
            settle(16); // one frame: the panes paint
        }
        send(QEvent::MouseButtonRelease, off, Qt::LeftButton, Qt::NoButton);
        const double dragMs = wall.nsecsElapsed() / 1e6;
        const Counts c1 = Counts::now();
        m_extra.insert(prefix + QStringLiteral("_during_arrived_thumbs"), double(arrivedKinds[MediaKey::PdfThumb]));
        m_extra.insert(prefix + QStringLiteral("_during_arrived_pages"), double(arrivedKinds[MediaKey::PdfPage]));
        m_extra.insert(prefix + QStringLiteral("_during_arrived_tiles"), double(arrivedKinds[MediaKey::PdfTile]));
        disconnect(kinds);
        const auto dragSamples = instr::takeSamples();
        const platform::ProcessSample s1 = platform::sampleSelf();
        media->waitIdle(60000);
        settle(400);
        media->waitIdle(60000);
        const Counts c2 = Counts::now();
        const auto afterSamples = instr::takeSamples();
        const platform::ProcessSample s2 = platform::sampleSelf();
        m_extra.insert(prefix + QStringLiteral("_wall_ms"), dragMs);
        putCounts(prefix + QStringLiteral("_during"), c0, c1);
        putCounts(prefix + QStringLiteral("_after"), c1, c2);
        m_extra.insert(prefix + QStringLiteral("_process_cpu_s"),
                       (s1.userSeconds + s1.systemSeconds) - (s0.userSeconds + s0.systemSeconds));
        m_extra.insert(prefix + QStringLiteral("_settle_process_cpu_s"),
                       (s2.userSeconds + s2.systemSeconds) - (s1.userSeconds + s1.systemSeconds));
        m_extra.insert(prefix + QStringLiteral("_render_cpu_s"), renderCpu(dragSamples) + renderCpu(afterSamples));
        m_extra.insert(prefix + QStringLiteral("_viewer_paints"), double(v->paintCount() - viewPaints0));
        m_extra.insert(prefix + QStringLiteral("_editor_paints"), double(m_paints - editorPaints0));
        m_extra.insert(prefix + QStringLiteral("_peak_footprint_bytes"), double(s2.peakFootprintBytes));
        for (qint64 us : dragSamples.value(QStringLiteral("pdf_view_paint_us")))
            add(prefix + QStringLiteral("_viewer_paint_us"), double(us));
        fprintf(stderr, "split: %s done\n", qPrintable(prefix));
    }
    v->setSettleResize(true);
    media->waitIdle();

    // 4. Typing beside the open PDF.
    {
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        ed->setTextCursor(c);
        settle(300);
        instr::takeSamples();
        const int viewPaints0 = v->paintCount();
        const Counts c0 = Counts::now();
        typeText(QStringLiteral("Typing beside the PDF. "), 200, false);
        m_extra.insert(QStringLiteral("typing_viewer_paints"), double(v->paintCount() - viewPaints0));
        m_extra.insert(QStringLiteral("typing_pdf_renders"), double(Counts::now().renders - c0.renders));
        const auto samples = instr::takeSamples();
        for (qint64 us : samples.value(QStringLiteral("input_to_paint_us")))
            add(QStringLiteral("typing_beside_input_to_paint_us"), double(us));
    }

    // 5. Closed.
    m_app->closePdf();
    media->waitIdle();
    idleWindow(QStringLiteral("closed"));
    m_extra.insert(QStringLiteral("closed_media_cache_bytes"), double(media->cacheBytes()));
    m_extra.insert(QStringLiteral("closed_pdf_documents_open"), double(platform::PdfDocument::liveCount()));
}

} // namespace folia
