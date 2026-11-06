# Images and PDFs: design (revision 2)

Status: proposal, not implemented. Revision 2 takes in an outside review of the
first draft and adds facts checked against this repository on 2026-11-06. Every
number below is a **target for a benchmark**, not a measurement.

## 0. Facts that constrain the design

- **The shipped bundle has no Qt image-format plugins.** `dist/Folia.app` ships
  only `platforms/libqcocoa` and `styles/libqmacstyle`. In the shipped app,
  `QImageReader` reads only the formats built into QtGui (PNG, BMP, PPM, XBM,
  XPM). JPEG is not one of them. The Homebrew Qt 6.12 has plugins only for
  gif, ico and jpeg. `qtimageformats` (WebP, HEIC) is not installed. The first
  draft's claim that "QImageReader reads HEIC through ImageIO" was wrong for
  this build.
- **QtPdf is not installed.** The app links only Core, Gui and Widgets.
- **Deployment target is macOS 14.4.** ImageIO there decodes JPEG, PNG, HEIC,
  WebP, GIF and TIFF. CoreGraphics renders PDF. Neither adds any bundle size.
- **Existing pieces to reuse**: `PreviewService` (supersession per block, a
  byte-capped LRU, `trim()`), `VirtualLayout::evictFarBlocks`,
  `platform::onMemoryPressure` → `AppController::reclaimMemory`, `Instrument`
  counters, `BenchDriver`.

Consequence: on macOS, **ImageIO decodes images and CoreGraphics renders PDFs**,
both through `MacPlatform.mm`. `QImageReader` is the portable fallback, and
QtPdf (PDFium) is the fallback for other platforms. This settles the renderer
question the first draft left open.

## 1. Storage

- Originals are files under `Application Support/Folia/attachments/<sha256>`,
  content-addressed. SQLite holds only metadata:
  `attachments(hash, mime, bytes, width, height, orientation, pages, page_geom)`.
  - `width` and `height` are **after the orientation transform**, so EXIF
    rotation never causes a layout change later.
  - `page_geom` is a compact blob: one `(w, h, rotation)` per PDF page, since
    pages differ in size.
  - All metadata is collected on a worker during import, never on the UI
    thread.
- **Import never blocks typing and never reads the whole file into RAM.**
  1. A worker streams the source to a temp file and hashes it as it copies.
  2. It fsyncs, then renames the file to its hash.
  3. It reads the metadata.
  4. Only then does the block's `attachment:<hash>` reference go through
     `applyEdit`.

  Until step 4 the block shows a placeholder that is not serialized, so a page
  is never saved pointing at a missing file. Durable ordering is kept: file
  first, reference second. A pasted in-memory image is encoded to PNG on the
  worker.
- **Garbage collection runs in batches, from references, never "when a page is
  trashed".** A file is deleted only when nothing references it: no live page,
  no trashed page, and no open session's undo stack. The sweep runs at idle,
  well after the last reference goes away.
- Markdown forms: `![alt](attachment:hash)` and `[file.pdf](attachment:hash)`.
  In the document, each is one U+FFFC line, like page links.

## 2. One media budget, not one cap per cache

A cache cap does not bound peak memory. A decoder can allocate the full-size
bitmap before scaling it down. A PDF render allocates embedded images, fonts and
transparency groups outside any cache. So all media work goes through one
`MediaBudget`, shared by images, PDF and (later) math:

```
media bytes = cached pixels + reserved in-flight working sets + display copies
```

- Before starting a job, the scheduler **reserves** an estimate of its working
  set: output bytes, plus a per-backend overhead factor that benchmarks
  calibrate. When the job finishes, the reservation is released and replaced by
  the real cached size.
- If a job does not fit, it waits. If its estimate alone exceeds the whole
  budget, it is not run: the block shows "too large to preview" at its stored
  size. Under `QImageReader`, `QImageReader::setAllocationLimit` stays on as a
  second guard. It is not a process-wide limit.
- Memory pressure trims caches toward 0 (critical) or a floor (warning) and
  **lowers the budget for a cool-down period**. Without that, caches would grow
  straight back while the system is still under pressure.
- **No pixel references outlive eviction.** `QImage` shares its pixel data, so
  dropping a cache entry frees nothing while a copy lives elsewhere. So:
  - Blocks store only a cache key. Paint looks the key up and draws, and keeps
    no copy.
  - Completion lambdas drop their image immediately when the target is gone.
  - Debug builds assert `QImage::isDetached()`-style uniqueness on eviction.

Opaque renderer allocations (ImageIO and CG internals) cannot be reserved
exactly. The overhead factor plus headroom covers them, and the peak-footprint
benchmark checks that it is enough.

## 3. Images

- **macOS decoder: `CGImageSourceCreateThumbnailAtIndex`**, with:
  - `kCGImageSourceThumbnailMaxPixelSize = max(targetW, targetH)`;
  - `kCGImageSourceCreateThumbnailFromImageAlways`;
  - `kCGImageSourceCreateThumbnailWithTransform` (orientation);
  - `kCGImageSourceShouldCacheImmediately`.

  A max pixel size bounds **both** dimensions, so a 1000×40000 screenshot cannot
  allocate a giant bitmap. Whether each format decodes at reduced cost or
  decodes in full and then scales is **measured per format** (JPEG, PNG, HEIC,
  WebP). It is not assumed. The overhead factor in §2 is set per format from
  those measurements.
- **Output size is limited in total pixels as well as width.** The target is the
  physical width (`column width × DPR`, capped at native width). Height follows
  from the aspect ratio. Total pixels are clamped (start at 16 MP). Tall images
  past that limit are decoded at a lower density and drawn scaled up.
- **Fallback decoder: `QImageReader`**, with `setScaledSize` and
  `setAutoTransform(true)`. `setScaledSize` may decode in full and then scale,
  depending on the format handler. The budget assumes the worst case for any
  format not yet measured.
- **Cache key**: hash, physical width, physical height, orientation, colour
  handling. At most 2 resolutions are kept per attachment.
- **Eviction does not mean "decode again".** When `evictFarBlocks` clears a
  block, it drops only the block's key. The shared LRU keeps recent bitmaps
  while the budget allows, so scrolling down and back up hits the cache. Memory
  pressure evicts aggressively.
- **Scheduling:**
  - There is one pending decode per cache key. Blocks that show the same
    attachment attach to it.
  - The queue is visible-first and owned by the service, not by
    `QThreadPool::start`. Requests for blocks that scroll out of the keep window
    are removed **before they start**.
  - Directional prefetch covers at most one viewport ahead. It runs only when
    the worker is idle and the budget is under half used.
- **Resize:**
  - While the pane divider moves, the existing bitmap is drawn scaled (fast
    transform).
  - One decode at the new size runs after the resize settles (coalesced, around
    150 ms).
  - If the new physical width is within ±10 % of a cached resolution, that
    bitmap is reused and drawn smooth-scaled, and nothing is decoded.
- **Paint**: no file I/O, decoding or format conversion inside paint. Images are
  cached in the format the painter blits fastest (`ARGB32_Premultiplied` /
  `RGB32`). Large bitmaps still cost memory bandwidth per repaint, so only
  damaged regions repaint when a result arrives.
- Static only: the first frame of GIF/APNG, with no animation timers.
- **A disk-cached display preview** (a downscaled copy kept beside the
  original) is deferred. It is worth adding only if benchmarks show repeated
  expensive decodes (huge PNGs, HEIC). It is never generated upfront for every
  import.

## 4. PDF

- **In the note**: a first-page thumbnail (one small bitmap through the same
  scheduler and budget), plus the filename and page count.
- **The viewer** takes over the editor slot, like Settings. It is one custom
  viewport, not a widget per page:
  - Page offsets come from `page_geom`, with a prefix sum and a binary search
    for the visible pages.
  - Invalidation is kept separate from the note editor, so typing or the caret
    blink never repaints the PDF.
- **Render strategy by output size:**

  | Output needed | Strategy |
  |---|---|
  | Thumbnail | One bitmap |
  | Normal zoom, page bitmap fits easily in the budget | One page bitmap |
  | High zoom or very large page | Visible tiles only |
  | Pinch or zoom gesture in progress | Existing pixels scaled, refined after it settles |

  Each render call re-walks the page's content stream (QtPdf even reopens the
  page per call), so tiling a page that would fit as one bitmap costs extra CPU.
  When tiles are used:
  - They are bounded in **both** physical dimensions. Starting sizes are
    512×512 (1 MiB) and 1024×1024 (4 MiB); the benchmark picks one.
  - Each tile renders straight into a tile-sized context through a transform
    and clip, never as a full page that is then cropped.
  - Tiles bound the destination bitmap only. Renderer-internal memory is
    covered by the §2 overhead factor.
- **Release**: the document handle closes when the viewer closes, or after it
  has been hidden for a while. Text extraction (search, selection) is a later
  milestone, done lazily per page.

## 5. Workers and cancellation

- One scheduler and one budget, with **two lanes**: at most one image decode
  and at most one PDF render at a time. They run concurrently only when both
  reservations fit, so one slow PDF page never holds up every image.
- PDF calls are serialized per document: CGPDFDocument is not documented as
  safe for concurrent drawing, and PDFium requires single-threaded access.
  Adding PDF threads would not add safe parallelism.
- **Cancellation is layered**, because neither ImageIO, `QImageReader` nor
  `CGContextDrawPDFPage` can be interrupted mid-call:
  1. Stale requests are dropped from the queue before they start. This is the
     main mechanism.
  2. Between steps of a job (tile N → tile N+1), the job checks whether it is
     still wanted.
  3. Obsolete results are thrown away: no repaint, no caching beyond the LRU
     rules.
  4. Cooperative interruption is used only where a backend offers it. PDFium's
     progressive render has a pause callback; consider it only if profiling
     shows long, uninterruptible renders hurting.

  Throwing a result away does not refund the CPU spent making it, so wasted CPU
  is measured (§6).
- **Worker expiry**: the first draft copied `PreviewService`'s 2 s expiry "for
  zero wakeups". That reasoning is wrong. A parked pool thread waits on a
  condition variable and wakes only once, to exit, whatever the timeout.
  Thread creation (tens of µs) is also tiny next to a decode (ms). The timeout
  only trades a parked thread's stack against that creation cost. Start at
  Qt's 30 s default, so threads are not recreated after every reading pause.
  Keep it only if the idle benchmark shows no regression in footprint or thread
  count.

## 6. Instrumentation and benchmarks

New `Instrument` counters and gauges:

- `media_budget_bytes`, `media_reserved_bytes`, `media_cache_bytes`
- `image_decodes`, `image_cache_hits`, `pdf_renders`, `pdf_tiles`
- `media_jobs_dropped_queued` (cancelled before start)
- `media_jobs_discarded` and `media_cpu_us_discarded` (finished but unwanted)
- `media_budget_waits`, `media_too_large`

Each scenario records:

- peak footprint
- total CPU time
- input latency
- cache hit rate
- CPU time spent on discarded work
- extra wakeups caused by attachments after the work settles

| Scenario | What it shows |
|---|---|
| Image-heavy page: idle, then scrolled | Plateau; whether idle returns to the baseline |
| Scroll down, then immediately back up | Decodes repeated because of eviction |
| Drag the pane divider continuously | Decodes caused by size changes; coalescing |
| Paste or import a 50 MB photo while typing | Import blocking input; RAM held during the copy |
| Huge PNG, very tall screenshot, HEIC, WebP | Full-resolution fallback; dimension limits; per-format overhead factor |
| 200-page PDF: open, scroll, zoom repeatedly | Obsolete renders, temporary allocations, duplicate cache entries |
| Scanned, vector-heavy and transparency-heavy PDFs | Renderer cost that plain text PDFs hide |
| Type while a PDF renders beside the note | UI contention; unnecessary repaints |
| Trigger memory pressure, then keep scrolling | Caches stay low during cool-down, and rendering does not stall |

Corrections to the first draft's figures:

- A 4:3 image at 700 logical px × 2 DPR is 1400×1050×4 B ≈ **5.6 MiB**, so a
  32 MiB cache holds about **five**, not eight. The draft's "12 MP → 8 MB" also
  contradicted itself.
- "CoreGraphics uses the least memory" and "a long PDF takes 20–60 MB" are
  targets to demonstrate on comparable documents. Page count alone does not
  predict rendering cost.

## 7. Order of work

1. Attachment store, streaming import, reference-based GC.
2. `MediaBudget` plus the two-lane scheduler, with counters, before any decoder
   exists.
3. Image block with the ImageIO decoder. Run the image benchmarks and calibrate
   the overhead factors.
4. PDF thumbnail, then the viewer with the CoreGraphics backend. Run the PDF
   benchmarks and choose the tile size and the whole-page threshold.
5. QtPdf and `QImageReader` backends for other platforms.
