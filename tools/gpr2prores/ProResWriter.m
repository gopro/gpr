// ProResWriter.m — AVAssetWriter-based ProRes 422 HQ writer.
//
// Uses AVAssetWriter + AVAssetWriterInputPixelBufferAdaptor. This wraps
// VideoToolbox's hardware ProRes encoder under the hood. We feed it
// CVPixelBuffer-backed IOSurfaces that the demosaic kernel renders into,
// which means the Metal texture and VideoToolbox encode share GPU-accessible
// memory with no CPU copy.

#import "ProResWriter.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

@implementation ProResWriter {
    AVAssetWriter                          *_writer;
    AVAssetWriterInput                     *_input;
    AVAssetWriterInputPixelBufferAdaptor   *_adaptor;
    CVPixelBufferPoolRef                    _pool;
    uint32_t                                _width;
    uint32_t                                _height;
    int                                     _fps;
    int                                     _framesAppended;
    NSString                               *_path;
}

- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                  fps:(int)fps
                               device:(id<MTLDevice>)device
{
    self = [super init];
    if (!self) return nil;
    _width = width;
    _height = height;
    _fps = fps;
    _framesAppended = 0;
    _path = path;

    // Remove existing file
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];

    NSError *err = nil;
    NSURL *url = [NSURL fileURLWithPath:path];
    _writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeQuickTimeMovie error:&err];
    if (!_writer) {
        fprintf(stderr, "ProResWriter: AVAssetWriter init failed: %s\n",
                [err.localizedDescription UTF8String]);
        return nil;
    }

    // Use ProRes 422 HQ (or 422 if not available)
    NSDictionary *outputSettings = @{
        AVVideoCodecKey: AVVideoCodecTypeAppleProRes422HQ,
        AVVideoWidthKey: @(width),
        AVVideoHeightKey: @(height),
    };
    _input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                outputSettings:outputSettings];
    _input.expectsMediaDataInRealTime = NO;
    _input.mediaTimeScale = fps;
    NSDictionary *sourceAttrs = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString *)kCVPixelBufferWidthKey: @(width),
        (NSString *)kCVPixelBufferHeightKey: @(height),
        (NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    _adaptor = [AVAssetWriterInputPixelBufferAdaptor
                assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_input
                                            sourcePixelBufferAttributes:sourceAttrs];

    if ([_writer canAddInput:_input]) [_writer addInput:_input];
    else { fprintf(stderr, "ProResWriter: cannot add input\n"); return nil; }
    if (![_writer startWriting]) {
        fprintf(stderr, "ProResWriter: startWriting failed: %s\n",
                [_writer.error.localizedDescription UTF8String]);
        return nil;
    }
    [_writer startSessionAtSourceTime:kCMTimeZero];

    fprintf(stderr, "ProResWriter: %ux%u @ %d fps → %s\n",
            width, height, fps, [path UTF8String]);
    return self;
}

- (nullable CVPixelBufferRef)pixelBuffer {
    // Lazy: pool is owned by the adaptor.
    if (!_adaptor.pixelBufferPool) {
        fprintf(stderr, "ProResWriter: pool not ready yet\n");
        return NULL;
    }
    CVPixelBufferRef pb = NULL;
    CVReturn r = CVPixelBufferPoolCreatePixelBuffer(NULL, _adaptor.pixelBufferPool, &pb);
    if (r != kCVReturnSuccess) { fprintf(stderr, "pool buf alloc r=%d\n", r); return NULL; }
    // Return autoreleased so caller doesn't have to free; we transfer ownership
    // back to AVAssetWriterInputPixelBufferAdaptor via append.
    return (CVPixelBufferRef)CFAutorelease(pb);
}

- (int)appendPixelBuffer:(CVPixelBufferRef)pb frameIndex:(int)idx {
    // Wait until input is ready (basic backpressure)
    int spins = 0;
    while (!_input.isReadyForMoreMediaData) {
        usleep(1000);
        if (++spins > 30000) { fprintf(stderr, "input never ready\n"); return -1; }
    }
    CMTime t = CMTimeMake(idx, _fps);
    BOOL ok = [_adaptor appendPixelBuffer:pb withPresentationTime:t];
    if (!ok) {
        fprintf(stderr, "appendPixelBuffer failed: %s (status=%ld)\n",
                _writer.error ? [_writer.error.localizedDescription UTF8String] : "(none)",
                (long)_writer.status);
        return -1;
    }
    if (idx + 1 > _framesAppended) _framesAppended = idx + 1;
    return 0;
}

- (int)finish {
    if (_framesAppended > 0) {
        [_writer endSessionAtSourceTime:CMTimeMake(_framesAppended, _fps)];
    }
    [_input markAsFinished];
    __block BOOL done = NO;
    [_writer finishWritingWithCompletionHandler:^{ done = YES; }];
    while (!done) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]]; }
    if (_writer.status != AVAssetWriterStatusCompleted) {
        fprintf(stderr, "ProResWriter: finish status=%ld err=%s\n", (long)_writer.status,
                [_writer.error.localizedDescription UTF8String]);
        return -1;
    }
    return 0;
}

@end
