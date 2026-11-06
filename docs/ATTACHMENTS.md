# Images and PDFs: design (revision 4)

Status: steps 1–4 of §7 are implemented (the attachment store, `MediaBudget`
with the two-lane scheduler, the image block on ImageIO, and PDFs on
CoreGraphics: the in-note card and the viewer). Step 5 (QtPdf and
`QImageReader` backends for other platforms) is not. Revision 2 took in an
outside review of the first draft and added facts checked against this
repository on 2026-11-06. Revision 3 replaced estimates with measurements
where the image benchmarks contradicted them; revision 4 does the same for
PDFs. Those places are marked **Measured** and the numbers are in
`docs/BENCHMARKS.md` §6. Numbers not marked that way are still targets.

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
  content-addressed. **Implemented:** the folder sits next to the workspace
  file (`<folder>/attachments` for `workspace.db`, `<name>.attachments` for
  any other workspace file), so two workspaces never sweep each other's
  files. SQLite holds only metadata (schema version 2):
  `attachments(hash, mime, bytes, width, height, orientation, pages, page_geom)`.
  - `width` and `height` are **after the orientation transform**, so EXIF
    rotation never causes a layout change later.
  - `page_geom` is a compact blob: one `(w, h, rotation)` per PDF page, since
    pages differ in size.
    - **Implemented:** w and h are the crop box in points *after* the
      rotation, so layout never needs the rotation. Identical pages are
      stored as one run, so a uniform document of any length is 21 bytes.
      Each page also carries two render-cost hints read from its resources
      at import (dictionary lookups, nothing drawn): the largest embedded
      image's pixel count (and whether it is JPEG/JPX), and whether the page
      uses transparency groups or soft masks. §4 says why.
    - The `width`/`height` columns of a PDF hold its first page's size in
      points (rotated), so the card's thumbnail is laid out like an image.
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
  - **Implemented:** an open session pins every hash it has shown since it
    opened, until it closes. That is a superset of what its undo stack can
    restore, without inspecting Qt's undo stack. An import pins its hash
    before the file appears under its name and hands the pin to the session.
  - The sweep is one writer-thread transaction, so no save interleaves: it
    scans `blocks` for references (live and trashed pages alike, since
    trashed pages keep their blocks), then deletes unpinned, unreferenced
    files and their rows, checking each pin under the same lock imports
    take. Files without a row and stale temp files are removed only after
    10 minutes. It runs 60 s after the last pin is released, and 120 s
    after launch if the folder is not empty.
- Markdown forms: `![alt](attachment:hash)` and `[file.pdf](attachment:hash)`.
  In the document, each is one U+FFFC line, like page links. **Implemented**
  for both; the GC sweep and page loads read both forms (a sweep that only
  knew the image form would delete every PDF).

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
  second guard. **Correction:** in Qt 6 it *is* a process-wide limit
  (`static`), so it is left at Qt's default rather than tuned per decode.
- Memory pressure trims caches toward 0 (critical) or a floor (warning) and
  **lowers the budget for a cool-down period**. Without that, caches would grow
  straight back while the system is still under pressure.
  - **Measured:** lowering the budget alone stalled rendering. With the budget
    at ¼ (32 MiB), every photo's estimate (~46 MB at 1520 px) waited for the
    whole 60 s cool-down. Request sizes now follow the *current* budget, so
    during a cool-down images decode at a lower density instead of not at
    all, and owners re-request full density when the cool-down ends.
- **No pixel references outlive eviction.** `QImage` shares its pixel data, so
  dropping a cache entry frees nothing while a copy lives elsewhere. So:
  - Blocks store only a cache key. Paint looks the key up and draws, and keeps
    no copy.
  - Completion lambdas drop their image immediately when the target is gone.
  - Debug builds assert `QImage::isDetached()`-style uniqueness on eviction.
  - **Measured:** unique ownership is not enough on macOS. Freed multi-MB
    malloc blocks stay in the footprint: fifteen 1520×1140 bitmaps freed
    leave 107 MB counted, and `malloc_zone_pressure_relief` releases none of
    them. So decoded bitmaps live in their own anonymous mapping
    (`platform::allocateImage`), unmapped when the cache drops them, and the
    footprint falls with the cache.

Opaque renderer allocations (ImageIO and CG internals) cannot be reserved
exactly. The overhead factor plus headroom covers them, and the peak-footprint
benchmark checks that it is enough.

**Measured: the working set is not proportional to the output.** ImageIO
decodes at native/2^k, the largest power-of-two reduction that still covers
the output (up to 1/8 for JPEG, PNG and HEIC), scales that into the
thumbnail, and the thumbnail is copied into our bitmap. So the reservation is

```
estimate = intermediate × a + output × b + fixed      (4 bytes per pixel)
```

with a, b and fixed per format (`MediaService::costFor`). They are fitted
with headroom to the probe's peaks and to the peaks inside the app, where
PNG decodes cost about 30 MB more (tables in `docs/BENCHMARKS.md` §6). A
single factor on the output could not fit both a 2880×1800 PNG at 1400 px
(k = 1, 24 MB peak) and at 1520 px (k = 0, 37 MB peak). ImageIO also keeps
1–28 MB after a decode (allocator and ImageIO caches, HEIC the most). No
reservation covers that; the plateau benchmark shows it.

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
  - **Measured:** JPEG, HEIC *and PNG* decode at a reduced size (a 6000×4000
    PNG at 1400 px peaks at 0.25× its full-size bitmap). WebP decodes in full
    (1.3× the full-size bitmap at any output). `kCGImageSourceSubsampleFactor`
    lowered no peak and is not used.
  - Thread CPU understates JPEG and HEIC cost: ImageIO decodes them partly on
    its own threads (HEIC: ~160 ms wall, ~20 ms on our worker), so
    `media_cpu_us_discarded` counts only the worker's share.
- **Output size is limited in total pixels as well as width.** The target is the
  physical width (`column width × DPR`, capped at native width). Height follows
  from the aspect ratio. Total pixels are clamped (start at 16 MP). Tall images
  past that limit are decoded at a lower density and drawn scaled up.
  - **Measured:** with a 128 MiB budget the 16 MP clamp never binds. The
    binding clamp is "one decode reserves at most 75 % of the current
    budget". A PNG decoded at native/2 still needs a native/2 intermediate,
    so the 1000×40000 screenshot (shown 760 pt wide) decodes at 234×9360,
    about 0.15 of the display's density, and looks blurry. Decoding tall
    images in strips (the PDF tile machinery of step 4) is the follow-up
    that restores full density.
- **Fallback decoder: `QImageReader`**, with `setScaledSize` and
  `setAutoTransform(true)`. `setScaledSize` may decode in full and then scale,
  depending on the format handler. The budget assumes the worst case for any
  format not yet measured.
- **Cache key**: hash, physical width, physical height, orientation, colour
  handling. At most 2 resolutions are kept per attachment.
- **Eviction does not mean "decode again".** When `evictFarBlocks` clears a
  block, it drops only the block's key. The shared LRU keeps recent bitmaps
  while the budget allows, so scrolling down and back up hits the cache. Memory
  pressure evicts aggressively. A cache hit is counted when a block starts
  wanting a key that is already decoded.
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
- **Implemented in the editor:** an image is one U+FFFC line, like a page
  link. Its height comes from the stored dimensions, so the estimate before
  layout equals the laid-out height. Paste (encoded clipboard bytes are
  stored as is; raw pixels become PNG), drag-and-drop of image files and
  Edit ▸ Insert Image… all go through the same import. Copying an image line
  copies its Markdown, and pasting that Markdown back gives an image line on
  a line of its own.
- **A disk-cached display preview** (a downscaled copy kept beside the
  original) is deferred. It is worth adding only if benchmarks show repeated
  expensive decodes (huge PNGs, HEIC). It is never generated upfront for every
  import.

## 4. PDF

- **In the note**: a first-page thumbnail (one small bitmap through the same
  scheduler and budget), plus the filename and page count.
  - **Implemented:** a fixed-height card (132 pt) with the thumbnail's width
    from the stored first-page geometry, so the estimated height equals the
    laid-out height and nothing is rendered to lay it out. The thumbnail is
    a `PdfThumb` request on the PDF lane; the document is opened for that
    one job and closed after it. Clicking the card (or Mod+Enter on its
    line) opens the viewer. Paste and drag-and-drop of `.pdf` files and
    Edit ▸ Insert PDF… go through the same import as images.
- **The viewer** takes over the editor slot, like Settings. It is one custom
  viewport, not a widget per page:
  - Page offsets come from `page_geom`, with a prefix sum and a binary search
    for the visible pages.
  - Invalidation is kept separate from the note editor, so typing or the caret
    blink never repaints the PDF.
  - **Implemented** (`PdfView` in a `PdfViewerPage`, shown in place of the
    layout like Settings; Esc, Mod+W or the back button return to the note).
    The viewer reacts only to results for its own document's pages and
    tiles, and repaints only the rect they cover; the note's layout reacts
    only to image and thumbnail results. It opens at fit width and keeps
    fitting while the window resizes until the user zooms (pinch,
    Mod+wheel, Mod+=/−/0, the header buttons).
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
  - **Measured: CoreGraphics' working set follows the page's content, not
    the output, and transparency layers follow the clip.** On the fixtures
    (`docs/BENCHMARKS.md` §6, "PDF render cost"): a plain page peaks at
    ~1.1–2.7× its bitmap; a page with transparency groups or soft masks at
    ~8–13× (one device-resolution layer per group, clipped to the bitmap);
    a 300 dpi scan at the decoded size of its embedded JPEG (2550×3300:
    ~73 MiB at full density, whatever the tile size), reduced by a power of
    two when the page is drawn small, like ImageIO. A whole 2000 px page
    with transparency peaked at 157 MiB; one 512 px tile of it at 11 MiB.
    A single output factor could not fit both, so the reservation uses the
    page's cost hints from `page_geom` (§1):
    `output × (2.5, or 13 with layers) + image(native/2^k) × 2.4 + 6 MiB
    (with an image) + 2 MiB` (`MediaService::pdfEstimate`).
  - **Measured: what CoreGraphics frees stays in the footprint.** After a
    render, the allocator keeps CG's freed large blocks (live
    `MALLOC_LARGE` regions; `malloc_zone_pressure_relief` returns none) and
    reuses them for the next render, so the footprint plateaus at the
    largest single render's working set, and closing the document does not
    lower it. One whole 2000 px transparency page left 148 MB; 512 px
    tiles leave ≤ 10 MB on every fixture.
  - **Chosen from those measurements:** tiles are **512×512**, and a page is
    drawn as **one bitmap only up to 2 MiB** (about a Letter page 360 pt
    wide at 2×: thumbnails-sized and zoomed-out views). 512 px tiles cut the
    worst per-render peak 4× against 1024 px tiles (11 vs 45 MiB with
    transparency) and the memory left behind 3×; they cost the same time on
    text and transparency, and up to 1.4× on scans and 1.7× on a page of
    3000 stroked curves (each tile walks the content stream again). A whole page beats tiles on CPU
    only while small: at 1.8 MiB it peaks at ~1.5× a tile's working set,
    at 7 MiB already ~5×. So at fit width on a 2× display, pages are tiled.
  - One render never reserves more than 75 % of the current budget, as for
    images: when even a tile does not fit (a pressure cool-down, a huge
    scan), the page is drawn whole at a lower density and scaled up rather
    than not at all.
  - **Measured: a fling wastes uninterruptible renders.** Scrolling through
    the scan discarded 57 of 89 renders (1.3 s of CPU), through the vector
    document 30 (8.7 s). Heavy pages (layers, an image of 1 MP or more, or
    content streams over 64 KB, all from the `page_geom` hints) are now
    requested only once the view moves slower than a quarter viewport per
    100 ms for 150 ms; text pages render at once (the 200-page document
    keeps up with 16.7 ms frames). Discarded: 1 per document.
- **Release**: the document handle closes when the viewer closes, or after it
  has been hidden for a while. Text extraction (search, selection) is a later
  milestone, done lazily per page.
  - **Implemented:** the handle is shared by the viewer's render jobs and
    opened lazily by the first one, on the worker (never on the main
    thread). Closing the viewer drops its queued requests, frees its pages
    and tiles from the cache, and releases the handle (a job already
    running keeps its own reference until it returns). Hidden for 30 s
    (window minimized, app hidden), the handle closes and is reopened by
    the next render when shown again. While open, the viewer pins the
    attachment, so a sweep cannot remove the file under it.

## 5. Workers and cancellation

- One scheduler and one budget, with **two lanes**: at most one image decode
  and at most one PDF render at a time. They run concurrently only when both
  reservations fit, so one slow PDF page never holds up every image.
- PDF calls are serialized per document: CGPDFDocument is not documented as
  safe for concurrent drawing, and PDFium requires single-threaded access.
  Adding PDF threads would not add safe parallelism.
  - **Implemented:** every CoreGraphics PDF call (thumbnails, pages, tiles)
    runs on the PDF lane, which runs one job at a time, so calls on a
    document are serialized by construction. Imports read page geometry on
    the import worker, on a document object of their own.
- **Cancellation is layered**, because neither ImageIO, `QImageReader` nor
  `CGContextDrawPDFPage` can be interrupted mid-call (for PDFs, not
  starting heavy pages during a fling, §4, comes before all of these):
  1. Stale requests are dropped from the queue before they start. This is the
     main mechanism.
  2. Between steps of a job (tile N → tile N+1), the job checks whether it is
     still wanted. **Implemented** by making each tile its own job: the
     queue is re-examined between tiles, so a tile that scrolled away is
     dropped before it starts.
  3. Obsolete results are thrown away: no repaint, no caching beyond the LRU
     rules. **Implemented:** a page or tile that finishes after its viewer
     closed is not cached at all.
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
  - **Measured, and kept at 30 s:** idle on the image page (0.15 % of a core,
    6.0 wakeups/s) matched the text page (0.27 %, 7.0/s). The thread count
    went from 7 to 11 (the import worker, the image lane and ImageIO's own
    threads), all parked.

## 6. Instrumentation and benchmarks

New `Instrument` counters and gauges:

- `media_budget_bytes`, `media_reserved_bytes`, `media_cache_bytes`
- `image_decodes`, `image_cache_hits`, `pdf_renders`, `pdf_tiles`,
  `pdf_cache_hits`
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

The `images` bench scenario and `folia_mediaprobe` cover the image rows
below; the `pdf` scenario, `folia_mediaprobe --pdf` and
`bench/scripts/pdfprobe_matrix.py` cover the PDF rows. The measured
results are in `docs/BENCHMARKS.md` §6.

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
  - **Measured:** the default column is 760 pt, so a photo is 1520×1140 ≈
    6.9 MB. The 128 MiB budget kept 14 of them cached (101.7 MB) beside a
    decode in flight. Scrolling a 30-photo page down and back up found 13 in
    the cache and decoded 16 again.
- "CoreGraphics uses the least memory" and "a long PDF takes 20–60 MB" are
  targets to demonstrate on comparable documents. Page count alone does not
  predict rendering cost.
  - **Measured** (200-page text document at fit width on a 2× display,
    27 MB of tiles per page): the viewer fills the budget, so the footprint
    plateaus at the baseline plus ~131 MB of cached tiles plus ~60 MB of
    CoreGraphics and allocator working set (58 → ~249 MB), the same on a
    second pass, and closing returns it to 90 MB with the cache empty. What
    a long PDF costs is set by the budget, not by its length. Comparing
    with QtPdf waits for step 5.

## 7. Order of work

1. Attachment store, streaming import, reference-based GC.
2. `MediaBudget` plus the two-lane scheduler, with counters, before any decoder
   exists.
3. Image block with the ImageIO decoder. Run the image benchmarks and calibrate
   the overhead factors.
4. PDF thumbnail, then the viewer with the CoreGraphics backend. Run the PDF
   benchmarks and choose the tile size and the whole-page threshold.
   **Done:** 512×512 tiles, whole pages up to 2 MiB (§4).
5. QtPdf and `QImageReader` backends for other platforms.
