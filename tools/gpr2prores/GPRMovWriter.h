// GPRMovWriter.h — write a sequence of encoded GPR frames into a single MOV
// file via libavformat.
//
// Container layout:
//   ftyp + moov (track header for a "data" stream with codec_tag='GPR1')
//   + optional timecode (tmcd) track
//   + optional audio track (PCM s16le or copied AAC)
//   + optional external .exif sidecar carrying per-frame EXIF (GEXF blobs).
//   mdat (concatenated AVPackets, one per frame, payload = raw .gpr bytes)
//
// The video codec_id is AV_CODEC_ID_NONE; only the 4cc codec_tag distinguishes
// the stream. Decoders read packets via av_read_frame() and feed pkt->data
// directly to gpr_decode_fused.
//
// This is a write-only API; the reader counterpart is in GPRMovReader.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class DNGExifInfo;

@interface GPRMovWriter : NSObject

// Open an output MOV. width/height are the decoded bayer dims (e.g. 4140x2760
// for Z8 with decimate=2); they go into the track-header dims. fps is the
// nominal frame rate (e.g. 24).
- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                  fps:(int)fps;

// Optional: enable a timecode track. tcStart is an SMPTE timecode string
// "HH:MM:SS:FF" (or "HH:MM:SS;FF" for drop-frame). dropFrame must be YES for
// fractional frame rates (29.97, 59.94); for 23.976/24/25/30 use NO.
// Must be called BEFORE the first -appendEncoded… call.
- (BOOL)addTimecodeStart:(NSString *)tcStart dropFrame:(BOOL)dropFrame;

// Optional: enable an audio track. Currently supports WAV (s16/s24/f32 PCM)
// passthrough — the input is decoded to s16 PCM and copied into the MOV.
// Must be called BEFORE the first -appendEncoded… call.
- (BOOL)addAudioFromWAV:(NSString *)wavPath;

// Append one encoded frame's raw bytes. Returns 0 on success.
- (int)appendEncodedBytes:(const uint8_t *)bytes length:(size_t)length;

// Append one encoded frame with attached per-frame EXIF metadata. The EXIF
// is serialized via [DNGExif serialize:] into a sibling .exif file.
// Keep that file beside the MOV when copying a recording.
- (int)appendEncodedBytes:(const uint8_t *)bytes
                   length:(size_t)length
                     exif:(nullable DNGExifInfo *)exif;

// Finish + close the file.
- (int)finish;

@end

NS_ASSUME_NONNULL_END
