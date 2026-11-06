// makescan <out.pdf> <imgW> <imgH> <pages> [gray]: Letter pages, one JPEG scan each (DCT passthrough).
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    const int w = atoi(argv[2]), h = atoi(argv[3]), pages = atoi(argv[4]);
    const bool gray = argc > 5 && atoi(argv[5]);
    @autoreleasepool {
        CGRect letter = CGRectMake(0, 0, 612, 792);
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        CGContextRef pdf = CGPDFContextCreateWithURL((__bridge CFURLRef)url, &letter, nullptr);
        std::vector<uint32_t> px(size_t(w) * h);
        for (int p = 0; p < pages; ++p) {
            srand(3000 + p);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int n = rand() & 15;
                    const bool ink = ((x + p * 7) / 40) % 9 == 0 && ((y / 22) % 3 != 2);
                    int c = ink ? 40 + n : 205 + ((x / 7 + y / 13 + p) & 31) + n - 8;
                    c = c < 0 ? 0 : c > 255 ? 255 : c;
                    px[size_t(y) * w + x] = 0xff000000u | (c << 16) | ((c - (ink ? 0 : 3)) << 8) | (c - (ink ? 0 : 9));
                }
            CGColorSpaceRef cs = gray ? CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2)
                                      : CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef bc = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, rgb, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
            CGImageRef raw = CGBitmapContextCreateImage(bc);
            if (gray) {
                CGContextRef gc = CGBitmapContextCreate(nullptr, w, h, 8, w, cs, kCGImageAlphaNone);
                CGContextDrawImage(gc, CGRectMake(0, 0, w, h), raw);
                CGImageRelease(raw);
                raw = CGBitmapContextCreateImage(gc);
                CGContextRelease(gc);
            }
            NSMutableData *jpeg = [NSMutableData data];
            CGImageDestinationRef dst = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, nullptr);
            CGImageDestinationAddImage(dst, raw, (__bridge CFDictionaryRef) @{(id)kCGImageDestinationLossyCompressionQuality: @0.7});
            CGImageDestinationFinalize(dst);
            CFRelease(dst);
            CGImageRelease(raw);
            CGContextRelease(bc);
            CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)jpeg);
            CGImageRef img = CGImageCreateWithJPEGDataProvider(dp, nullptr, true, kCGRenderingIntentDefault);
            CGDataProviderRelease(dp);
            CGPDFContextBeginPage(pdf, nullptr);
            CGContextDrawImage(pdf, letter, img);
            CGPDFContextEndPage(pdf);
            CGImageRelease(img);
            CGColorSpaceRelease(cs);
            CGColorSpaceRelease(rgb);
        }
        CGPDFContextClose(pdf);
        CGContextRelease(pdf);
    }
    return 0;
}
