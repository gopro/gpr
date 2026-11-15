// Deep RGB ProRes transport for already rendered Rec.709 TIFF frames.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>

static void require(BOOL condition, NSString *message) {
    if (!condition) @throw [NSException exceptionWithName:@"gpr_prores" reason:message userInfo:nil];
}
static int64_t integer(id value) {
    require([value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID(), @"expected integer timestamp");
    require(strchr("csilqCSILQ", [value objCType][0]) != NULL, @"timestamp must be an integer");
    require([value compare:@0] != NSOrderedAscending && [value compare:@(INT64_MAX)] != NSOrderedDescending, @"timestamp outside signed 64-bit range");
    return [value longLongValue];
}

// One bounded line per frame lets the producer reuse its TIFF after the
// encoder acknowledges consumption. A stalled or truncated producer fails.
static NSDictionary *stream_record(void) {
    NSMutableData *data = [NSMutableData data];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:180];
    for (;;) {
        int remaining = (int)(deadline.timeIntervalSinceNow*1000);
        require(remaining > 0, @"stream input timed out");
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        int status = poll(&descriptor, 1, remaining);
        if (status < 0 && errno == EINTR) continue;
        require(status > 0, @"stream input timed out or failed");
        unsigned char byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count < 0 && errno == EINTR) continue;
        require(count == 1, @"stream ended before the declared frames and finish record");
        if (byte == '\n') break;
        require(data.length < 65536, @"stream record exceeds 64 KiB");
        [data appendBytes:&byte length:1];
    }
    id record = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    require([record isKindOfClass:NSDictionary.class], @"invalid stream JSON record");
    return record;
}

static void stream_reply(NSString *line) {
    require(fprintf(stdout, "%s\n", line.UTF8String) > 0 && !fflush(stdout), @"stream reply failed");
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--version")) {
            puts("gpr_prores 2; AVFoundation; 16-bit ARGB input; ProRes 422 HQ/4444; streaming");
            return 0;
        }
        if (argc != 3) { fprintf(stderr, "usage: gpr_prores frames.json|--stream output.mov\n"); return 1; }
        BOOL streaming = !strcmp(argv[1], "--stream");
        signal(SIGPIPE, SIG_IGN);
        NSString *temporary = nil;
        AVAssetWriter *writer = nil;
        CGColorSpaceRef space = NULL;
        @try {
            struct stat st;
            NSDictionary *manifest;
            if (streaming) manifest = stream_record();
            else {
                require(!stat(argv[1], &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 16*1024*1024, @"manifest must be a regular file of at most 16 MiB");
                NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]];
                manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
            }
            require([manifest isKindOfClass:NSDictionary.class] && [manifest[@"schema"] isEqual:
                    (streaming ? @"gpr.prores_stream.v1" : @"gpr.prores_frames.v1")], @"invalid manifest schema");
            NSArray *frames = manifest[@"frames"];
            require(streaming || [frames isKindOfClass:NSArray.class], @"invalid frames array");
            int64_t frameCount = streaming ? integer(manifest[@"frame_count"]) : frames.count;
            require(frameCount > 0 && frameCount <= 100000, @"requires 1..100000 frames");
            int64_t timescale = integer(manifest[@"timescale"]);
            int64_t end = integer(manifest[@"end_pts"]);
            require(timescale > 0 && timescale <= INT_MAX && end > 0, @"invalid track clock");
            NSString *profile = manifest[@"profile"];
            require([profile isEqual:@"hq"] || [profile isEqual:@"4444"], @"profile must be hq or 4444");
            NSString *output = [NSString stringWithUTF8String:argv[2]];
            char *pattern = strdup([[output stringByAppendingString:@".tmp.XXXXXX"] fileSystemRepresentation]);
            require(pattern != NULL, @"temporary path allocation failed");
            char *directory = mkdtemp(pattern);
            if (directory) temporary = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:directory length:strlen(directory)];
            free(pattern);
            require(temporary != nil, @"cannot create private output directory");
            NSString *staged = [temporary stringByAppendingPathComponent:@"video.mov"];
            NSError *error = nil;
            writer = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:staged] fileType:AVFileTypeQuickTimeMovie error:&error];
            require(writer != nil, error.localizedDescription ?: @"cannot create writer");
            writer.movieTimeScale = (int32_t)timescale;
            CIContext *context = [CIContext contextWithOptions:@{kCIContextCacheIntermediates:@NO}];
            space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
            require(space != NULL, @"Rec.709 color space unavailable");
            AVAssetWriterInput *input = nil;
            AVAssetWriterInputPixelBufferAdaptor *adaptor = nil;
            int64_t previous = -1;
            int64_t previousNext = 0;
            size_t width = 0, height = 0;
            NSUInteger index = 0;
            if (streaming) stream_reply(@"{\"schema\":\"gpr.prores_stream.ready.v1\"}");
            while (index < (NSUInteger)frameCount) {
              @autoreleasepool {
                id item = streaming ? stream_record() : frames[index];
                require([item isKindOfClass:NSDictionary.class], @"invalid frame record");
                int64_t pts = integer(item[@"pts"]);
                require(pts > previous && pts < end && (previous != -1 || pts == 0), @"non-increasing or out-of-range timestamp");
                require(streaming || index+1 == frames.count || [frames[index+1] isKindOfClass:NSDictionary.class], @"invalid next frame record");
                int64_t next = streaming ? integer(item[@"next_pts"]) :
                    (index+1 < frames.count ? integer(frames[index+1][@"pts"]) : end);
                require(next > pts && next <= end, @"invalid next-frame duration");
                require(!streaming || (pts == previousNext && (index+1 != (NSUInteger)frameCount || next == end)), @"stream frame durations disagree");
                NSString *path = item[@"path"];
                require([path isKindOfClass:NSString.class] && path.isAbsolutePath &&
                        !stat(path.fileSystemRepresentation, &st) && S_ISREG(st.st_mode) &&
                        st.st_size > 0 && st.st_size <= (off_t)2*1024*1024*1024, @"frame must be an absolute regular TIFF path");
                // Only accept our explicit deep Rec.709 intermediate, never RAW
                // or an embedded thumbnail silently decoded through ImageIO.
                CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
                require(source != NULL, @"cannot read TIFF frame");
                BOOL tiff = [(__bridge NSString *)CGImageSourceGetType(source) isEqual:@"public.tiff"];
                NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
                CFRelease(source);
                require(tiff && [properties[(NSString *)kCGImagePropertyDepth] intValue] == 16, @"requires 16-bit TIFF frames");
                CIImage *image = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:path]
                                                     options:@{kCIImageColorSpace:(__bridge id)space}];
                CGRect extent = image ? image.extent : CGRectZero;
                size_t w = [properties[(NSString *)kCGImagePropertyPixelWidth] unsignedIntegerValue];
                size_t h = [properties[(NSString *)kCGImagePropertyPixelHeight] unsignedIntegerValue];
                require(image && w > 0 && h > 0 && w <= 32768 && h <= 32768 && w*h <= 128000000 &&
                        extent.size.width == w && extent.size.height == h, @"invalid frame dimensions");
                if (!input) {
                    width = w; height = h;
                    // Apple's deep-input contract forbids scaling and output color
                    // matching. Pre-render at exact dimensions and tag the buffers.
                    NSDictionary *settings = @{AVVideoCodecKey:[profile isEqual:@"hq"] ? AVVideoCodecTypeAppleProRes422HQ : AVVideoCodecTypeAppleProRes4444,
                                               AVVideoWidthKey:@(w), AVVideoHeightKey:@(h)};
                    require([writer canApplyOutputSettings:settings forMediaType:AVMediaTypeVideo], @"ProRes profile unavailable");
                    input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
                    input.expectsMediaDataInRealTime = NO;
                    input.mediaTimeScale = (int32_t)timescale;
                    adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                         sourcePixelBufferAttributes:@{(NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_64ARGB),
                           (NSString *)kCVPixelBufferWidthKey:@(w), (NSString *)kCVPixelBufferHeightKey:@(h)}];
                    require([writer canAddInput:input], @"cannot add ProRes track");
                    [writer addInput:input];
                    require([writer startWriting], writer.error.localizedDescription ?: @"cannot start writer");
                    [writer startSessionAtSourceTime:kCMTimeZero];
                }
                require(w == width && h == height, @"rendered dimensions changed between frames");
                NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:60];
                while (!input.readyForMoreMediaData && writer.status == AVAssetWriterStatusWriting && deadline.timeIntervalSinceNow > 0) usleep(1000);
                require(input.readyForMoreMediaData && writer.status == AVAssetWriterStatusWriting, writer.error.localizedDescription ?: @"writer timed out");
                CVPixelBufferRef buffer = NULL;
                require(adaptor.pixelBufferPool && CVPixelBufferPoolCreatePixelBuffer(NULL, adaptor.pixelBufferPool, &buffer) == kCVReturnSuccess, @"cannot allocate deep pixel buffer");
                NSMutableData *rgba = [NSMutableData dataWithLength:w*h*8];
                [context render:image toBitmap:rgba.mutableBytes rowBytes:w*8 bounds:extent format:kCIFormatRGBA16 colorSpace:space];
                CVReturn locked = CVPixelBufferLockBaseAddress(buffer, 0);
                if (locked != kCVReturnSuccess) { CVPixelBufferRelease(buffer); require(NO, @"cannot lock pixel buffer"); }
                size_t stride = CVPixelBufferGetBytesPerRow(buffer);
                unsigned char *base = CVPixelBufferGetBaseAddress(buffer);
                if (!base || stride < w*8) { CVPixelBufferUnlockBaseAddress(buffer, 0); CVPixelBufferRelease(buffer); require(NO, @"invalid pixel buffer pitch"); }
                // kCIFormatRGBA16 is native-endian UInt16 RGBA. CoreVideo's
                // kCVPixelFormatType_64ARGB is big-endian UInt16 ARGB.
                const uint16_t *pixels = rgba.bytes;
                for (size_t y=0; y<h; ++y) {
                    uint16_t *row = (uint16_t *)(base+y*stride);
                    for (size_t x=0; x<w; ++x) {
                        const uint16_t *p = pixels+(y*w+x)*4;
                        row[x*4] = CFSwapInt16HostToBig(65535);
                        row[x*4+1] = CFSwapInt16HostToBig(p[0]);
                        row[x*4+2] = CFSwapInt16HostToBig(p[1]);
                        row[x*4+3] = CFSwapInt16HostToBig(p[2]);
                    }
                }
                CVPixelBufferUnlockBaseAddress(buffer, 0);
                CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
                CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
                CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
                CMVideoFormatDescriptionRef description = NULL;
                CMSampleBufferRef sample = NULL;
                OSStatus status = CMVideoFormatDescriptionCreateForImageBuffer(NULL, buffer, &description);
                CMSampleTimingInfo timing = {CMTimeMake(next-pts, (int32_t)timescale), CMTimeMake(pts, (int32_t)timescale), kCMTimeInvalid};
                if (!status) status = CMSampleBufferCreateReadyWithImageBuffer(NULL, buffer, description, &timing, &sample);
                BOOL appended = !status && [input appendSampleBuffer:sample];
                if (sample) CFRelease(sample);
                if (description) CFRelease(description);
                CVPixelBufferRelease(buffer);
                require(appended, writer.error.localizedDescription ?: @"frame append failed");
                previous = pts;
                previousNext = next;
                ++index;
              }
              if (streaming) stream_reply([NSString stringWithFormat:@"{\"schema\":\"gpr.prores_stream.consumed.v1\",\"index\":%lu}", (unsigned long)index-1]);
            }
            if (streaming) require([stream_record()[@"event"] isEqual:@"finish"], @"stream requires explicit finish record");
            [writer endSessionAtSourceTime:CMTimeMake(end, (int32_t)timescale)];
            [input markAsFinished];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
            require(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 120*NSEC_PER_SEC)) == 0, @"writer finalization timed out");
            require(writer.status == AVAssetWriterStatusCompleted, writer.error.localizedDescription ?: @"writer failed");
            require(!link(staged.fileSystemRepresentation, argv[2]), @"cannot exclusively publish output");
            puts("{\"schema\":\"gpr.prores_encode.v1\",\"input_bits\":16,\"input_format\":\"64ARGB\",\"color_space\":\"ITU-R BT.709\"}");
        } @catch (NSException *error) {
            if (writer.status == AVAssetWriterStatusWriting) [writer cancelWriting];
            fprintf(stderr, "gpr_prores: %s\n", error.reason.UTF8String);
            return 1;
        } @finally {
            if (space) CGColorSpaceRelease(space);
            if (temporary) [[NSFileManager defaultManager] removeItemAtPath:temporary error:NULL];
        }
        return 0;
    }
}
