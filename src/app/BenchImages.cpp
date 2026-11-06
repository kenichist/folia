// The image benchmark scenario (docs/ATTACHMENTS.md §6, docs/BENCHMARKS.md):
//
//   --bench images [--bench-arg photos=24] [--bench-arg idle_seconds=20]
//                  [--bench-arg media_dir=<dir>] [--bench-arg step=40]
//
// Builds an image-heavy page through the real import path, then measures:
// idle on a text page (baseline) and on the image page; one continuous scroll
// down (footprint plateau, decodes, dropped/discarded work) and straight back
// up (cache hits vs repeated decodes); a continuous window resize (decodes
// caused by size changes, coalescing); a 36 MB PNG imported while typing
// (input latency, RAM during the copy); the decode cost of each format; and
// scrolling under a lowered budget after memory pressure.
#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "app/MediaFixtures.h"
#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorPane.h"
#include "editor/MediaService.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/VirtualLayout.h"
#include "storage/AttachmentStore.h"
#include "storage/Store.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMainWindow>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace folia {

namespace {

struct MediaCounters {
    qint64 decodes, hits, dropped, discarded, cpuDiscarded, waits, tooLarge;
    static MediaCounters now()
    {
        return {instr::get(instr::ImageDecodes), instr::get(instr::ImageCacheHits),
                instr::get(instr::MediaJobsDroppedQueued), instr::get(instr::MediaJobsDiscarded),
                instr::get(instr::MediaCpuUsDiscarded), instr::get(instr::MediaBudgetWaits),
                instr::get(instr::MediaTooLarge)};
    }
    MediaCounters operator-(const MediaCounters &o) const
    {
        return {decodes - o.decodes, hits - o.hits, dropped - o.dropped, discarded - o.discarded,
                cpuDiscarded - o.cpuDiscarded, waits - o.waits, tooLarge - o.tooLarge};
    }
};

const char *kLorem = "Notes between the pictures: what was taken where, and why it matters later on. ";

} // namespace

void BenchDriver::scenarioImages()
{
    MediaService *media = m_app->media();
    AttachmentStore *as = m_app->attachments();
    NoteEditor *ed = m_app->editorPane()->editor();
    if (!media || !as) {
        m_notes.append(QStringLiteral("no media service"));
        return;
    }
    const QString dir = m_args.value(QStringLiteral("media_dir"), QDir(m_app->homeDir()).filePath(QStringLiteral("bench-media"))).toString();
    const int photos = m_args.value(QStringLiteral("photos"), 24).toInt();
    const int idleSeconds = m_args.value(QStringLiteral("idle_seconds"), 20).toInt();
    const int settleMs = m_args.value(QStringLiteral("settle_ms"), 3000).toInt();
    const int step = m_args.value(QStringLiteral("step"), 40).toInt();
    QElapsedTimer gen;
    gen.start();
    const fixtures::MediaSet set = fixtures::makeMediaSet(dir, photos, true);
    m_extra.insert(QStringLiteral("fixtures_ms"), double(gen.elapsed()));
    if (gen.elapsed() > 500)
        m_notes.append(QStringLiteral("fixtures were generated in this process (run `folia_mediaprobe --make <dir> <photos>` "
                                      "first): footprints are inflated by the encoder's freed memory"));
    const QString counterName = platform::sampleSelf().counterName;
    m_extra.insert(QStringLiteral("media_budget_bytes"), double(media->budget().base()));
    m_extra.insert(QStringLiteral("media_cache_max_bytes"), double(media->config().cacheMaxBytes));

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
            add(prefix + QStringLiteral("_footprint_bytes"), double(s.footprintBytes), QStringLiteral("bytes"), counterName);
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
    auto putCounters = [&](const QString &prefix, const MediaCounters &d) {
        m_extra.insert(prefix + QStringLiteral("_decodes"), double(d.decodes));
        m_extra.insert(prefix + QStringLiteral("_cache_hits"), double(d.hits));
        m_extra.insert(prefix + QStringLiteral("_cache_hit_rate"), d.hits + d.decodes > 0 ? double(d.hits) / double(d.hits + d.decodes) : 1.0);
        m_extra.insert(prefix + QStringLiteral("_jobs_dropped_queued"), double(d.dropped));
        m_extra.insert(prefix + QStringLiteral("_jobs_discarded"), double(d.discarded));
        m_extra.insert(prefix + QStringLiteral("_cpu_ms_discarded"), d.cpuDiscarded / 1000.0);
        m_extra.insert(prefix + QStringLiteral("_budget_waits"), double(d.waits));
        m_extra.insert(prefix + QStringLiteral("_too_large"), double(d.tooLarge));
    };
    QScrollBar *vb = ed->verticalScrollBar();
    // macOS does not paint a covered window (or one on another Space), and
    // without paints nothing is wanted or decoded. Such frames are counted
    // and, unless offscreen_fallback=0, painted through the same code into a
    // reused offscreen image, so decode and memory behaviour stay measured
    // (their timings are left out).
    const bool fallback = m_args.value(QStringLiteral("offscreen_fallback"), 1).toInt() != 0;
    QImage canvas;
    QHash<QString, int> unpainted;
    auto ensurePaint = [&](int before, const QString &phase) {
        if (waitFor([&] { return m_paints > before; }, 100))
            return true;
        ++unpainted[phase];
        if (fallback) {
            if (canvas.isNull()) {
                const qreal dpr = m_app->window()->devicePixelRatioF();
                canvas = QImage(ed->viewport()->size() * dpr, QImage::Format_ARGB32_Premultiplied);
                canvas.setDevicePixelRatio(dpr);
                m_notes.append(QStringLiteral("window not painted by the system: some frames painted offscreen "
                                              "(see *_unpainted_frames; their timings are excluded)"));
            }
            if (canvas.size() != ed->viewport()->size() * canvas.devicePixelRatio()) {
                const qreal dpr = canvas.devicePixelRatio();
                canvas = QImage(ed->viewport()->size() * dpr, QImage::Format_ARGB32_Premultiplied);
                canvas.setDevicePixelRatio(dpr);
            }
            ed->viewport()->render(&canvas);
        }
        return false;
    };
    // One display-paced frame: move, wait for the paint, record the work.
    auto frame = [&](int to, const QString &metric) {
        const int before = m_paints;
        QElapsedTimer t;
        t.start();
        vb->setValue(to);
        if (ensurePaint(before, metric))
            add(metric, t.nsecsElapsed() / 1e6, QStringLiteral("ms"),
                QStringLiteral("scroll step to the end of the next editor paint (main thread, includes waiting for the frame)"));
    };

    // 0. Decode cost per format, in-process on this thread, before anything
    // else runs (the peak is process-wide).
    const qreal dpr = m_app->window()->devicePixelRatioF();
    const qreal col = 760; // the default theme's column
    struct Fmt { QString name, path; };
    QVector<Fmt> fmts = {{QStringLiteral("jpeg"), set.photos.value(0)}, {QStringLiteral("heic"), set.heic},
                         {QStringLiteral("webp"), set.webp}, {QStringLiteral("png_screenshot"), set.screenshots.value(0)},
                         {QStringLiteral("png_tall"), set.tall}, {QStringLiteral("png_huge"), set.hugePng}};
    for (const Fmt &f : std::as_const(fmts)) {
        if (f.path.isEmpty())
            continue;
        const platform::ImageInfo ii = platform::imageInfo(f.path);
        AttachmentInfo ai;
        ai.hash = QStringLiteral("probe");
        ai.mime = ii.mime;
        ai.width = ii.width;
        ai.height = ii.height;
        const MediaService::Request r = media->imageRequest(ai, f.path, qMin<qreal>(col, ii.width), dpr);
        for (int rep = 0; rep < 3; ++rep) {
            platform::intervalPeakFootprint();
            const qint64 before = platform::sampleSelf().footprintBytes;
            platform::intervalPeakFootprint();
            QElapsedTimer t;
            t.start();
            QImage img = platform::decodeImage(f.path, qMax(r.key.width, r.key.height));
            const double ms = t.nsecsElapsed() / 1e6;
            const double peak = double(platform::intervalPeakFootprint() - before);
            add(QStringLiteral("decode_ms_") + f.name, ms, QStringLiteral("ms"));
            add(QStringLiteral("decode_peak_bytes_") + f.name, peak, QStringLiteral("bytes"),
                QStringLiteral("rusage interval max footprint growth during one decode (main thread)"));
            add(QStringLiteral("decode_estimate_over_peak_") + f.name, peak > 0 ? double(r.estimate) / peak : 0,
                QStringLiteral("ratio"), QStringLiteral("reservation / measured peak; >= 1 means the reservation covered it"));
            m_extra.insert(QStringLiteral("decode_size_") + f.name,
                           QStringLiteral("%1x%2 from %3x%4").arg(img.width()).arg(img.height()).arg(ii.width).arg(ii.height));
        }
    }

    // 1. Baseline: a text-only page.
    m_app->newPage(0, -1, QStringLiteral("Text"), false);
    for (int i = 0; i < 40; ++i)
        ed->textCursor().insertText(QString::fromUtf8(kLorem) + QStringLiteral("\n"));
    m_app->flushSaves();
    idleWindow(QStringLiteral("idle_text"));

    // 2. The image page, built through the real import path (files streamed
    // by the worker, references inserted after each file is stored).
    m_app->newPage(0, -1, QStringLiteral("Images"), false);
    QStringList order = set.photos;
    for (const QString &p : {set.heic, set.webp})
        if (!p.isEmpty())
            order.insert(order.size() / 2, p);
    for (int i = 0; i < set.screenshots.size(); ++i)
        order.insert(qMin<int>(order.size(), 3 + i * 7), set.screenshots[i]);
    order.insert(order.size() / 3, set.tall);
    order.removeAll(QString());
    QElapsedTimer imp;
    imp.start();
    for (const QString &path : std::as_const(order)) {
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n") + QString::fromUtf8(kLorem) + QString::fromUtf8(kLorem));
        ed->setTextCursor(c);
        AttachmentStore::Source s;
        s.path = path;
        s.name = QFileInfo(path).completeBaseName();
        ed->importImages({s});
        as->waitForImports();
    }
    m_extra.insert(QStringLiteral("images_on_page"), int(order.size()));
    m_extra.insert(QStringLiteral("import_all_ms"), double(imp.elapsed()));
    m_app->flushSaves();
    m_app->store()->flush(10000);
    PageSession *ses = ed->session();
    int imageLines = 0;
    for (QTextBlock b = ses->document()->firstBlock(); b.isValid(); b = b.next())
        imageLines += PageSession::roleOf(b) == LineRole::Image ? 1 : 0;
    m_extra.insert(QStringLiteral("image_lines"), imageLines);
    media->trim(0); // measured phases start cold

    // 3. Open state: the visible images decode, then the page idles.
    vb->setValue(0);
    {
        const int before = m_paints;
        ed->viewport()->update();
        ensurePaint(before, QStringLiteral("open"));
    }
    settle(200);
    media->waitIdle();
    m_extra.insert(QStringLiteral("open_cache_bytes"), double(media->cacheBytes()));
    idleWindow(QStringLiteral("idle_images"));

    // 4. Scroll down through the whole page, continuously.
    instr::takeSamples();
    MediaCounters c0 = MediaCounters::now();
    platform::intervalPeakFootprint();
    double maxFootprint = 0, maxCache = 0, maxUsed = 0;
    int frames = 0;
    while (vb->value() < vb->maximum() && frames < 5000) {
        frame(qMin(vb->maximum(), vb->value() + step), QStringLiteral("images_scroll_down_frame_ms"));
        maxCache = qMax(maxCache, double(media->cacheBytes()));
        maxUsed = qMax(maxUsed, double(media->budget().used()));
        if (++frames % 10 == 0) {
            const double f = footprint();
            maxFootprint = qMax(maxFootprint, f);
            add(QStringLiteral("images_scroll_down_footprint_bytes"), f, QStringLiteral("bytes"), counterName);
        }
    }
    media->waitIdle();
    m_extra.insert(QStringLiteral("down_frames"), frames);
    m_extra.insert(QStringLiteral("down_footprint_max_sampled"), maxFootprint);
    m_extra.insert(QStringLiteral("down_footprint_peak"), double(platform::intervalPeakFootprint()));
    m_extra.insert(QStringLiteral("down_footprint_end"), footprint());
    m_extra.insert(QStringLiteral("down_media_cache_max"), maxCache);
    m_extra.insert(QStringLiteral("down_media_used_max"), maxUsed);
    putCounters(QStringLiteral("down"), MediaCounters::now() - c0);

    // 5. Straight back up: what eviction made decode again.
    c0 = MediaCounters::now();
    frames = 0;
    while (vb->value() > 0 && frames < 5000) {
        frame(qMax(0, vb->value() - step), QStringLiteral("images_scroll_up_frame_ms"));
        ++frames;
    }
    media->waitIdle();
    m_extra.insert(QStringLiteral("up_frames"), frames);
    m_extra.insert(QStringLiteral("up_footprint_end"), footprint());
    putCounters(QStringLiteral("up"), MediaCounters::now() - c0);

    // 6. Continuous resize (a pane divider drag) to a narrower window:
    // decodes while it moves (should be none) and once it settles.
    vb->setValue(qMin(vb->maximum(), vb->value() + 2000));
    settle(300);
    media->waitIdle();
    QWidget *win = m_app->window();
    const QSize size0 = win->size();
    c0 = MediaCounters::now();
    for (int k = 1; k <= 40; ++k) {
        const int before = m_paints;
        QElapsedTimer t;
        t.start();
        win->resize(size0.width() - k * 10, size0.height());
        if (ensurePaint(before, QStringLiteral("images_resize_frame_ms")))
            add(QStringLiteral("images_resize_frame_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"));
    }
    const MediaCounters during = MediaCounters::now() - c0;
    settle(400);
    media->waitIdle();
    putCounters(QStringLiteral("resize"), MediaCounters::now() - c0);
    m_extra.insert(QStringLiteral("resize_decodes_during_drag"), double(during.decodes));
    m_extra.insert(QStringLiteral("resize_visible_images"), [&] {
        int n = 0;
        const qreal top = vb->value(), bottom = top + ed->viewport()->height();
        for (QTextBlock b = ses->document()->firstBlock(); b.isValid(); b = b.next())
            if (PageSession::roleOf(b) == LineRole::Image) {
                const QRectF r = ses->layout()->imageRect(b);
                n += r.bottom() > top && r.top() < bottom ? 1 : 0;
            }
        return n;
    }());
    const MediaCounters c1 = MediaCounters::now();
    for (int k = 1; k <= 20; ++k) { // a small back-and-forth: within ±10 %, reused
        const int before = m_paints;
        win->resize(size0.width() - 400 + (k % 2) * 30, size0.height());
        ensurePaint(before, QStringLiteral("small_resize"));
    }
    settle(400);
    media->waitIdle();
    m_extra.insert(QStringLiteral("small_resize_decodes"), double((MediaCounters::now() - c1).decodes));
    win->resize(size0);
    settle(400);
    media->waitIdle();

    // 7. A 36 MB PNG pasted while typing: input latency, and the RAM the
    // import holds while it copies (peak taken when the file is stored,
    // before the reference goes in and the image is decoded).
    if (!set.hugePng.isEmpty()) {
        QTextCursor c = ed->textCursor();
        c.movePosition(QTextCursor::End);
        ed->setTextCursor(c);
        instr::takeSamples();
        platform::intervalPeakFootprint();
        const qint64 before = platform::sampleSelf().footprintBytes;
        platform::intervalPeakFootprint();
        AttachmentStore::Source s;
        s.path = set.hugePng;
        s.name = QStringLiteral("huge");
        QElapsedTimer t;
        t.start();
        const int batch = ses->beginImport(c.position(), 1);
        bool stored = false;
        double storedMs = 0, storedPeak = 0;
        as->import(s, ses, [&](AttachmentStore::ImportResult r) {
            storedMs = t.nsecsElapsed() / 1e6;
            storedPeak = double(platform::intervalPeakFootprint() - before);
            ses->finishImport(batch, r.ok, r.info, r.alt);
            stored = true;
        });
        typeText(QString::fromUtf8(kLorem), 150, false);
        const double typedMs = t.nsecsElapsed() / 1e6;
        waitFor([&] { return stored; }, 30000);
        m_extra.insert(QStringLiteral("import_huge_png_stored_ms"), storedMs);
        m_extra.insert(QStringLiteral("import_huge_png_typing_ms"), typedMs);
        m_extra.insert(QStringLiteral("import_huge_png_peak_growth_bytes"), storedPeak);
        m_extra.insert(QStringLiteral("import_huge_png_file_bytes"), double(QFileInfo(set.hugePng).size()));
        const auto samples = instr::takeSamples();
        for (qint64 v : samples.value(QStringLiteral("input_to_paint_us")))
            add(QStringLiteral("import_typing_input_to_paint_us"), double(v), QStringLiteral("us"),
                QStringLiteral("key event to end of editor paint while a 36 MB PNG is imported"));
        media->waitIdle();
    }

    // 8. Memory pressure, then keep scrolling during the cool-down.
    vb->setValue(0);
    settle(300);
    media->waitIdle();
    const double beforePressure = footprint();
    m_app->reclaimMemory(true);
    m_extra.insert(QStringLiteral("pressure_cache_after_trim"), double(media->cacheBytes()));
    m_extra.insert(QStringLiteral("pressure_footprint_after_trim"), footprint());
    m_extra.insert(QStringLiteral("pressure_budget_limit"), double(media->budget().limit()));
    c0 = MediaCounters::now();
    maxUsed = 0;
    maxFootprint = 0;
    for (int i = 0; i < 300 && vb->value() < vb->maximum(); ++i) {
        frame(qMin(vb->maximum(), vb->value() + step), QStringLiteral("images_pressure_frame_ms"));
        maxUsed = qMax(maxUsed, double(media->budget().used()));
        if (i % 10 == 0)
            maxFootprint = qMax(maxFootprint, footprint());
    }
    media->waitIdle();
    putCounters(QStringLiteral("pressure"), MediaCounters::now() - c0);
    m_extra.insert(QStringLiteral("pressure_media_used_max"), maxUsed);
    m_extra.insert(QStringLiteral("pressure_footprint_before"), beforePressure);
    m_extra.insert(QStringLiteral("pressure_footprint_max_sampled"), maxFootprint);
    m_extra.insert(QStringLiteral("pressure_still_cooling"), media->budget().coolingDown());

    // 9. Idle again, with the page scrolled into the middle.
    idleWindow(QStringLiteral("idle_after"));
    for (auto it = unpainted.cbegin(); it != unpainted.cend(); ++it)
        m_extra.insert(it.key() + QStringLiteral("_unpainted_frames"), it.value());
    m_extra.insert(QStringLiteral("offscreen_canvas_bytes"), double(canvas.sizeInBytes()));
    addInstrSamples();
}

} // namespace folia
