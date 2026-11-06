// The PDF benchmark scenario (docs/ATTACHMENTS.md §6, docs/BENCHMARKS.md):
//
//   --bench pdf [--bench-arg pdf_dir=<dir>] [--bench-arg idle_seconds=20]
//               [--bench-arg step=300] [--bench-arg zoom_rounds=6]
//
// Fixtures come from `folia_mediaprobe --make-pdf <dir>` (written out of
// process when possible). Measures, in this order:
//   0. in-app render cost of each document kind, for a thumbnail and for one
//      viewer tile: wall time and peak footprint against the reservation;
//   1. idle on a text page (baseline);
//   2. a note with five PDF cards, built through the real import path;
//   3. typing in that note while a PDF view beside it renders vector tiles
//      at high zoom (input latency; repaints each side caused the other);
//   4. the 200-page document in the viewer: scrolled to the end and back
//      (footprint plateau, renders, tiles, dropped and discarded work, cache
//      hits), then zoomed in and out repeatedly (renders during gestures,
//      after settling);
//   5. scanned, vector-heavy and transparency-heavy documents: first full
//      viewport and a scroll through every page;
//   6. idle with the viewer open and settled, then memory pressure and
//      scrolling on during the cool-down;
//   7. closed: footprint released, documents closed, idle again.
#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "app/MediaFixtures.h"
#include "app/PdfViewerPage.h"
#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorPane.h"
#include "editor/MediaService.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PdfView.h"
#include "storage/AttachmentStore.h"
#include "storage/Store.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMainWindow>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>

#include <cstdio>

namespace folia {

namespace {

struct PdfCounters {
    qint64 renders, tiles, hits, dropped, discarded, cpuDiscarded, waits, tooLarge;
    static PdfCounters now()
    {
        return {instr::get(instr::PdfRenders), instr::get(instr::PdfTiles), instr::get(instr::PdfCacheHits),
                instr::get(instr::MediaJobsDroppedQueued), instr::get(instr::MediaJobsDiscarded),
                instr::get(instr::MediaCpuUsDiscarded), instr::get(instr::MediaBudgetWaits),
                instr::get(instr::MediaTooLarge)};
    }
    PdfCounters operator-(const PdfCounters &o) const
    {
        return {renders - o.renders, tiles - o.tiles, hits - o.hits, dropped - o.dropped, discarded - o.discarded,
                cpuDiscarded - o.cpuDiscarded, waits - o.waits, tooLarge - o.tooLarge};
    }
};

const char *kLorem = "Notes next to the documents: what to read first, and what each one is for. ";

} // namespace

void BenchDriver::scenarioPdf()
{
    MediaService *media = m_app->media();
    AttachmentStore *as = m_app->attachments();
    NoteEditor *ed = m_app->editorPane()->editor();
    if (!media || !as) {
        m_notes.append(QStringLiteral("no media service"));
        return;
    }
    const QString dir = m_args.value(QStringLiteral("pdf_dir"), QDir(m_app->homeDir()).filePath(QStringLiteral("bench-pdf"))).toString();
    const int idleSeconds = m_args.value(QStringLiteral("idle_seconds"), 20).toInt();
    const int step = m_args.value(QStringLiteral("step"), 300).toInt();
    const int zoomRounds = m_args.value(QStringLiteral("zoom_rounds"), 6).toInt();
    const int settleMs = m_args.value(QStringLiteral("settle_ms"), 3000).toInt();
    QElapsedTimer gen;
    gen.start();
    const fixtures::PdfSet set = fixtures::makePdfSet(dir);
    m_extra.insert(QStringLiteral("fixtures_ms"), double(gen.elapsed()));
    if (set.long200.isEmpty()) {
        m_notes.append(QStringLiteral("this platform cannot write the PDF fixtures (CoreGraphics only)"));
        return;
    }
    if (gen.elapsed() > 500)
        m_notes.append(QStringLiteral("fixtures were generated in this process (run `folia_mediaprobe --make-pdf <dir>` "
                                      "first): footprints include the writer's freed memory"));
    const QString counterName = platform::sampleSelf().counterName;
    m_extra.insert(QStringLiteral("media_budget_bytes"), double(media->budget().base()));
    if (const int hold = m_args.value(QStringLiteral("demo_hold_ms"), 0).toInt(); hold > 0) {
        // Screenshots only: the note with its cards, the viewer, a zoom.
        m_app->newPage(0, -1, QStringLiteral("Reading list"), false);
        ed->textCursor().insertText(QString::fromUtf8(kLorem) + QStringLiteral("\n"));
        for (const QString &path : {set.long200, set.transparency, set.scanned}) {
            AttachmentStore::Source s;
            s.path = path;
            s.name = QFileInfo(path).fileName();
            QTextCursor c = ed->textCursor();
            c.movePosition(QTextCursor::End);
            c.insertText(QStringLiteral("\n"));
            ed->setTextCursor(c);
            ed->importImages({s});
            as->waitForImports();
        }
        media->waitIdle();
        fprintf(stderr, "demo: cards\n");
        settle(hold);
        QTextBlock card = ed->session()->document()->findBlockByNumber(2);
        m_app->showPdf(PageSession::imageHash(card), QStringLiteral("long-200.pdf"));
        PdfView *dv = m_app->pdfViewer()->view();
        dv->goToPage(2);
        settle(300);
        media->waitIdle();
        fprintf(stderr, "demo: viewer\n");
        settle(hold);
        dv->zoomGesture(2.5, QPointF(dv->viewport()->width() / 2.0, 200));
        settle(400);
        media->waitIdle();
        fprintf(stderr, "demo: zoomed\n");
        settle(hold);
        m_app->closePdf();
        settle(200);
        return;
    }
    auto footprint = [] { return double(platform::sampleSelf().footprintBytes); };
    auto idleWindow = [&](const QString &prefix) {
        settle(settleMs);
        const platform::ProcessSample first = platform::sampleSelf();
        platform::ProcessSample prev = first;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < idleSeconds; ++i) {
            settle(1000);
            const platform::ProcessSample s = platform::sampleSelf();
            add(prefix + QStringLiteral("_cpu_percent_of_core"),
                ((s.userSeconds + s.systemSeconds) - (prev.userSeconds + prev.systemSeconds)) * 100, QStringLiteral("ratio"));
            add(prefix + QStringLiteral("_wakeups_per_s"),
                double((s.idleWakeups + s.interruptWakeups) - (prev.idleWakeups + prev.interruptWakeups)), QStringLiteral("ratio"),
                QStringLiteral("task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)"));
            prev = s;
        }
        const double wall = t.nsecsElapsed() / 1e9;
        m_extra.insert(prefix + QStringLiteral("_mean_cpu_percent"),
                       ((prev.userSeconds + prev.systemSeconds) - (first.userSeconds + first.systemSeconds)) / wall * 100);
        m_extra.insert(prefix + QStringLiteral("_mean_wakeups_per_s"),
                       double((prev.idleWakeups + prev.interruptWakeups) - (first.idleWakeups + first.interruptWakeups)) / wall);
        m_extra.insert(prefix + QStringLiteral("_footprint_bytes"), double(prev.footprintBytes));
        m_extra.insert(prefix + QStringLiteral("_threads"), prev.threads);
    };
    auto putCounters = [&](const QString &prefix, const PdfCounters &d) {
        m_extra.insert(prefix + QStringLiteral("_renders"), double(d.renders));
        m_extra.insert(prefix + QStringLiteral("_tiles"), double(d.tiles));
        m_extra.insert(prefix + QStringLiteral("_cache_hits"), double(d.hits));
        m_extra.insert(prefix + QStringLiteral("_cache_hit_rate"), d.hits + d.renders > 0 ? double(d.hits) / double(d.hits + d.renders) : 1.0);
        m_extra.insert(prefix + QStringLiteral("_jobs_dropped_queued"), double(d.dropped));
        m_extra.insert(prefix + QStringLiteral("_jobs_discarded"), double(d.discarded));
        m_extra.insert(prefix + QStringLiteral("_cpu_ms_discarded"), d.cpuDiscarded / 1000.0);
        m_extra.insert(prefix + QStringLiteral("_budget_waits"), double(d.waits));
        m_extra.insert(prefix + QStringLiteral("_too_large"), double(d.tooLarge));
    };
    auto putCpu = [&](const QString &prefix, const platform::ProcessSample &a, const platform::ProcessSample &b) {
        m_extra.insert(prefix + QStringLiteral("_process_cpu_s"), (b.userSeconds + b.systemSeconds) - (a.userSeconds + a.systemSeconds));
    };
    // The window may not be painted by the system (covered, another Space):
    // such frames are painted through the same code into a reused offscreen
    // image, so rendering and memory stay measured (timings excluded).
    QImage canvas;
    QHash<QString, int> unpainted;
    auto ensureViewerPaint = [&](PdfView *v, int before, const QString &phase) {
        if (waitFor([&] { return v->paintCount() > before; }, 100))
            return true;
        ++unpainted[phase];
        const qreal dpr = m_app->window()->devicePixelRatioF();
        if (canvas.isNull() || canvas.size() != v->viewport()->size() * dpr) {
            if (canvas.isNull())
                m_notes.append(QStringLiteral("window not painted by the system: some frames painted offscreen "
                                              "(see *_unpainted_frames; their timings are excluded)"));
            canvas = QImage(v->viewport()->size() * dpr, QImage::Format_ARGB32_Premultiplied);
            canvas.setDevicePixelRatio(dpr);
        }
        v->viewport()->render(&canvas);
        return false;
    };
    auto visibleDone = [&](PdfView *v) {
        for (const MediaService::Request &r : v->lastWants())
            if (r.priority == 0 && !media->contains(r.key) && !media->failed(r.key) && !media->tooLarge(r.key))
                return false;
        return !v->lastWants().isEmpty();
    };
    // Waits until the viewport is fully rendered at the current plan.
    auto waitViewport = [&](PdfView *v, const QString &phase, int timeoutMs) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            const int before = v->paintCount();
            v->viewport()->update();
            ensureViewerPaint(v, before, phase);
            if (visibleDone(v))
                return t.nsecsElapsed() / 1e6;
            settle(5);
        }
        return -1.0;
    };
    // One display-paced frame of the viewer: move, wait for its paint.
    auto viewerFrame = [&](PdfView *v, int to, const QString &metric) {
        const int before = v->paintCount();
        QElapsedTimer t;
        t.start();
        v->verticalScrollBar()->setValue(to);
        if (ensureViewerPaint(v, before, metric))
            add(metric, t.nsecsElapsed() / 1e6, QStringLiteral("ms"),
                QStringLiteral("scroll step to the end of the next viewer paint (main thread, includes waiting for the frame)"));
    };
    auto openViewer = [&](const AttachmentInfo &info, const QString &name) -> PdfView * {
        if (!m_app->showPdf(info.hash, name))
            return nullptr;
        PdfView *v = m_app->pdfViewer()->view();
        settle(50);
        return v;
    };

    // 0. Render cost per document kind, in this process, before anything
    // else runs (the peak is process-wide): the card thumbnail, and one
    // viewer tile at fit width on this display.
    const qreal dpr = m_app->window()->devicePixelRatioF();
    struct Doc { QString name, path; };
    const QVector<Doc> docs = {{QStringLiteral("text"), set.long200}, {QStringLiteral("scanned"), set.scanned},
                               {QStringLiteral("vector"), set.vector}, {QStringLiteral("transparency"), set.transparency}};
    const int fitWidthPx = qRound((m_app->window()->width() - 2 * 16 - 16) * dpr); // the viewer's fit width
    for (const Doc &d : docs) {
        const platform::PdfInfo pi = platform::pdfInfo(d.path);
        if (!pi.ok)
            continue;
        const PdfPageGeom g = pi.geom.value(qMin(2, pi.pages - 1));
        const int page = qMin(2, pi.pages - 1);
        struct Probe { QString what; int pageW; QRect region; };
        const int thumbW = qRound(87 * dpr);
        const QVector<Probe> probes = {
            {QStringLiteral("thumb"), thumbW, QRect(0, 0, thumbW, qRound(thumbW * g.h / g.w))},
            {QStringLiteral("tile"), fitWidthPx, QRect(512, 512, 512, 512)},
        };
        for (const Probe &p : probes) {
            for (int rep = 0; rep < 2; ++rep) { // cold (document just opened), then warm
                auto doc = std::make_unique<platform::PdfDocument>(d.path);
                if (rep == 1)
                    platform::renderPdfPage(doc.get(), page, p.pageW, p.region); // warms CG's per-document caches
                platform::intervalPeakFootprint();
                const qint64 before = platform::sampleSelf().footprintBytes;
                platform::intervalPeakFootprint();
                QElapsedTimer t;
                t.start();
                QImage img = platform::renderPdfPage(doc.get(), page, p.pageW, p.region);
                const double ms = t.nsecsElapsed() / 1e6;
                const double peak = double(platform::intervalPeakFootprint() - before);
                const QString key = d.name + QLatin1Char('_') + p.what + (rep ? QStringLiteral("_warm") : QStringLiteral("_cold"));
                const qint64 est = MediaService::pdfEstimate(g, p.pageW, p.region.width(), p.region.height());
                m_extra.insert(QStringLiteral("render_ms_") + key, ms);
                m_extra.insert(QStringLiteral("render_peak_bytes_") + key, peak);
                m_extra.insert(QStringLiteral("render_estimate_over_peak_") + key, peak > 0 ? double(est) / peak : 0.0);
                Q_UNUSED(img);
            }
        }
    }
    m_extra.insert(QStringLiteral("fit_width_px"), fitWidthPx);

    // 1. Baseline: a text page.
    m_app->newPage(0, -1, QStringLiteral("Text"), false);
    for (int i = 0; i < 40; ++i)
        ed->textCursor().insertText(QString::fromUtf8(kLorem) + QStringLiteral("\n"));
    m_app->flushSaves();
    idleWindow(QStringLiteral("idle_text"));

    // 2. A note with the five PDFs, through the real import path.
    m_app->newPage(0, -1, QStringLiteral("Documents"), false);
    QElapsedTimer imp;
    imp.start();
    QHash<QString, AttachmentInfo> infos;
    for (const QString &path : {set.long200, set.scanned, set.vector, set.transparency, set.mixed}) {
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n") + QString::fromUtf8(kLorem) + QString::fromUtf8(kLorem));
        ed->setTextCursor(c);
        AttachmentStore::Source s;
        s.path = path;
        s.name = QFileInfo(path).fileName();
        ed->importImages({s});
        as->waitForImports();
    }
    m_extra.insert(QStringLiteral("import_all_ms"), double(imp.elapsed()));
    PageSession *ses = ed->session();
    for (QTextBlock b = ses->document()->firstBlock(); b.isValid(); b = b.next())
        if (PageSession::roleOf(b) == LineRole::Pdf)
            if (const AttachmentInfo *i = as->info(PageSession::imageHash(b)))
                infos.insert(QFileInfo(b.begin().fragment().charFormat().property(kImageAltProperty).toString()).completeBaseName(), *i);
    m_extra.insert(QStringLiteral("pdf_cards"), int(infos.size()));
    for (auto it = infos.cbegin(); it != infos.cend(); ++it)
        m_extra.insert(QStringLiteral("page_geom_bytes_") + it.key(), double(it->pageGeom.size()));
    m_app->flushSaves();
    m_app->store()->flush(10000);
    ed->verticalScrollBar()->setValue(0);
    ed->viewport()->update();
    settle(300);
    media->waitIdle();
    m_extra.insert(QStringLiteral("cards_cache_bytes"), double(media->cacheBytes()));
    const AttachmentInfo longInfo = infos.value(QStringLiteral("long-200"));
    const AttachmentInfo vectorInfo = infos.value(QStringLiteral("vector-16"));
    if (!longInfo.isPdf() || !vectorInfo.isPdf()) {
        m_notes.append(QStringLiteral("PDF import failed"));
        return;
    }

    // 3. Typing while a PDF renders beside the note: a PDF view in its own
    // window next to the main one, rendering vector tiles at 3x.
    {
        QWidget *main = m_app->window();
        PdfView beside(m_app->editorContext(), vectorInfo, as->pathFor(vectorInfo.hash));
        beside.setWindowTitle(QStringLiteral("PDF beside the note"));
        beside.setWindowFlags(beside.windowFlags() | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput);
        beside.resize(560, main->height());
        beside.move(main->geometry().right() + 8, main->geometry().top());
        beside.show();
        settle(100);
        beside.setZoom(3.0);
        beside.viewport()->update();
        settle(50);
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        ed->setTextCursor(c);
        instr::takeSamples();
        const PdfCounters c0 = PdfCounters::now();
        const int besidePaints0 = beside.paintCount();
        const int editorPaints0 = m_paints;
        const platform::ProcessSample s0 = platform::sampleSelf();
        QElapsedTimer t;
        t.start();
        typeText(QString::fromUtf8(kLorem), 300, false);
        const double typedMs = t.nsecsElapsed() / 1e6;
        const PdfCounters during = PdfCounters::now() - c0;
        m_extra.insert(QStringLiteral("beside_typing_ms"), typedMs);
        m_extra.insert(QStringLiteral("beside_pdf_renders_while_typing"), double(during.renders));
        m_extra.insert(QStringLiteral("beside_pdf_tiles_while_typing"), double(during.tiles));
        m_extra.insert(QStringLiteral("beside_viewer_paints_while_typing"), double(beside.paintCount() - besidePaints0));
        m_extra.insert(QStringLiteral("beside_editor_paints_while_typing"), double(m_paints - editorPaints0));
        m_extra.insert(QStringLiteral("beside_pdf_busy_during_typing"), media->runningJobs() > 0 || during.renders > 0);
        putCpu(QStringLiteral("beside"), s0, platform::sampleSelf());
        const auto samples = instr::takeSamples();
        for (qint64 v : samples.value(QStringLiteral("input_to_paint_us")))
            add(QStringLiteral("pdf_beside_typing_input_to_paint_us"), double(v), QStringLiteral("us"),
                QStringLiteral("key event to end of editor paint while a PDF view beside it renders vector tiles"));
        for (qint64 v : samples.value(QStringLiteral("pdf_view_paint_us")))
            add(QStringLiteral("pdf_beside_view_paint_us"), double(v));
        // The editor paints once per key at most: PDF results never repaint it.
        const int editorPaintsBefore = m_paints;
        media->waitIdle(60000);
        settle(100);
        m_extra.insert(QStringLiteral("beside_editor_paints_after_typing"), double(m_paints - editorPaintsBefore));
        putCounters(QStringLiteral("beside"), PdfCounters::now() - c0);
    }
    settle(200);
    media->waitIdle();

    // 4. The 200-page document in the viewer.
    media->trim(0); // measured phases start cold
    const double beforeViewer = footprint();
    PdfView *v = openViewer(longInfo, QStringLiteral("long-200.pdf"));
    if (!v) {
        m_notes.append(QStringLiteral("viewer did not open"));
        return;
    }
    m_extra.insert(QStringLiteral("long_footprint_before_open"), beforeViewer);
    m_extra.insert(QStringLiteral("long_open_first_viewport_ms"), waitViewport(v, QStringLiteral("long_open"), 30000));
    m_extra.insert(QStringLiteral("long_zoom_fit"), v->zoom());
    m_extra.insert(QStringLiteral("long_plan_tiles"), v->planFor(0, v->zoom()).tiles);
    m_extra.insert(QStringLiteral("long_page_px"), QStringLiteral("%1x%2").arg(v->planFor(0, v->zoom()).width).arg(v->planFor(0, v->zoom()).height));
    media->waitIdle();
    QScrollBar *vb = v->verticalScrollBar();
    instr::takeSamples();
    PdfCounters c0 = PdfCounters::now();
    platform::ProcessSample s0 = platform::sampleSelf();
    platform::intervalPeakFootprint();
    double maxFootprint = 0, maxCache = 0, maxUsed = 0;
    int frames = 0;
    while (vb->value() < vb->maximum() && frames < 5000) {
        viewerFrame(v, qMin(vb->maximum(), vb->value() + step), QStringLiteral("pdf_scroll_down_frame_ms"));
        maxCache = qMax(maxCache, double(media->cacheBytes()));
        maxUsed = qMax(maxUsed, double(media->budget().used()));
        if (++frames % 10 == 0) {
            const double f = footprint();
            maxFootprint = qMax(maxFootprint, f);
            add(QStringLiteral("pdf_scroll_down_footprint_bytes"), f, QStringLiteral("bytes"), counterName);
        }
    }
    waitViewport(v, QStringLiteral("long_down_end"), 30000);
    media->waitIdle();
    m_extra.insert(QStringLiteral("down_frames"), frames);
    m_extra.insert(QStringLiteral("down_footprint_max_sampled"), maxFootprint);
    m_extra.insert(QStringLiteral("down_footprint_peak"), double(platform::intervalPeakFootprint()));
    m_extra.insert(QStringLiteral("down_footprint_end"), footprint());
    m_extra.insert(QStringLiteral("down_media_cache_max"), maxCache);
    m_extra.insert(QStringLiteral("down_media_used_max"), maxUsed);
    putCounters(QStringLiteral("down"), PdfCounters::now() - c0);
    putCpu(QStringLiteral("down"), s0, platform::sampleSelf());
    // Straight back up.
    c0 = PdfCounters::now();
    s0 = platform::sampleSelf();
    frames = 0;
    while (vb->value() > 0 && frames < 5000) {
        viewerFrame(v, qMax(0, vb->value() - step), QStringLiteral("pdf_scroll_up_frame_ms"));
        ++frames;
    }
    waitViewport(v, QStringLiteral("long_up_end"), 30000);
    media->waitIdle();
    m_extra.insert(QStringLiteral("up_frames"), frames);
    m_extra.insert(QStringLiteral("up_footprint_end"), footprint());
    putCounters(QStringLiteral("up"), PdfCounters::now() - c0);
    putCpu(QStringLiteral("up"), s0, platform::sampleSelf());
    // A second pass down: the plateau holds (no growth with pages visited).
    frames = 0;
    double secondMax = 0;
    while (vb->value() < vb->maximum() && frames < 5000) {
        viewerFrame(v, qMin(vb->maximum(), vb->value() + step), QStringLiteral("pdf_scroll_down2_frame_ms"));
        if (++frames % 10 == 0)
            secondMax = qMax(secondMax, footprint());
    }
    media->waitIdle();
    m_extra.insert(QStringLiteral("down2_footprint_max_sampled"), secondMax);
    m_extra.insert(QStringLiteral("down2_footprint_end"), footprint());
    // Zoom in and out repeatedly around page 100: gestures draw cached
    // pixels scaled and render once they settle.
    v->goToPage(100);
    waitViewport(v, QStringLiteral("zoom_start"), 30000);
    media->waitIdle();
    c0 = PdfCounters::now();
    s0 = platform::sampleSelf();
    int gestureRenders = 0, settles = 0;
    double zoomPeakUsed = 0;
    QPointF anchor(v->viewport()->width() / 2.0, v->viewport()->height() / 3.0);
    for (int round = 0; round < zoomRounds; ++round) {
        const qreal factor = round % 2 == 0 ? 1.12 : 1 / 1.12;
        const PdfCounters g0 = PdfCounters::now();
        const int running = media->runningJobs();
        for (int i = 0; i < 12; ++i) { // ~200 ms of pinching at 60 Hz
            const int before = v->paintCount();
            QElapsedTimer t;
            t.start();
            v->zoomGesture(factor, anchor);
            if (ensureViewerPaint(v, before, QStringLiteral("pdf_zoom_gesture_frame_ms")))
                add(QStringLiteral("pdf_zoom_gesture_frame_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"),
                    QStringLiteral("one pinch step to the end of the viewer paint (cached pixels scaled)"));
            settle(qMax<qint64>(0, 16 - t.elapsed()));
        }
        gestureRenders += int((PdfCounters::now() - g0).renders) - running; // a render already running finishes
        const double ms = waitViewport(v, QStringLiteral("pdf_zoom_settle"), 60000);
        if (ms >= 0) {
            add(QStringLiteral("pdf_zoom_settle_to_sharp_ms"), ms, QStringLiteral("ms"),
                QStringLiteral("end of the gesture to a fully rendered viewport at the new zoom (includes the settle delay)"));
            ++settles;
        }
        zoomPeakUsed = qMax(zoomPeakUsed, double(media->budget().used()));
    }
    media->waitIdle();
    m_extra.insert(QStringLiteral("zoom_rounds"), zoomRounds);
    m_extra.insert(QStringLiteral("zoom_rounds_settled"), settles);
    m_extra.insert(QStringLiteral("zoom_renders_started_during_gestures"), double(qMax(0, gestureRenders)));
    m_extra.insert(QStringLiteral("zoom_media_used_max"), zoomPeakUsed);
    m_extra.insert(QStringLiteral("zoom_footprint_end"), footprint());
    putCounters(QStringLiteral("zoom"), PdfCounters::now() - c0);
    putCpu(QStringLiteral("zoom"), s0, platform::sampleSelf());
    // High zoom, then scroll: tiles only, bounded.
    v->setZoom(4.0);
    m_extra.insert(QStringLiteral("high_zoom_first_viewport_ms"), waitViewport(v, QStringLiteral("high_zoom"), 30000));
    c0 = PdfCounters::now();
    for (int i = 0; i < 60 && vb->value() < vb->maximum(); ++i)
        viewerFrame(v, qMin(vb->maximum(), vb->value() + step), QStringLiteral("pdf_high_zoom_scroll_frame_ms"));
    media->waitIdle();
    putCounters(QStringLiteral("high_zoom"), PdfCounters::now() - c0);
    m_extra.insert(QStringLiteral("high_zoom_footprint_end"), footprint());
    v->fitWidth();
    media->waitIdle();
    // Closing returns the bitmaps and the document.
    const double beforeClose = footprint();
    const double cacheBeforeClose = double(media->cacheBytes());
    m_app->closePdf();
    media->waitIdle();
    settle(500);
    m_extra.insert(QStringLiteral("long_close_footprint_before"), beforeClose);
    m_extra.insert(QStringLiteral("long_close_footprint_after"), footprint());
    m_extra.insert(QStringLiteral("long_close_cache_before"), cacheBeforeClose);
    m_extra.insert(QStringLiteral("long_close_cache_after"), double(media->cacheBytes()));
    m_extra.insert(QStringLiteral("long_close_documents_open"), platform::PdfDocument::liveCount());

    // 5. Scanned, vector-heavy and transparency-heavy documents.
    for (const QString &name : {QStringLiteral("scanned-24"), QStringLiteral("vector-16"), QStringLiteral("transparency-16")}) {
        const AttachmentInfo info = infos.value(name);
        if (!info.isPdf())
            continue;
        const QString kind = name.section(QLatin1Char('-'), 0, 0);
        media->trim(0);
        instr::takeSamples();
        const double f0 = footprint();
        platform::intervalPeakFootprint();
        const PdfCounters k0 = PdfCounters::now();
        const platform::ProcessSample ks0 = platform::sampleSelf();
        PdfView *kv = openViewer(info, name + QStringLiteral(".pdf"));
        if (!kv)
            continue;
        m_extra.insert(kind + QStringLiteral("_first_viewport_ms"), waitViewport(kv, kind + QStringLiteral("_open"), 120000));
        QScrollBar *kb = kv->verticalScrollBar();
        int kf = 0;
        double kmax = 0;
        while (kb->value() < kb->maximum() && kf < 2000) {
            viewerFrame(kv, qMin(kb->maximum(), kb->value() + step), QStringLiteral("pdf_") + kind + QStringLiteral("_scroll_frame_ms"));
            if (++kf % 5 == 0)
                kmax = qMax(kmax, footprint());
        }
        m_extra.insert(kind + QStringLiteral("_last_viewport_ms"), waitViewport(kv, kind + QStringLiteral("_end"), 120000));
        media->waitIdle(120000);
        m_extra.insert(kind + QStringLiteral("_scroll_frames"), kf);
        m_extra.insert(kind + QStringLiteral("_footprint_before"), f0);
        m_extra.insert(kind + QStringLiteral("_footprint_max_sampled"), kmax);
        m_extra.insert(kind + QStringLiteral("_footprint_peak"), double(platform::intervalPeakFootprint()));
        m_extra.insert(kind + QStringLiteral("_footprint_open_end"), footprint());
        putCounters(kind, PdfCounters::now() - k0);
        putCpu(kind, ks0, platform::sampleSelf());
        const auto samples = instr::takeSamples();
        for (qint64 us : samples.value(QStringLiteral("pdf_render_us")))
            add(QStringLiteral("pdf_render_ms_") + kind, us / 1000.0, QStringLiteral("ms"),
                QStringLiteral("one render job on the PDF lane (wall)"));
        m_app->closePdf();
        media->waitIdle();
        settle(500);
        m_extra.insert(kind + QStringLiteral("_footprint_after_close"), footprint());
    }

    // 6. Idle with the viewer open and settled; then memory pressure, and
    // scrolling on during the cool-down.
    v = openViewer(longInfo, QStringLiteral("long-200.pdf"));
    if (v) {
        vb = v->verticalScrollBar();
        waitViewport(v, QStringLiteral("idle_viewer_open"), 30000);
        media->waitIdle();
        idleWindow(QStringLiteral("idle_viewer"));
        const double beforePressure = footprint();
        m_app->reclaimMemory(true);
        m_extra.insert(QStringLiteral("pressure_cache_after_trim"), double(media->cacheBytes()));
        m_extra.insert(QStringLiteral("pressure_footprint_after_trim"), footprint());
        m_extra.insert(QStringLiteral("pressure_budget_limit"), double(media->budget().limit()));
        c0 = PdfCounters::now();
        maxUsed = 0;
        for (int i = 0; i < 150 && vb->value() < vb->maximum(); ++i) {
            viewerFrame(v, qMin(vb->maximum(), vb->value() + step), QStringLiteral("pdf_pressure_frame_ms"));
            maxUsed = qMax(maxUsed, double(media->budget().used()));
        }
        m_extra.insert(QStringLiteral("pressure_last_viewport_ms"), waitViewport(v, QStringLiteral("pressure_end"), 30000));
        media->waitIdle();
        putCounters(QStringLiteral("pressure"), PdfCounters::now() - c0);
        m_extra.insert(QStringLiteral("pressure_media_used_max"), maxUsed);
        m_extra.insert(QStringLiteral("pressure_footprint_before"), beforePressure);
        m_extra.insert(QStringLiteral("pressure_still_cooling"), media->budget().coolingDown());
        const PdfView::Plan pl = v->planFor(v->currentPage(), v->zoom());
        m_extra.insert(QStringLiteral("pressure_plan"), QStringLiteral("%1 %2x%3").arg(pl.tiles ? QStringLiteral("tiles") : QStringLiteral("page"))
                                                         .arg(pl.width).arg(pl.height));
        // 7. Closed: idle again.
        m_app->closePdf();
    }
    media->waitIdle();
    idleWindow(QStringLiteral("idle_after_close"));
    m_extra.insert(QStringLiteral("documents_open_at_end"), platform::PdfDocument::liveCount());
    for (auto it = unpainted.cbegin(); it != unpainted.cend(); ++it)
        m_extra.insert(it.key() + QStringLiteral("_unpainted_frames"), it.value());
    m_extra.insert(QStringLiteral("offscreen_canvas_bytes"), double(canvas.sizeInBytes()));
    addInstrSamples();
}

} // namespace folia
