// Platform probes and hooks (macOS implementation in platform/mac; portable
// fallbacks elsewhere). Counter names are reported verbatim in benchmarks.
#pragma once

#include "core/Attachments.h"

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

namespace folia::platform {

struct ProcessSample {
    qint64 footprintBytes = 0;      // macOS: task_vm_info.phys_footprint; Linux: RSS; Windows: PrivateUsage
    qint64 peakFootprintBytes = 0;  // macOS: ledger_phys_footprint_peak
    qint64 residentBytes = 0;       // resident_size (shared pages included)
    double userSeconds = 0;
    double systemSeconds = 0;
    qint64 interruptWakeups = 0;    // macOS: task_power_info.task_interrupt_wakeups
    qint64 idleWakeups = 0;         // macOS: task_power_info.task_platform_idle_wakeups
    QString counterName;            // which OS counter footprintBytes is
    int threads = 0;                // threads in this process
};

ProcessSample sampleSelf();
QString osDescription();
QString thermalState();             // "nominal", "fair", "serious", "critical", or "unknown"
QString powerSource();
bool reducedMotion();
bool reducedTransparency();
// Benchmarks: keep the process out of App Nap (macOS throttles the threads
// of a background app whose windows are covered). Off by default.
void setAppNapAllowed(bool allowed);
// Called on the main thread when the OS reports memory pressure.
void onMemoryPressure(std::function<void(bool critical)> cb);

// ---- images (docs/ATTACHMENTS.md §3). All thread-safe; called on workers.
struct ImageInfo {
    bool ok = false;
    QString mime;        // "image/jpeg", ...
    int width = 0;       // after the orientation transform
    int height = 0;
    int orientation = 1; // EXIF 1..8
    int frames = 1;
    QString decoder;     // "imageio" or "qimagereader"
    QString error;
};
// Reads the header only (no pixel data).
ImageInfo imageInfo(const QString &path);
// Decodes the first frame so that neither side exceeds maxPixelSize, with
// the orientation applied, converted to sRGB, as RGB32 (opaque) or
// ARGB32_Premultiplied (what QPainter blits fastest), into allocateImage()
// memory. macOS: ImageIO's
// CGImageSourceCreateThumbnailAtIndex (max pixel size, transform, cache
// immediately), falling back to QImageReader; elsewhere QImageReader.
QImage decodeImage(const QString &path, int maxPixelSize, QString *error = nullptr);
// The portable QImageReader path (setScaledSize + setAutoTransform). Note
// that it may decode in full before scaling, depending on the format.
ImageInfo imageInfoQt(const QString &path);
QImage decodeImageQt(const QString &path, int maxPixelSize, QString *error = nullptr);
// Fixtures for tests and benchmarks: `format` is a UTI on macOS
// ("public.jpeg", "public.png", "public.heic") or a Qt format name. False
// when the encoder is unavailable (ImageIO cannot write WebP).
bool encodeImage(const QImage &image, const QString &path, const QString &format, double quality = 0.85,
                 int orientation = 1); // EXIF orientation tag to write (ImageIO only)
// CPU time consumed by the calling thread (µs).
qint64 threadCpuMicros();
// Peak footprint since the previous call (macOS: rusage
// ri_interval_max_phys_footprint, which the kernel resets on every read, so
// call it once before and once after the work to measure). Elsewhere: the
// current footprint.
qint64 intervalPeakFootprint();
// A bitmap in its own anonymous memory mapping, unmapped when the last
// QImage sharing it goes. Measured on macOS: freed multi-MB malloc blocks
// stay in the footprint (and malloc_zone_pressure_relief releases none of
// them), so cache eviction would free nothing. Null on allocation failure.
QImage allocateImage(int width, int height, QImage::Format format);
// Asks the allocator to return free pages to the system (macOS:
// malloc_zone_pressure_relief on every zone). Returns bytes released.
qint64 releaseFreedMemory();

// ---- PDF (docs/ATTACHMENTS.md §4). macOS: CoreGraphics (CGPDFDocument);
// elsewhere unavailable until the QtPdf backend (step 5).
struct PdfInfo {
    bool ok = false;
    int pages = 0;
    QVector<PdfPageGeom> geom; // one per page
    QString error;
};
// True when the file starts like a PDF ("%PDF-" within the first 1 KiB).
bool looksLikePdf(const QString &path);
// Page count and per-page crop-box geometry. Parses the cross-reference
// table and each page dictionary; no page content is drawn.
PdfInfo pdfInfo(const QString &path);

// An open document, shared by every render job of one viewer. Opened on
// first use, on the worker that renders (never on the main thread), and
// closed when the last reference goes. Not safe for concurrent drawing:
// callers serialize renders per document (the PDF lane runs one at a time).
class PdfDocument {
public:
    explicit PdfDocument(QString path);
    ~PdfDocument();
    PdfDocument(const PdfDocument &) = delete;
    PdfDocument &operator=(const PdfDocument &) = delete;
    const QString &path() const { return m_path; }
    bool isOpen() const { return m_handle != nullptr; }
    void *handle(QString *error); // opens on first call (worker)
    // Live documents in this process (tests: handles must not leak).
    static int liveCount();

private:
    QString m_path;
    void *m_handle = nullptr;
    bool m_failed = false;
};
using PdfDocumentPtr = std::shared_ptr<PdfDocument>;

// Renders the part `region` (physical pixels, in the coordinate space of the
// whole page drawn `pageWidthPx` wide in display orientation) of page
// `page` (0-based) straight into a region-sized bitmap: the transform and
// clip select the region, so a tile never allocates the whole page. White
// background, sRGB, RGB32, in allocateImage() memory. Null on failure.
QImage renderPdfPage(PdfDocument *doc, int page, int pageWidthPx, const QRect &region, QString *error = nullptr);

// Fixtures for tests and benchmarks (macOS: written with CGPDFContext):
// "text" (paragraphs of body text), "vector" (thousands of stroked and
// filled curves per page), "transparency" (nested transparency layers,
// blend modes, soft shadows), "scanned" (one 300 dpi photo-like JPEG per
// page), "mixed" (sizes vary: Letter, A4, landscape, a small receipt).
bool writeFixturePdf(const QString &path, const QString &style, int pages);

} // namespace folia::platform
