// macOS platform probes: Mach task counters, thermal state, accessibility
// preferences, dispatch memory-pressure notifications, the ImageIO image
// decoder (docs/ATTACHMENTS.md §3) and the CoreGraphics PDF renderer (§4).
#include "app/Platform.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <QColorSpace>
#include <QFileInfo>
#include <QMetaObject>
#include <QCoreApplication>
#include <QRandomGenerator>
#include <QSet>
#include <cstring>
#include <atomic>
#include <cmath>
#include <dispatch/dispatch.h>
#include <libproc.h>
#include <malloc/malloc.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

namespace folia::platform {

ProcessSample sampleSelf()
{
    ProcessSample s;
    s.counterName = QStringLiteral("task_vm_info.phys_footprint");
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        s.footprintBytes = qint64(vm.phys_footprint);
        s.residentBytes = qint64(vm.resident_size);
        if (count >= TASK_VM_INFO_REV4_COUNT)
            s.peakFootprintBytes = qint64(vm.ledger_phys_footprint_peak);
    }
    task_power_info_data_t pw{};
    count = TASK_POWER_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO, reinterpret_cast<task_info_t>(&pw), &count) == KERN_SUCCESS) {
        // total_user/total_system are Mach absolute-time units, not ns.
        static mach_timebase_info_data_t tb = [] {
            mach_timebase_info_data_t t{};
            mach_timebase_info(&t);
            return t;
        }();
        const double toNs = double(tb.numer) / double(tb.denom);
        s.userSeconds = double(pw.total_user) * toNs / 1e9;
        s.systemSeconds = double(pw.total_system) * toNs / 1e9;
        s.interruptWakeups = qint64(pw.task_interrupt_wakeups);
        s.idleWakeups = qint64(pw.task_platform_idle_wakeups);
    }
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        s.threads = int(n);
        for (mach_msg_type_number_t i = 0; i < n; ++i)
            mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), vm_address_t(list), n * sizeof(thread_act_t));
    }
    return s;
}

QString osDescription()
{
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    return QStringLiteral("macOS %1.%2.%3").arg(v.majorVersion).arg(v.minorVersion).arg(v.patchVersion);
}

QString thermalState()
{
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return QStringLiteral("nominal");
    case NSProcessInfoThermalStateFair: return QStringLiteral("fair");
    case NSProcessInfoThermalStateSerious: return QStringLiteral("serious");
    case NSProcessInfoThermalStateCritical: return QStringLiteral("critical");
    }
    return QStringLiteral("unknown");
}

QString powerSource()
{
    return NSProcessInfo.processInfo.lowPowerModeEnabled ? QStringLiteral("low-power-mode")
                                                         : QStringLiteral("normal-power-mode");
}

bool reducedMotion()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}

bool reducedTransparency()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceTransparency;
}

void setAppNapAllowed(bool allowed)
{
    static id<NSObject> activity = nil;
    if (!allowed && !activity) {
        activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                                                reason:@"Folia benchmark"];
    } else if (allowed && activity) {
        [NSProcessInfo.processInfo endActivity:activity];
        activity = nil;
    }
}

void onMemoryPressure(std::function<void(bool)> cb)
{
    static dispatch_source_t source = nullptr;
    if (source)
        return;
    source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                    DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                    dispatch_get_main_queue());
    auto *heap = new std::function<void(bool)>(std::move(cb));
    dispatch_source_set_event_handler(source, ^{
        const unsigned long level = dispatch_source_get_data(source);
        (*heap)((level & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0);
    });
    dispatch_resume(source);
}

qint64 releaseFreedMemory()
{
    return qint64(malloc_zone_pressure_relief(nullptr, 0));
}

qint64 intervalPeakFootprint()
{
    rusage_info_v4 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&ri)) == 0)
        return qint64(ri.ri_interval_max_phys_footprint);
    return sampleSelf().footprintBytes;
}

// ------------------------------------------------------------- images ----
namespace {

CGImageSourceRef openSource(const QString &path)
{
    NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
    NSDictionary *opts = @{(id)kCGImageSourceShouldCache: @NO};
    return CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)opts);
}

QString mimeForUti(CFStringRef uti)
{
    if (!uti)
        return QString();
    UTType *t = [UTType typeWithIdentifier:(__bridge NSString *)uti];
    if (t.preferredMIMEType)
        return QString::fromNSString(t.preferredMIMEType);
    return QStringLiteral("application/octet-stream");
}

int intProp(NSDictionary *d, CFStringRef key, int def)
{
    id v = d[(__bridge NSString *)key];
    return [v isKindOfClass:[NSNumber class]] ? [v intValue] : def;
}

} // namespace

ImageInfo imageInfo(const QString &path)
{
    ImageInfo i;
    i.decoder = QStringLiteral("imageio");
    @autoreleasepool {
        CGImageSourceRef src = openSource(path);
        if (!src) {
            i.error = QStringLiteral("cannot open");
            return i;
        }
        CFStringRef type = CGImageSourceGetType(src);
        const size_t count = CGImageSourceGetCount(src);
        if (!type || count == 0) {
            CFRelease(src);
            i.error = QStringLiteral("not an image");
            return i;
        }
        const size_t idx = CGImageSourceGetPrimaryImageIndex(src);
        NSDictionary *opts = @{(id)kCGImageSourceShouldCache: @NO};
        CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, idx, (__bridge CFDictionaryRef)opts);
        if (props) {
            NSDictionary *d = (__bridge NSDictionary *)props;
            const int w = intProp(d, kCGImagePropertyPixelWidth, 0);
            const int h = intProp(d, kCGImagePropertyPixelHeight, 0);
            i.orientation = qBound(1, intProp(d, kCGImagePropertyOrientation, 1), 8);
            const bool swap = i.orientation >= 5; // 5..8 rotate by 90°
            i.width = swap ? h : w;
            i.height = swap ? w : h;
            CFRelease(props);
        }
        i.mime = mimeForUti(type);
        i.frames = int(count);
        CFRelease(src);
        i.ok = i.width > 0 && i.height > 0;
        if (!i.ok)
            i.error = QStringLiteral("unknown image size");
    }
    if (!i.ok) {
        const ImageInfo q = imageInfoQt(path);
        if (q.ok)
            return q;
    }
    return i;
}

QImage decodeImage(const QString &path, int maxPixelSize, QString *error)
{
    QImage out;
    @autoreleasepool {
        CGImageSourceRef src = openSource(path);
        if (!src) {
            if (error)
                *error = QStringLiteral("cannot open");
            return out;
        }
        const size_t idx = CGImageSourceGetPrimaryImageIndex(src); // first frame of GIF/APNG
        // (kCGImageSourceSubsampleFactor was measured and does not lower the
        // peak for any format here: docs/BENCHMARKS.md, "Image decode cost".)
        NSDictionary *opts = @{
            (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
            (id)kCGImageSourceThumbnailMaxPixelSize: @(qMax(1, maxPixelSize)),
            (id)kCGImageSourceCreateThumbnailWithTransform: @YES,
            (id)kCGImageSourceShouldCacheImmediately: @YES,
        };
        CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, idx, (__bridge CFDictionaryRef)opts);
        CFRelease(src);
        if (img) {
            const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
            const CGImageAlphaInfo a = CGImageGetAlphaInfo(img);
            const bool opaque = a == kCGImageAlphaNone || a == kCGImageAlphaNoneSkipFirst || a == kCGImageAlphaNoneSkipLast;
            out = allocateImage(int(w), int(h), opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);
            if (!out.isNull()) {
                // Draw into the QImage's own buffer (one copy, colour-converted
                // to sRGB); the thumbnail's bitmap is released right after.
                CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                const uint32_t info = kCGBitmapByteOrder32Little
                    | (opaque ? kCGImageAlphaNoneSkipFirst : kCGImageAlphaPremultipliedFirst);
                CGContextRef ctx = CGBitmapContextCreate(out.bits(), w, h, 8, size_t(out.bytesPerLine()), cs, info);
                if (ctx) {
                    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
                    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
                    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
                    CGContextRelease(ctx);
                    out.setColorSpace(QColorSpace::SRgb);
                } else {
                    out = QImage();
                }
                CGColorSpaceRelease(cs);
            }
            CGImageRelease(img);
        }
    }
    if (out.isNull()) {
        // Formats ImageIO cannot read (or a failed allocation): portable path.
        return decodeImageQt(path, maxPixelSize, error);
    }
    return out;
}

bool encodeImage(const QImage &image, const QString &path, const QString &format, double quality, int orientation)
{
    bool ok = false;
    @autoreleasepool {
        const QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(const_cast<uchar *>(src.constBits()), size_t(src.width()),
                                                 size_t(src.height()), 8, size_t(src.bytesPerLine()), cs,
                                                 kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
        NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
        CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                                          (__bridge CFStringRef)format.toNSString(), 1, nullptr)
                                        : nullptr;
        if (dst) {
            NSDictionary *props = @{(id)kCGImageDestinationLossyCompressionQuality: @(quality),
                                    (id)kCGImagePropertyOrientation: @(qBound(1, orientation, 8))};
            CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)props);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        if (img)
            CGImageRelease(img);
        if (ctx)
            CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
    }
    return ok;
}

// ---------------------------------------------------------------- PDF ----
namespace {

std::atomic<int> g_liveDocs{0};

CGPDFDocumentRef openPdf(const QString &path, QString *error)
{
    NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL((__bridge CFURLRef)url);
    if (!doc) {
        if (error)
            *error = QStringLiteral("cannot open the PDF");
        return nullptr;
    }
    if (CGPDFDocumentIsEncrypted(doc) && !CGPDFDocumentIsUnlocked(doc)
        && !CGPDFDocumentUnlockWithPassword(doc, "")) {
        CGPDFDocumentRelease(doc);
        if (error)
            *error = QStringLiteral("the PDF is password-protected");
        return nullptr;
    }
    if (CGPDFDocumentGetNumberOfPages(doc) == 0) {
        CGPDFDocumentRelease(doc);
        if (error)
            *error = QStringLiteral("the PDF has no pages");
        return nullptr;
    }
    return doc;
}

int normalizedRotation(CGPDFPageRef page)
{
    const int r = ((CGPDFPageGetRotationAngle(page) % 360) + 360) % 360;
    return (r / 90) * 90;
}

// Render-cost hints from a page's resources (dictionary lookups only):
// the largest image XObject, and whether anything needs a transparency
// layer (a group, or a soft mask). Form XObjects are followed a few levels
// deep; each one is visited once.
struct CostScan {
    PdfPageGeom *g;
    QSet<const void *> seen;
    int depth = 0;
};

bool nameIs(CGPDFDictionaryRef d, const char *key, const char *value)
{
    const char *n = nullptr;
    return CGPDFDictionaryGetName(d, key, &n) && n && strcmp(n, value) == 0;
}

bool reducibleFilter(CGPDFDictionaryRef d)
{
    const char *n = nullptr;
    if (CGPDFDictionaryGetName(d, "Filter", &n))
        return n && (strcmp(n, "DCTDecode") == 0 || strcmp(n, "JPXDecode") == 0);
    CGPDFArrayRef a = nullptr;
    if (CGPDFDictionaryGetArray(d, "Filter", &a) && CGPDFArrayGetCount(a) > 0
        && CGPDFArrayGetName(a, CGPDFArrayGetCount(a) - 1, &n))
        return n && (strcmp(n, "DCTDecode") == 0 || strcmp(n, "JPXDecode") == 0);
    return false;
}

void scanResources(CGPDFDictionaryRef res, CostScan *scan);

void scanXObject(const char *, CGPDFObjectRef value, void *info)
{
    auto *scan = static_cast<CostScan *>(info);
    CGPDFStreamRef st = nullptr;
    if (!CGPDFObjectGetValue(value, kCGPDFObjectTypeStream, &st) || scan->seen.size() > 4000)
        return;
    if (scan->seen.contains(st))
        return;
    scan->seen.insert(st);
    CGPDFDictionaryRef d = CGPDFStreamGetDictionary(st);
    if (!d)
        return;
    if (nameIs(d, "Subtype", "Image")) {
        CGPDFInteger w = 0, h = 0;
        CGPDFDictionaryGetInteger(d, "Width", &w);
        CGPDFDictionaryGetInteger(d, "Height", &h);
        const quint64 px = quint64(qMax<CGPDFInteger>(0, w)) * quint64(qMax<CGPDFInteger>(0, h));
        if (px > scan->g->imagePixels) {
            scan->g->imagePixels = quint32(qMin<quint64>(px, 0xffffffffu));
            if (reducibleFilter(d))
                scan->g->flags |= PdfPageGeom::ReducibleImage;
            else
                scan->g->flags &= quint16(~PdfPageGeom::ReducibleImage);
        }
        CGPDFStreamRef mask = nullptr;
        if (CGPDFDictionaryGetStream(d, "SMask", &mask))
            scan->g->flags |= PdfPageGeom::Layers;
    } else if (nameIs(d, "Subtype", "Form")) {
        CGPDFDictionaryRef group = nullptr;
        if (CGPDFDictionaryGetDictionary(d, "Group", &group))
            scan->g->flags |= PdfPageGeom::Layers;
        CGPDFDictionaryRef res = nullptr;
        if (scan->depth < 4 && CGPDFDictionaryGetDictionary(d, "Resources", &res)) {
            ++scan->depth;
            scanResources(res, scan);
            --scan->depth;
        }
    }
}

void scanExtGState(const char *, CGPDFObjectRef value, void *info)
{
    auto *scan = static_cast<CostScan *>(info);
    CGPDFDictionaryRef gs = nullptr;
    CGPDFDictionaryRef mask = nullptr;
    if (CGPDFObjectGetValue(value, kCGPDFObjectTypeDictionary, &gs) && CGPDFDictionaryGetDictionary(gs, "SMask", &mask))
        scan->g->flags |= PdfPageGeom::Layers; // a soft mask (not /None)
}

void scanResources(CGPDFDictionaryRef res, CostScan *scan)
{
    if (!res)
        return;
    CGPDFDictionaryRef xobjects = nullptr;
    if (CGPDFDictionaryGetDictionary(res, "XObject", &xobjects))
        CGPDFDictionaryApplyFunction(xobjects, scanXObject, scan);
    CGPDFDictionaryRef states = nullptr;
    if (CGPDFDictionaryGetDictionary(res, "ExtGState", &states))
        CGPDFDictionaryApplyFunction(states, scanExtGState, scan);
}

void scanPage(CGPDFPageRef page, PdfPageGeom *g)
{
    CGPDFDictionaryRef pd = CGPDFPageGetDictionary(page);
    if (!pd)
        return;
    // (A page-level /Group alone composites onto white: no extra layer.)
    CostScan scan;
    scan.g = g;
    CGPDFDictionaryRef res = nullptr;
    if (CGPDFDictionaryGetDictionary(pd, "Resources", &res))
        scanResources(res, &scan);
    // Content length, from the stream dictionaries' /Length (not read).
    CGPDFInteger bytes = 0;
    CGPDFStreamRef st = nullptr;
    CGPDFArrayRef parts = nullptr;
    CGPDFDictionaryRef sd = nullptr;
    CGPDFInteger n = 0;
    if (CGPDFDictionaryGetStream(pd, "Contents", &st)) {
        if ((sd = CGPDFStreamGetDictionary(st)) && CGPDFDictionaryGetInteger(sd, "Length", &n))
            bytes += n;
    } else if (CGPDFDictionaryGetArray(pd, "Contents", &parts)) {
        for (size_t i = 0; i < CGPDFArrayGetCount(parts); ++i)
            if (CGPDFArrayGetStream(parts, i, &st) && (sd = CGPDFStreamGetDictionary(st))
                && CGPDFDictionaryGetInteger(sd, "Length", &n))
                bytes += n;
    }
    if (bytes > 64 * 1024)
        g->flags |= PdfPageGeom::HeavyContent;
}

} // namespace

PdfInfo pdfInfo(const QString &path)
{
    PdfInfo i;
    @autoreleasepool {
        CGPDFDocumentRef doc = openPdf(path, &i.error);
        if (!doc)
            return i;
        const size_t n = CGPDFDocumentGetNumberOfPages(doc);
        i.geom.reserve(int(n));
        for (size_t p = 1; p <= n; ++p) {
            CGPDFPageRef page = CGPDFDocumentGetPage(doc, p);
            PdfPageGeom g;
            if (page) {
                const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
                g.rotation = normalizedRotation(page);
                const bool swap = g.rotation % 180 != 0;
                g.w = float(std::fabs(swap ? box.size.height : box.size.width));
                g.h = float(std::fabs(swap ? box.size.width : box.size.height));
                scanPage(page, &g);
            }
            if (!(g.w > 0) || !(g.h > 0)) { // a broken page: Letter, so layout stays sane
                g.w = 612;
                g.h = 792;
            }
            i.geom.push_back(g);
        }
        i.pages = int(n);
        i.ok = n > 0;
        CGPDFDocumentRelease(doc);
    }
    return i;
}

PdfDocument::PdfDocument(QString path) : m_path(std::move(path)) {}

PdfDocument::~PdfDocument()
{
    if (m_handle) {
        CGPDFDocumentRelease(static_cast<CGPDFDocumentRef>(m_handle)); // frees CG's per-document caches
        g_liveDocs.fetch_sub(1);
    }
}

int PdfDocument::liveCount() { return g_liveDocs.load(); }

void *PdfDocument::handle(QString *error)
{
    if (!m_handle && !m_failed) {
        m_handle = openPdf(m_path, error);
        m_failed = m_handle == nullptr;
        if (m_handle)
            g_liveDocs.fetch_add(1);
    } else if (!m_handle && error) {
        *error = QStringLiteral("cannot open the PDF");
    }
    return m_handle;
}

QImage renderPdfPage(PdfDocument *pdoc, int pageIndex, int pageWidthPx, const QRect &region, QString *error)
{
    QImage out;
    if (!pdoc || pageWidthPx <= 0 || region.isEmpty())
        return out;
    @autoreleasepool {
        CGPDFDocumentRef doc = static_cast<CGPDFDocumentRef>(pdoc->handle(error));
        if (!doc)
            return out;
        CGPDFPageRef page = CGPDFDocumentGetPage(doc, size_t(pageIndex) + 1);
        if (!page) {
            if (error)
                *error = QStringLiteral("no such page");
            return out;
        }
        const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
        const int rot = normalizedRotation(page);
        const bool swap = rot % 180 != 0;
        const CGFloat dw = std::fabs(swap ? box.size.height : box.size.width);
        const CGFloat dh = std::fabs(swap ? box.size.width : box.size.height);
        if (!(dw > 0) || !(dh > 0)) {
            if (error)
                *error = QStringLiteral("empty page");
            return out;
        }
        const CGFloat s = CGFloat(pageWidthPx) / dw;
        const CGFloat pageHeightPx = dh * s;
        out = allocateImage(region.width(), region.height(), QImage::Format_RGB32);
        if (out.isNull()) {
            if (error)
                *error = QStringLiteral("out of memory");
            return out;
        }
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(out.bits(), size_t(out.width()), size_t(out.height()), 8,
                                                 size_t(out.bytesPerLine()), cs,
                                                 kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGColorSpaceRelease(cs);
        if (!ctx) {
            if (error)
                *error = QStringLiteral("cannot create a bitmap context");
            return QImage();
        }
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, region.width(), region.height()));
        // Region (top-left origin, page pixels) -> this bitmap (bottom-left
        // origin): only the region is rasterized; CG clips to the context.
        CGContextTranslateCTM(ctx, -CGFloat(region.x()), -(pageHeightPx - CGFloat(region.y()) - CGFloat(region.height())));
        CGContextScaleCTM(ctx, s, s);
        // Crop box -> (0, 0, dw, dh) in display orientation (the page's
        // /Rotate applied); scale 1, so the "never scales up" rule is moot.
        CGContextConcatCTM(ctx, CGPDFPageGetDrawingTransform(page, kCGPDFCropBox, CGRectMake(0, 0, dw, dh), 0, true));
        CGContextClipToRect(ctx, box);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationDefault);
        CGContextDrawPDFPage(ctx, page);
        CGContextRelease(ctx);
        out.setColorSpace(QColorSpace::SRgb);
    }
    return out;
}

// ------------------------------------------------------- PDF fixtures ----
namespace {

const char *kFixtureText =
    "Folia keeps notes as plain blocks and renders attachments next to them. This paragraph exists to give "
    "the renderer realistic body text: kerning, ligatures, mixed punctuation (commas; semicolons: colons) and "
    "numbers like 3.14159 or 2026-11-06. A long document repeats paragraphs like this one with small changes, "
    "so that every page is laid out and drawn on its own. ";

void drawText(CGContextRef ctx, const CGRect &frame, NSString *text, CGFloat size)
{
    CTFontRef font = CTFontCreateWithName(CFSTR("Times New Roman"), size, nullptr);
    NSDictionary *attrs = @{(id)kCTFontAttributeName: (__bridge id)font,
                            (id)kCTForegroundColorFromContextAttributeName: @YES};
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    CGPathRef path = CGPathCreateWithRect(frame, nullptr);
    CTFrameRef f = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
    CTFrameDraw(f, ctx);
    CFRelease(f);
    CGPathRelease(path);
    CFRelease(fs);
    CFRelease(font);
}

void drawTextPage(CGContextRef ctx, CGRect media, int page)
{
    CGContextSetRGBFillColor(ctx, 0.1, 0.1, 0.12, 1);
    NSMutableString *body = [NSMutableString string];
    [body appendFormat:@"Chapter %d\n\n", page / 12 + 1];
    for (int i = 0; i < 9; ++i)
        [body appendFormat:@"%d.%d  %s\n", page + 1, i + 1, kFixtureText];
    drawText(ctx, CGRectInset(media, 54, 54), body, 10.5);
    if (page % 5 == 2) { // a figure now and then
        CGContextSetRGBStrokeColor(ctx, 0.2, 0.3, 0.7, 1);
        CGContextSetLineWidth(ctx, 1.2);
        const CGFloat x0 = media.origin.x + 72, y0 = media.origin.y + 72;
        for (int k = 0; k < 40; ++k) {
            CGContextMoveToPoint(ctx, x0, y0 + k * 3);
            CGContextAddCurveToPoint(ctx, x0 + 120, y0 + 160 - k * 2, x0 + 300, y0 + k * 4, x0 + 460, y0 + 90);
        }
        CGContextStrokePath(ctx);
    }
}

void drawVectorPage(CGContextRef ctx, CGRect media, int page)
{
    QRandomGenerator rng(quint32(1000 + page));
    auto r = [&](double lo, double hi) { return lo + (hi - lo) * rng.generateDouble(); };
    for (int i = 0; i < 3000; ++i) {
        CGContextSetRGBStrokeColor(ctx, r(0, 1), r(0, 1), r(0, 1), 1);
        CGContextSetLineWidth(ctx, r(0.2, 2.5));
        CGContextMoveToPoint(ctx, r(0, media.size.width), r(0, media.size.height));
        for (int k = 0; k < 3; ++k)
            CGContextAddCurveToPoint(ctx, r(0, media.size.width), r(0, media.size.height), r(0, media.size.width),
                                     r(0, media.size.height), r(0, media.size.width), r(0, media.size.height));
        CGContextStrokePath(ctx);
    }
    for (int i = 0; i < 400; ++i) {
        CGContextSetRGBFillColor(ctx, r(0, 1), r(0, 1), r(0, 1), 1);
        const CGFloat cx = r(0, media.size.width), cy = r(0, media.size.height);
        CGContextMoveToPoint(ctx, cx, cy);
        for (int k = 0; k < 7; ++k)
            CGContextAddLineToPoint(ctx, cx + r(-40, 40), cy + r(-40, 40));
        CGContextClosePath(ctx);
        CGContextEOFillPath(ctx);
    }
}

void drawTransparencyPage(CGContextRef ctx, CGRect media, int page)
{
    QRandomGenerator rng(quint32(2000 + page));
    auto r = [&](double lo, double hi) { return lo + (hi - lo) * rng.generateDouble(); };
    const CGBlendMode modes[] = {kCGBlendModeMultiply, kCGBlendModeScreen, kCGBlendModeOverlay,
                                 kCGBlendModeColorBurn, kCGBlendModeDifference, kCGBlendModeLuminosity};
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    for (int g = 0; g < 24; ++g) {
        CGContextSaveGState(ctx);
        CGContextSetAlpha(ctx, CGFloat(r(0.4, 0.9)));
        CGContextSetBlendMode(ctx, modes[g % 6]);
        CGContextBeginTransparencyLayer(ctx, nullptr); // a transparency group in the PDF
        for (int k = 0; k < 6; ++k) {
            CGContextSaveGState(ctx);
            CGContextSetBlendMode(ctx, modes[(g + k) % 6]);
            CGContextBeginTransparencyLayer(ctx, nullptr); // nested group
            CGContextSetRGBFillColor(ctx, r(0, 1), r(0, 1), r(0, 1), CGFloat(r(0.3, 0.8)));
            const CGRect e = CGRectMake(r(0, media.size.width - 200), r(0, media.size.height - 200), r(80, 260), r(80, 260));
            CGContextFillEllipseInRect(ctx, e);
            const CGFloat comps[] = {CGFloat(r(0, 1)), CGFloat(r(0, 1)), CGFloat(r(0, 1)), 0.9,
                                     CGFloat(r(0, 1)), CGFloat(r(0, 1)), CGFloat(r(0, 1)), 0.0};
            CGGradientRef grad = CGGradientCreateWithColorComponents(cs, comps, nullptr, 2);
            CGContextAddRect(ctx, CGRectInset(e, -30, -30));
            CGContextClip(ctx);
            CGContextDrawLinearGradient(ctx, grad, e.origin, CGPointMake(CGRectGetMaxX(e), CGRectGetMaxY(e)), 0); // alpha shading: a soft mask
            CGGradientRelease(grad);
            CGContextEndTransparencyLayer(ctx);
            CGContextRestoreGState(ctx);
        }
        CGContextEndTransparencyLayer(ctx);
        CGContextRestoreGState(ctx);
    }
    CGColorSpaceRelease(cs);
}

// A photo-like 300 dpi scan: smooth gradients plus grain (so JPEG has
// realistic work to do), different on every page.
CGImageRef scannedImage(int page, int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    QRandomGenerator rng(quint32(3000 + page));
    const int ox = int(rng.bounded(255)), oy = int(rng.bounded(255));
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int n = int(rng.generate() & 15);
            const int v = 205 + ((x / 7 + y / 13 + ox) & 31) + n - 8; // paper
            const bool ink = ((x + oy) / 40) % 9 == 0 && ((y / 22) % 3 != 2); // text-like strokes
            const int c = ink ? 40 + n : qBound(0, v, 255);
            line[x] = qRgb(c, c - (ink ? 0 : 3), c - (ink ? 0 : 9));
        }
    }
    NSMutableData *jpeg = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, nullptr);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef bc = CGBitmapContextCreate(img.bits(), size_t(w), size_t(h), 8, size_t(img.bytesPerLine()), cs,
                                            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGImageRef raw = CGBitmapContextCreateImage(bc);
    NSDictionary *props = @{(id)kCGImageDestinationLossyCompressionQuality: @0.7};
    CGImageDestinationAddImage(dst, raw, (__bridge CFDictionaryRef)props);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(raw);
    CGContextRelease(bc);
    // Built from JPEG data, so the PDF keeps it as a DCT stream (like a scanner).
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)jpeg);
    CGImageRef out = CGImageCreateWithJPEGDataProvider(dp, nullptr, true, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return out;
}

} // namespace

bool writeFixturePdf(const QString &path, const QString &style, int pages)
{
    bool ok = false;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
        CGRect letter = CGRectMake(0, 0, 612, 792);
        CGContextRef ctx = CGPDFContextCreateWithURL((__bridge CFURLRef)url, &letter, nullptr);
        if (!ctx)
            return false;
        for (int p = 0; p < pages; ++p) {
            CGRect media = letter;
            if (style == QLatin1String("mixed")) {
                const CGRect sizes[] = {letter, CGRectMake(0, 0, 595, 842), CGRectMake(0, 0, 792, 612),
                                        CGRectMake(0, 0, 226, 600)};
                media = sizes[p % 4];
            }
            NSData *box = [NSData dataWithBytes:&media length:sizeof(media)];
            CGPDFContextBeginPage(ctx, (__bridge CFDictionaryRef) @{(id)kCGPDFContextMediaBox: box});
            if (style == QLatin1String("vector")) {
                drawVectorPage(ctx, media, p);
            } else if (style == QLatin1String("transparency")) {
                drawTextPage(ctx, media, p);
                drawTransparencyPage(ctx, media, p);
            } else if (style == QLatin1String("scanned")) {
                CGImageRef img = scannedImage(p, 2550, 3300);
                if (img) {
                    CGContextDrawImage(ctx, media, img);
                    CGImageRelease(img);
                }
            } else {
                drawTextPage(ctx, media, p);
            }
            if (style == QLatin1String("mixed")) { // corner markers for orientation checks
                CGContextSetRGBFillColor(ctx, 1, 0, 0, 1);
                CGContextFillRect(ctx, CGRectMake(0, media.size.height - 40, 40, 40)); // top-left
                CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
                CGContextFillRect(ctx, CGRectMake(media.size.width - 40, 0, 40, 40)); // bottom-right
            }
            CGPDFContextEndPage(ctx);
        }
        CGPDFContextClose(ctx);
        CGContextRelease(ctx);
        ok = QFileInfo::exists(path);
    }
    return ok;
}

} // namespace folia::platform
