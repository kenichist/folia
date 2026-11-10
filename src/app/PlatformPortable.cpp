// Portable fallbacks for non-macOS targets (Linux / Windows), plus the
// QImageReader image decoder, which macOS uses as its fallback.
#include "app/Platform.h"

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeDatabase>
#include <cstring>
#include <ctime>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace folia::platform {

namespace {
// QImageIOHandler::Transformation bit for a 90° rotation (axes swap).
bool swapsAxes(QImageIOHandler::Transformations t) { return t & QImageIOHandler::TransformationRotate90; }

int exifOrientation(QImageIOHandler::Transformations t)
{
    switch (int(t)) {
    case QImageIOHandler::TransformationNone: return 1;
    case QImageIOHandler::TransformationMirror: return 2;
    case QImageIOHandler::TransformationRotate180: return 3;
    case QImageIOHandler::TransformationFlip: return 4;
    case QImageIOHandler::TransformationFlipAndRotate90: return 5;
    case QImageIOHandler::TransformationRotate90: return 6;
    case QImageIOHandler::TransformationMirrorAndRotate90: return 7;
    case QImageIOHandler::TransformationRotate270: return 8;
    }
    return 1;
}
} // namespace

namespace {
struct Mapping {
    void *p;
    size_t n;
};
void unmapImage(void *info)
{
    auto *m = static_cast<Mapping *>(info);
#ifdef Q_OS_WIN
    VirtualFree(m->p, 0, MEM_RELEASE);
#else
    munmap(m->p, m->n);
#endif
    delete m;
}
} // namespace

QImage allocateImage(int width, int height, QImage::Format format)
{
    if (width <= 0 || height <= 0)
        return QImage();
    const int depth = QImage::toPixelFormat(format).bitsPerPixel();
    const qsizetype bpl = ((qsizetype(width) * depth + 31) / 32) * 4;
    const size_t n = size_t(bpl) * size_t(height);
#ifdef Q_OS_WIN
    void *p = VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p)
        return QImage();
#else
    void *p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED)
        return QImage();
#endif
    return QImage(static_cast<uchar *>(p), width, height, bpl, format, unmapImage, new Mapping{p, n});
}

ImageInfo imageInfoQt(const QString &path)
{
    ImageInfo i;
    i.decoder = QStringLiteral("qimagereader");
    QImageReader r(path);
    r.setAutoTransform(true);
    if (!r.canRead()) {
        i.error = r.errorString();
        return i;
    }
    const QSize raw = r.size();
    if (!raw.isValid()) {
        i.error = QStringLiteral("unknown image size");
        return i;
    }
    const QImageIOHandler::Transformations t = r.transformation();
    i.orientation = exifOrientation(t);
    i.width = swapsAxes(t) ? raw.height() : raw.width();
    i.height = swapsAxes(t) ? raw.width() : raw.height();
    i.frames = qMax(1, r.imageCount());
    i.mime = QMimeDatabase().mimeTypeForFile(path).name();
    i.ok = true;
    return i;
}

QImage decodeImageQt(const QString &path, int maxPixelSize, QString *error)
{
    // QImageReader::setAllocationLimit (default 256 MiB) stays on as a second
    // guard; it is static, i.e. process-wide, so it is left at Qt's default.
    QImageReader r(path);
    r.setAutoTransform(true);
    const QSize raw = r.size();
    if (raw.isValid() && maxPixelSize > 0 && qMax(raw.width(), raw.height()) > maxPixelSize) {
        // The longest side is the same before and after a 90° rotation.
        const double s = double(maxPixelSize) / qMax(raw.width(), raw.height());
        r.setScaledSize(QSize(qMax(1, qRound(raw.width() * s)), qMax(1, qRound(raw.height() * s))));
    }
    QImage img = r.read(); // first frame only
    if (img.isNull()) {
        if (error)
            *error = r.errorString();
        return img;
    }
    if (img.colorSpace().isValid() && img.colorSpace() != QColorSpace::SRgb)
        img.convertToColorSpace(QColorSpace::SRgb);
    const QImage::Format f = img.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
    if (img.format() != f)
        img.convertTo(f);
    // Into its own mapping, so evicting it from the cache frees it.
    QImage out = allocateImage(img.width(), img.height(), f);
    if (out.isNull())
        return img;
    for (int y = 0; y < img.height(); ++y)
        memcpy(out.scanLine(y), img.constScanLine(y), size_t(img.bytesPerLine()));
    out.setColorSpace(img.colorSpace());
    return out;
}

bool looksLikePdf(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.read(1024).contains("%PDF-");
}

qint64 threadCpuMicros()
{
#if defined(Q_OS_WIN)
    FILETIME c, e, k, u;
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return qint64(((quint64(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (quint64(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 10);
    return 0;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return qint64(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    return 0;
#endif
}

} // namespace folia::platform

#ifndef Q_OS_MACOS
#include <QFile>
#include <QSysInfo>
#include <cstring>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace folia::platform {

ProcessSample sampleSelf()
{
    ProcessSample s;
#if defined(Q_OS_WIN)
    s.counterName = QStringLiteral("PROCESS_MEMORY_COUNTERS_EX.PrivateUsage");
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc)) {
        s.footprintBytes = qint64(pmc.PrivateUsage);
        s.peakFootprintBytes = qint64(pmc.PeakPagefileUsage);
        s.residentBytes = qint64(pmc.WorkingSetSize);
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto toS = [](FILETIME f) { return double((qint64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        s.userSeconds = toS(u);
        s.systemSeconds = toS(k);
    }
#else
    s.counterName = QStringLiteral("/proc/self/status VmRSS (RssAnon+RssFile+RssShmem)");
    QFile f(QStringLiteral("/proc/self/status"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            auto val = [&](const char *k) { return line.mid(int(strlen(k))).trimmed().split(' ').value(0).toLongLong() * 1024; };
            if (line.startsWith("VmRSS:")) { s.footprintBytes = val("VmRSS:"); s.residentBytes = s.footprintBytes; }
            else if (line.startsWith("VmHWM:")) s.peakFootprintBytes = val("VmHWM:");
            else if (line.startsWith("Threads:")) s.threads = int(line.mid(8).trimmed().toInt());
        }
    }
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        s.userSeconds = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
        s.systemSeconds = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        s.interruptWakeups = ru.ru_nvcsw + ru.ru_nivcsw; // context switches (proxy, labelled)
    }
#endif
    return s;
}

QString osDescription() { return QSysInfo::prettyProductName(); }
QString thermalState() { return QStringLiteral("unknown"); }
QString powerSource() { return QStringLiteral("unknown"); }
bool reducedMotion() { return false; }
bool reducedTransparency() { return false; }
void onMemoryPressure(std::function<void(bool)>) {}
void setAppNapAllowed(bool) {}
void showOverAllSpaces(quintptr, double) {}
qint64 intervalPeakFootprint() { return sampleSelf().footprintBytes; }
qint64 releaseFreedMemory() { return 0; }

ImageInfo imageInfo(const QString &path) { return imageInfoQt(path); }
QImage decodeImage(const QString &path, int maxPixelSize, QString *error) { return decodeImageQt(path, maxPixelSize, error); }

bool encodeImage(const QImage &image, const QString &path, const QString &format, double quality, int)
{
    QImageWriter w(path, format.toLatin1());
    w.setQuality(int(quality * 100));
    return w.write(image);
}

// PDFs need the QtPdf backend (docs/ATTACHMENTS.md §7, step 5).
PdfInfo pdfInfo(const QString &)
{
    PdfInfo i;
    i.error = QStringLiteral("PDF support is not available on this platform yet");
    return i;
}

PdfDocument::PdfDocument(QString path) : m_path(std::move(path)) {}
PdfDocument::~PdfDocument() = default;
int PdfDocument::liveCount() { return 0; }
void *PdfDocument::handle(QString *error)
{
    if (error)
        *error = QStringLiteral("PDF support is not available on this platform yet");
    return nullptr;
}

QImage renderPdfPage(PdfDocument *, int, int, const QRect &, QString *error)
{
    if (error)
        *error = QStringLiteral("PDF support is not available on this platform yet");
    return QImage();
}

bool writeFixturePdf(const QString &, const QString &, int) { return false; }

} // namespace folia::platform
#endif
