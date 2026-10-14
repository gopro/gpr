// GPRCodec.h — wraps the fused encoder + decoder (reusable context).

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface GPRCodec : NSObject

- (nullable instancetype)initWithWidth:(int)width
                                height:(int)height
                          pixelFormat:(int)pixelFormat
                              quality:(int)quality;

// Encode one frame. Returns 0 on success; vc5_out/vc5_size point into the
// context-owned output buffer (do NOT free).
- (int)encodeRawBayer:(const uint16_t *)bayer
                bytes:(size_t)bytes
              vc5Out:(uint8_t **)vc5Out
             vc5Size:(size_t *)vc5Size;

// Decode an encoded blob. outBayer must hold at least
// (out_width * out_height * 2) bytes worth of uint16. outPitch is in bytes.
- (int)decode:(const uint8_t *)enc
         size:(size_t)size
     outBayer:(uint16_t *)outBayer
     outPitch:(size_t)outPitch
     outWidth:(int *)outWidth
    outHeight:(int *)outHeight;

// Decode a full TIFF/GPR container through the lightweight GPR parser + VC5
// path. Used for .gvid streams whose frame payloads are complete GPR files
// rather than direct FUSD payloads.
- (int)decodeGPRContainer:(const uint8_t *)enc
                     size:(size_t)size
                 outBayer:(uint16_t *)outBayer
                 outPitch:(size_t)outPitch
                 outWidth:(int *)outWidth
                outHeight:(int *)outHeight;

// Half-resolution decode — same args as -decode:, but the output is at
// (header.width / 2) × (header.height / 2). For FUSED multi-level streams,
// this skips the level-1 inverse wavelet so it's ~1.5–2× faster than the
// full-res path. This is the playback default: the CNN consumes the
// half-res bayer directly, mirroring the pre-FUSED GPRCodec topology.
- (int)decodeHalfRes:(const uint8_t *)enc
                size:(size_t)size
            outBayer:(uint16_t *)outBayer
            outPitch:(size_t)outPitch
            outWidth:(int *)outWidth
           outHeight:(int *)outHeight;

@end

NS_ASSUME_NONNULL_END
