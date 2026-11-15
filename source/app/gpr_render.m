// Optional macOS RAW renderer. Camera calibration is read from the actual DNG.
// No stock SDK RGB conversion, synthetic camera profile, or embedded preview.
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreImage/CIRAWFilter.h>
#import <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

static int fail(NSString *message) {
    fprintf(stderr, "gpr_render: %s\n", message.UTF8String);
    return 1;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--version")) {
            puts("gpr_render 1; macOS CIRAWFilter; 16-bit Rec.709 TIFF");
            return 0;
        }
        if (argc != 3) return fail(@"usage: gpr_render input.dng output.tiff");
        NSString *source = [NSString stringWithUTF8String:argv[1]];
        NSString *destination = [NSString stringWithUTF8String:argv[2]];
        struct stat info;
        if (!source || !destination || stat(argv[1], &info) || !S_ISREG(info.st_mode) ||
            info.st_size <= 0 || info.st_size > (off_t)2*1024*1024*1024)
            return fail(@"input must be a nonempty regular RAW file of at most 2 GiB");
        if (@available(macOS 12.0, *)) {
            CIRAWFilter *raw = [CIRAWFilter filterWithImageURL:[NSURL fileURLWithPath:source]];
            if (!raw || !raw.supportedDecoderVersions.count ||
                [raw.decoderVersion isEqual:CIRAWDecoderVersionNone])
                return fail(@"RAW decoder does not support this input");
            // Keep the source's white balance, baseline exposure, lens correction,
            // crop and EXIF orientation. Remove adjustable enhancement, while
            // retaining the RAW decoder's camera-to-working-space conversion.
            raw.scaleFactor = 1;
            raw.draftModeEnabled = NO;
            raw.exposure = 0;
            raw.boostAmount = 0;
            raw.boostShadowAmount = 0;
            raw.localToneMapAmount = 0;
            raw.extendedDynamicRangeAmount = 0;
            if (raw.luminanceNoiseReductionSupported) raw.luminanceNoiseReductionAmount = 0;
            if (raw.colorNoiseReductionSupported) raw.colorNoiseReductionAmount = 0;
            if (raw.sharpnessSupported) raw.sharpnessAmount = 0;
            if (raw.detailSupported) raw.detailAmount = 0;
            if (raw.contrastSupported) raw.contrastAmount = 0;
            if (raw.moireReductionSupported) raw.moireReductionAmount = 0;
            CIImage *image = raw.outputImage;
            CGRect extent = image ? image.extent : CGRectZero;
            if (!image || !isfinite(extent.origin.x) || !isfinite(extent.origin.y) ||
                !isfinite(extent.size.width) || !isfinite(extent.size.height) ||
                extent.size.width < 1 || extent.size.height < 1 ||
                extent.size.width > 32768 || extent.size.height > 32768 ||
                extent.size.width*extent.size.height > 128000000 ||
                extent.size.width != floor(extent.size.width) || extent.size.height != floor(extent.size.height))
                return fail(@"RAW decoder returned an invalid or oversized image extent");
            image = [image imageByApplyingTransform:CGAffineTransformMakeTranslation(-extent.origin.x, -extent.origin.y)];
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
            if (!space) return fail(@"Rec.709 color space unavailable");
            CIContext *context = [CIContext contextWithOptions:@{kCIContextCacheIntermediates:@NO}];
            NSData *data = [context TIFFRepresentationOfImage:image format:kCIFormatRGBA16
                                                  colorSpace:space options:@{}];
            CGColorSpaceRelease(space);
            if (!data.length) return fail(@"RAW rendering failed");
            // Rendering must succeed before creating an exclusive output name.
            int fd = open(argv[2], O_WRONLY|O_CREAT|O_EXCL, 0666);
            if (fd < 0) return fail(@"cannot exclusively create output");
            const unsigned char *bytes = data.bytes;
            size_t remaining = data.length;
            BOOL ok = YES;
            while (remaining) {
                ssize_t count = write(fd, bytes, remaining);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) { ok = NO; break; }
                remaining -= (size_t)count;
                bytes += count;
            }
            if (close(fd)) ok = NO;
            if (!ok) { unlink(argv[2]); return fail(@"failed writing output"); }
            NSDictionary *report = @{
                @"schema":@"gpr.raw_render.v1", @"decoder":raw.decoderVersion,
                @"os_version":[NSProcessInfo processInfo].operatingSystemVersionString,
                @"width":@(extent.size.width), @"height":@(extent.size.height),
                @"native_width":@(raw.nativeSize.width), @"native_height":@(raw.nativeSize.height),
                @"source_orientation":@(raw.orientation), @"bits_per_component":@16,
                @"color_space":@"ITU-R BT.709", @"transfer":@"BT.709",
                @"baseline_exposure":@(raw.baselineExposure),
                @"neutral_temperature":@(raw.neutralTemperature), @"neutral_tint":@(raw.neutralTint),
                @"lens_correction_supported":@(raw.lensCorrectionSupported),
                @"lens_correction_enabled":@(raw.lensCorrectionEnabled),
                @"global_tone_amount":@0, @"local_tone_amount":@0, @"draft":@NO
            };
            NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:NULL];
            if (!json) { unlink(argv[2]); return fail(@"failed serializing renderer receipt"); }
            fwrite(json.bytes, 1, json.length, stdout);
            putchar('\n');
            return 0;
        }
        return fail(@"macOS 12 or newer is required");
    }
}
