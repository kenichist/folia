// macOS platform probes: Mach task counters, thermal state, accessibility
// preferences, dispatch memory-pressure notifications, and the ImageIO image
// decoder (docs/ATTACHMENTS.md §3).
#include "app/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <QColorSpace>
#include <QMetaObject>
#include <QCoreApplication>
#include <dispatch/dispatch.h>
#include <libproc.h>
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

} // namespace folia::platform
