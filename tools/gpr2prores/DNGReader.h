// DNGReader.h — Read a DNG file's raw Bayer data + metadata via ImageIO/TIFF.
//
// We bypass CIRAWFilter for raw extraction because CIRAWFilter performs
// demosaic + tone curve and we want the raw mosaic. Instead we read the
// SubIFD with PhotometricInterpretation=CFA (32803) directly via
// CGImageSourceCopyPropertiesAtIndex + a small TIFF strip parser.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct {
    uint32_t width;          // Bayer width  (e.g. 8280)
    uint32_t height;         // Bayer height (e.g. 5520)
    uint32_t bitsPerSample;  // 14 or 16 typically
    // CFA pattern: 0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR (we only care about first two).
    uint32_t cfaPattern;
    uint32_t blackLevel;     // single-channel approximation
    uint32_t whiteLevel;
    float    wbR, wbG, wbB;  // camera-neutral white-balance multipliers
    // 3x3 cameraRGB → sRGB matrix (taken from LibRaw rgb_cam after unpack).
    // Apply after WB to convert sensor-RGB into displayable sRGB.
    float    rgb_cam[3][3];
} DNGInfo;

@interface DNGReader : NSObject

// Read the raw Bayer plane from `path` into a freshly-allocated uint16 buffer.
// Returns NULL on failure. Caller owns the returned buffer (use free()).
// `info` is filled in on success.
+ (uint16_t *)readBayerFromPath:(NSString *)path info:(DNGInfo *)info;

@end

NS_ASSUME_NONNULL_END
