// Platform probes and hooks (macOS implementation in platform/mac; portable
// fallbacks elsewhere). Counter names are reported verbatim in benchmarks.
#pragma once

#include <QImage>
#include <QString>
#include <functional>

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

} // namespace folia::platform
