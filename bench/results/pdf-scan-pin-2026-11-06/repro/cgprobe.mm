// cgprobe: open a PDF, draw page(s) whole or tiled at a given width, close; repeat.
// Prints footprint and DefaultPurgeableMallocZone stats after each close.
//   cgprobe <pdf> <pageWidthPx> <cycles> [tile] [pagesPerCycle] [mode]
// mode: 0 = draw page as Folia does; 1 = same + CGContextSetInterpolationQuality none
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <time.h>
#import <ImageIO/ImageIO.h>

static long long footprint()
{
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) != KERN_SUCCESS)
        return -1;
    return (long long)info.phys_footprint;
}

static void zoneStats(const char *tag)
{
    malloc_zone_t *pz = malloc_default_purgeable_zone();
    malloc_statistics_t st{};
    malloc_zone_statistics(pz, &st);
    printf("%s footprint=%.1fMB purgeable_zone in_use=%.1fMB blocks=%u alloc=%.1fMB\n", tag, footprint() / 1e6,
           st.size_in_use / 1e6, st.blocks_in_use, st.size_allocated / 1e6);
    fflush(stdout);
}

static int g_bands = 1;
static double g_cpu = 0;
static double threadCpu()
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void drawRegion(CGPDFPageRef page, int pageW, CGRect region, int mode)
{
    const double c0 = threadCpu();
    const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    const CGFloat dw = box.size.width, dh = box.size.height;
    const CGFloat s = CGFloat(pageW) / dw;
    const CGFloat pageH = dh * s;
    const size_t w = size_t(region.size.width), h = size_t(region.size.height);
    void *bits = calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(bits, w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGColorSpaceRelease(cs);
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    const int bands = g_bands;
    const size_t bh = (h + bands - 1) / bands;
    for (int b = 0; b < bands; ++b) {
        CGContextSaveGState(ctx);
        if (bands > 1)
            CGContextClipToRect(ctx, CGRectMake(0, CGFloat(b * bh), w, CGFloat(MIN(bh, h - b * bh))));
        CGContextTranslateCTM(ctx, -region.origin.x, -(pageH - region.origin.y - region.size.height));
        CGContextScaleCTM(ctx, s, s);
        CGContextConcatCTM(ctx, CGPDFPageGetDrawingTransform(page, kCGPDFCropBox, CGRectMake(0, 0, dw, dh), 0, true));
        CGContextClipToRect(ctx, box);
        static const CGInterpolationQuality q[] = {kCGInterpolationDefault, kCGInterpolationNone, kCGInterpolationLow, kCGInterpolationMedium, kCGInterpolationHigh};
        CGContextSetInterpolationQuality(ctx, q[mode]);
        CGContextDrawPDFPage(ctx, page);
        CGContextRestoreGState(ctx);
    }
    if (getenv("SAVE")) {
        CGImageRef im = CGBitmapContextCreateImage(ctx);
        NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:getenv("SAVE")]];
        CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)u, CFSTR("public.png"), 1, nullptr);
        CGImageDestinationAddImage(d, im, nullptr);
        CGImageDestinationFinalize(d);
        CFRelease(d);
        CGImageRelease(im);
    }
    CGContextRelease(ctx);
    free(bits);
    g_cpu += threadCpu() - c0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage\n");
        return 1;
    }
    const char *path = argv[1];
    const int pageW = atoi(argv[2]);
    const int cycles = atoi(argv[3]);
    const int tile = argc > 4 ? atoi(argv[4]) : 0;
    const int perCycle = argc > 5 ? atoi(argv[5]) : 1;
    const int mode = argc > 6 ? atoi(argv[6]) : 0;
    if (getenv("BANDS"))
        g_bands = atoi(getenv("BANDS"));
    zoneStats("start");
    int nextPage = 0;
    for (int c = 0; c < cycles; ++c) {
        @autoreleasepool {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
            CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL((__bridge CFURLRef)url);
            const size_t n = CGPDFDocumentGetNumberOfPages(doc);
            for (int k = 0; k < perCycle; ++k) {
                CGPDFPageRef page = CGPDFDocumentGetPage(doc, size_t(nextPage % n) + 1);
                ++nextPage;
                const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
                const int pageH = int(pageW * box.size.height / box.size.width + 0.5);
                if (tile <= 0) {
                    drawRegion(page, pageW, CGRectMake(0, 0, pageW, pageH), mode);
                } else {
                    for (int y = 0; y < pageH; y += tile)
                        for (int x = 0; x < pageW; x += tile)
                            drawRegion(page, pageW, CGRectMake(x, y, MIN(tile, pageW - x), MIN(tile, pageH - y)), mode);
                }
            }
            CGPDFDocumentRelease(doc);
        }
        if (getenv("RELIEF")) {
            size_t fr = malloc_zone_pressure_relief(nullptr, 0);
            printf("relief freed %.1fMB\n", fr / 1e6);
        }
        char tag[32];
        snprintf(tag, sizeof tag, "cycle %2d", c + 1);
        zoneStats(tag);
    }
    printf("cpu_ms_per_cycle=%.1f\n", g_cpu * 1000 / cycles);
    if (getenv("HOLD"))
        sleep(atoi(getenv("HOLD")));
    return 0;
}
