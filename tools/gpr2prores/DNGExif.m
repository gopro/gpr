// DNGExif.m — read EXIF / DNG tags via ImageIO and (de)serialize to a compact
// binary blob suitable for storage as MOV side-data.
//
// Why ImageIO and not LibRaw: ImageIO exposes the full EXIF dictionary
// (CGImageMetadata) including all the tags we want (ISO, ExposureTime, etc.)
// without unpacking the raw mosaic. It's significantly faster than libraw_open
// + libraw_unpack when all you want is the metadata.

#import "DNGExif.h"

#import <ImageIO/ImageIO.h>
#import <CoreFoundation/CoreFoundation.h>

@implementation DNGExifInfo
@end

// --- helpers ---------------------------------------------------------------

static double numberOrZero(id obj) {
    if (![obj isKindOfClass:[NSNumber class]]) return 0;
    return [(NSNumber *)obj doubleValue];
}

static NSString *stringOrNil(id obj) {
    if (![obj isKindOfClass:[NSString class]]) return nil;
    return (NSString *)obj;
}

@implementation DNGExif

+ (nullable DNGExifInfo *)readFromPath:(NSString *)path {
    if (!path) return nil;
    NSURL *url = [NSURL fileURLWithPath:path];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    if (!src) {
        fprintf(stderr, "DNGExif: cannot open %s\n", [path UTF8String]);
        return nil;
    }
    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    if (!props) {
        CFRelease(src);
        fprintf(stderr, "DNGExif: cannot read props for %s\n", [path UTF8String]);
        return nil;
    }
    NSDictionary *all = (__bridge NSDictionary *)props;
    NSDictionary *exif = all[(NSString *)kCGImagePropertyExifDictionary];
    NSDictionary *tiff = all[(NSString *)kCGImagePropertyTIFFDictionary];
    NSDictionary *dng  = all[(NSString *)kCGImagePropertyDNGDictionary];

    DNGExifInfo *out = [DNGExifInfo new];

    // ISO: exif/ISOSpeedRatings is typically an array, take the first.
    id iso = exif[(NSString *)kCGImagePropertyExifISOSpeedRatings];
    if ([iso isKindOfClass:[NSArray class]] && ((NSArray *)iso).count > 0) {
        out.iso = numberOrZero(((NSArray *)iso).firstObject);
    } else {
        out.iso = numberOrZero(iso);
    }
    out.exposureTime = numberOrZero(exif[(NSString *)kCGImagePropertyExifExposureTime]);
    out.aperture     = numberOrZero(exif[(NSString *)kCGImagePropertyExifFNumber]);
    out.focalLength  = numberOrZero(exif[(NSString *)kCGImagePropertyExifFocalLength]);

    // Lens model: Exif LensModel (sometimes ExifAux). Fall back to TIFF for some sensors.
    out.lensModel = stringOrNil(exif[(NSString *)kCGImagePropertyExifLensModel]);

    out.cameraModel = stringOrNil(tiff[(NSString *)kCGImagePropertyTIFFModel]);
    out.cameraMake  = stringOrNil(tiff[(NSString *)kCGImagePropertyTIFFMake]);
    out.createDate  = stringOrNil(exif[(NSString *)kCGImagePropertyExifDateTimeOriginal])
                   ?: stringOrNil(tiff[(NSString *)kCGImagePropertyTIFFDateTime]);

    // AsShotNeutral (DNG tag 50932). ImageIO exposes it in the DNG dictionary
    // as "{DNG}/AsShotNeutral" - usually an array of 3 floats.
    id asn = dng[(NSString *)kCGImagePropertyDNGAsShotNeutral];
    if (!asn) asn = dng[@"AsShotNeutral"];
    if ([asn isKindOfClass:[NSArray class]] && ((NSArray *)asn).count >= 3) {
        out.asShotNeutral = @[((NSArray *)asn)[0], ((NSArray *)asn)[1], ((NSArray *)asn)[2]];
    }

    CFRelease(props);
    CFRelease(src);
    return out;
}

// --- binary (de)serialization ---------------------------------------------

static void appendDouble(NSMutableData *d, double v) {
    [d appendBytes:&v length:sizeof(double)];
}
static void appendU16(NSMutableData *d, uint16_t v) {
    [d appendBytes:&v length:sizeof(uint16_t)];
}
static void appendStr(NSMutableData *d, NSString *s) {
    NSData *u = [(s ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
    uint16_t L = (uint16_t)MIN(u.length, (NSUInteger)0xFFFF);
    appendU16(d, L);
    if (L > 0) [d appendBytes:u.bytes length:L];
}

+ (nullable NSData *)serialize:(DNGExifInfo *)exif {
    if (!exif) return nil;
    NSMutableData *d = [NSMutableData dataWithCapacity:256];
    const char *magic = "GEXF";
    [d appendBytes:magic length:4];
    appendU16(d, 1);                                   // version
    appendU16(d, 0);                                   // flags
    appendDouble(d, exif.iso);
    appendDouble(d, exif.exposureTime);
    appendDouble(d, exif.aperture);
    appendDouble(d, exif.focalLength);
    double wbR = NAN, wbG = NAN, wbB = NAN;
    if (exif.asShotNeutral.count >= 3) {
        wbR = exif.asShotNeutral[0].doubleValue;
        wbG = exif.asShotNeutral[1].doubleValue;
        wbB = exif.asShotNeutral[2].doubleValue;
    }
    appendDouble(d, wbR);
    appendDouble(d, wbG);
    appendDouble(d, wbB);
    appendStr(d, exif.lensModel);
    appendStr(d, exif.cameraModel);
    appendStr(d, exif.createDate);
    return d;
}

+ (nullable DNGExifInfo *)deserialize:(NSData *)data {
    if (!data || data.length < 4 + 2 + 2 + 7*sizeof(double) + 6) return nil;
    const uint8_t *p = data.bytes;
    if (memcmp(p, "GEXF", 4) != 0) return nil;
    size_t off = 4;
    uint16_t version = *(const uint16_t *)(p + off); off += 2;
    uint16_t flags   = *(const uint16_t *)(p + off); off += 2;
    (void)flags;
    if (version != 1) return nil;

    DNGExifInfo *out = [DNGExifInfo new];
    out.iso          = *(const double *)(p + off); off += 8;
    out.exposureTime = *(const double *)(p + off); off += 8;
    out.aperture     = *(const double *)(p + off); off += 8;
    out.focalLength  = *(const double *)(p + off); off += 8;
    double wbR = *(const double *)(p + off); off += 8;
    double wbG = *(const double *)(p + off); off += 8;
    double wbB = *(const double *)(p + off); off += 8;
    if (!isnan(wbR) && !isnan(wbG) && !isnan(wbB)) {
        out.asShotNeutral = @[@(wbR), @(wbG), @(wbB)];
    }

    // Read three length-prefixed strings.
    __block size_t off2 = off;
    NSString *(^readStr)(void) = ^NSString *(void) {
        if (off2 + 2 > data.length) return nil;
        uint16_t L = *(const uint16_t *)(p + off2); off2 += 2;
        if (off2 + L > data.length) return nil;
        NSString *s = nil;
        if (L > 0) {
            s = [[NSString alloc] initWithBytes:(p + off2) length:L encoding:NSUTF8StringEncoding];
        }
        off2 += L;
        return s;
    };
    out.lensModel   = readStr();
    out.cameraModel = readStr();
    out.createDate  = readStr();
    return out;
}

@end
