// DNGExif.h — extract per-frame EXIF / DNG metadata from a DNG file.
//
// Reads ISO, shutter, aperture, focal length, lens model, AsShotNeutral (WB)
// via ImageIO (CGImageSourceCopyPropertiesAtIndex). These fields are embedded
// alongside encoded GPR packets in the GVIDMov container so that downstream
// tools can recover per-frame exposure/WB metadata without holding the
// originating DNG.
//
// The struct uses sentinel values (NaN / nil / 0) when a tag is absent.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface DNGExifInfo : NSObject
@property (nonatomic, assign) double iso;            // ISOSpeedRatings; 0 if absent
@property (nonatomic, assign) double exposureTime;   // seconds; 0 if absent (e.g. 1/250 = 0.004)
@property (nonatomic, assign) double aperture;       // f-number, e.g. 1.8; 0 if absent
@property (nonatomic, assign) double focalLength;    // mm; 0 if absent
@property (nonatomic, copy, nullable) NSString *lensModel;
@property (nonatomic, copy, nullable) NSString *cameraModel;
@property (nonatomic, copy, nullable) NSString *cameraMake;
// AsShotNeutral, length 3 (R/G/B inverse-WB multipliers stored in DNG).
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *asShotNeutral;
// Original CreateDate / DateTimeOriginal (string, EXIF format)
@property (nonatomic, copy, nullable) NSString *createDate;
@end

@interface DNGExif : NSObject

// Read EXIF/DNG metadata from `path`. Returns nil on failure. Tags that aren't
// present default to 0 / nil — never nil-out the whole object.
+ (nullable DNGExifInfo *)readFromPath:(NSString *)path;

// Serialize a DNGExifInfo to a compact NSData binary blob (for storage as
// MOV side-data or atom payload). Returns nil if exif is nil.
//
// Format (little-endian):
//   magic     : 4 bytes  "GEXF"
//   version   : uint16   = 1
//   flags     : uint16   = 0 (reserved)
//   iso       : double
//   exposure  : double
//   aperture  : double
//   focalLen  : double
//   wbR       : double   (or NaN if absent)
//   wbG       : double
//   wbB       : double
//   lensLen   : uint16   (bytes)
//   lens[...] : UTF-8 bytes
//   modelLen  : uint16
//   model[..] : UTF-8 bytes
//   dateLen   : uint16
//   date[...] : UTF-8 bytes
+ (nullable NSData *)serialize:(DNGExifInfo *)exif;

// Deserialize from the above format. Returns nil on bad magic / truncation.
+ (nullable DNGExifInfo *)deserialize:(NSData *)data;

@end

NS_ASSUME_NONNULL_END
