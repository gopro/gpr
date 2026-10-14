// GPRPipeline.m — orchestrates {DNG | GPR} → [codec] → CNN → demosaic → ProRes.
//
// 4-deep frame pipeline:
//   [reader_queue]    read+decode -> bayer plane (sem-gated cnn_inbox, N=2 free slots)
//   [cnn_queue]       CNN super-res         (sem-gated demosaic_inbox, N=2)
//   [demosaic_queue]  demosaic + ProRes pb  (sem-gated writer_inbox,   N=4)
//   [writer_queue]    AVAssetWriter append in PTS order
//
// Main thread submits all frame jobs, waits on a final "all done" semaphore.
//
// Stage-internal state (CNN's persistent MTLBuffers, Demosaic's _bayerBuf,
// the GPR codec's encoder context) is shared and not thread-safe — each
// stage's queue is serial, so only one frame is processed at a time within a
// stage. Inter-stage overlap is what gives us the throughput speedup.
//
// AVAssetWriter requires frames in PTS order. The writer queue maintains a
// frame-tag dictionary and writes the next sequential frame whenever its tag
// arrives, holding higher-tagged frames in memory until predecessors land.

#import "GPRPipeline.h"
#import "DNGReader.h"
#import "GPRFileReader.h"
#import "Demosaic.h"
#import "Downscale.h"
#import "CIRawDemosaic.h"
#import "ProResWriter.h"
#import "GPRCodec.h"
#import "CNNConfig.h"
#if GPR_HAVE_SUPER_RES_COREML
#import "SuperResCNN.h"
#endif
#if GPR_HAVE_SUPER_RES_METAL
#import "SuperResMetal.h"
#endif

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <mach/mach_time.h>
#import <stdatomic.h>

static double now_ms(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1.0e6;
}

static BOOL pathIsGPR(NSString *p) {
    return [[p.lowercaseString pathExtension] isEqualToString:@"gpr"];
}

// ----------------------------------------------------------------------------
// BayerIOSurfacePool — fixed-size pool of (CVPixelBuffer, MTLBuffer) pairs
// sharing one IOSurface per pair.
//
// Per pair: a 14Bayer<CFA> CVPixelBuffer of (sensorW × sensorH), and an
// MTLBuffer that wraps the same IOSurface's backing memory via
// `newBufferWithBytesNoCopy:`. Both views point at the same bytes, so the
// CNN's rebayer kernel can write the bayer plane directly into the memory
// CIRAWFilter will consume next — zero CPU memcpy across the boundary.
//
// Acquire/release are thread-safe (NSLock-guarded). The pool holds strong
// refs to entries indefinitely; -dealloc releases them all.
// ----------------------------------------------------------------------------
@interface BayerIOSurfaceEntry : NSObject
@property (nonatomic, assign) CVPixelBufferRef pb;     // retained
@property (nonatomic, strong) id<MTLBuffer> mtlBuf;
@property (nonatomic, assign) size_t strideBytes;
@property (nonatomic, assign) BOOL inUse;
@end
@implementation BayerIOSurfaceEntry
- (void)dealloc {
    if (_pb) { CVPixelBufferRelease(_pb); _pb = NULL; }
}
@end

@interface BayerIOSurfacePool : NSObject
@property (nonatomic, strong) NSMutableArray<BayerIOSurfaceEntry *> *entries;
@property (nonatomic, strong) NSLock *lock;
@property (nonatomic, strong) dispatch_semaphore_t slots;
@property (nonatomic, assign) uint32_t sensorW;
@property (nonatomic, assign) uint32_t sensorH;
@end

@implementation BayerIOSurfacePool

// Build N entries up front. Returns nil on any failure (IOSurface alloc,
// MTLBuffer wrap, alignment). Caller is expected to fall back to the
// CPU-copy path when nil.
+ (nullable instancetype)poolWithDevice:(id<MTLDevice>)device
                            sensorWidth:(uint32_t)sensorW
                           sensorHeight:(uint32_t)sensorH
                            bayerFormat:(OSType)bayerFormat
                                  count:(uint32_t)count
{
    BayerIOSurfacePool *pool = [[BayerIOSurfacePool alloc] init];
    if (!pool) return nil;
    pool.entries = [NSMutableArray arrayWithCapacity:count];
    pool.lock = [[NSLock alloc] init];
    pool.slots = dispatch_semaphore_create(count);
    pool.sensorW = sensorW;
    pool.sensorH = sensorH;

    NSDictionary *pixelAttrs = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(bayerFormat),
        (NSString *)kCVPixelBufferWidthKey: @(sensorW),
        (NSString *)kCVPixelBufferHeightKey: @(sensorH),
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        // Hint a tight 2-byte alignment so the per-row stride matches our
        // natural width*2 bytes. CoreVideo may still pad to >=64 bytes; the
        // rebayer kernel and CIRAWFilter both honor whatever stride we get.
        (NSString *)kCVPixelBufferBytesPerRowAlignmentKey: @(2),
    };

    for (uint32_t i = 0; i < count; i++) {
        CVPixelBufferRef pb = NULL;
        CVReturn r = CVPixelBufferCreate(NULL, sensorW, sensorH, bayerFormat,
                                          (__bridge CFDictionaryRef)pixelAttrs,
                                          &pb);
        if (r != kCVReturnSuccess || !pb) {
            fprintf(stderr, "BayerIOSurfacePool: CVPixelBufferCreate #%u failed (r=%d)\n", i, r);
            return nil;
        }
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pb);
        if (!surface) {
            fprintf(stderr, "BayerIOSurfacePool: pb #%u has no IOSurface\n", i);
            CVPixelBufferRelease(pb);
            return nil;
        }

        // Retain the surface so it outlives any transient unlock; CVPixelBuffer
        // already holds a ref but being explicit is harmless. Lock to query
        // the base address; the surface is otherwise unlocked while we hand
        // it to Metal/CIRAWFilter (both consume it via the IOSurface itself,
        // not via the CPU-locked address).
        IOSurfaceLock(surface, 0, NULL);
        void *baseAddress = IOSurfaceGetBaseAddress(surface);
        size_t bytesPerRow = IOSurfaceGetBytesPerRow(surface);
        size_t allocSize = IOSurfaceGetAllocSize(surface);
        IOSurfaceUnlock(surface, 0, NULL);

        if (!baseAddress) {
            fprintf(stderr, "BayerIOSurfacePool: IOSurface #%u baseAddress is NULL\n", i);
            CVPixelBufferRelease(pb);
            return nil;
        }
        // newBufferWithBytesNoCopy: requires the address to be page-aligned
        // (4 KB on Apple Silicon). IOSurface should give us page-aligned
        // backing, but verify.
        if (((uintptr_t)baseAddress & 0xFFFu) != 0) {
            fprintf(stderr, "BayerIOSurfacePool: IOSurface #%u baseAddress %p not 4K-aligned\n",
                    i, baseAddress);
            CVPixelBufferRelease(pb);
            return nil;
        }

        // Wrap the IOSurface backing in an MTLBuffer with no copy. Both views
        // (CVPixelBuffer + MTLBuffer) alias the same bytes from now on.
        // Deallocator nil: the IOSurface owns the memory.
        id<MTLBuffer> mtlBuf = [device newBufferWithBytesNoCopy:baseAddress
                                                         length:allocSize
                                                        options:MTLResourceStorageModeShared
                                                    deallocator:nil];
        if (!mtlBuf) {
            fprintf(stderr, "BayerIOSurfacePool: newBufferWithBytesNoCopy #%u failed\n", i);
            CVPixelBufferRelease(pb);
            return nil;
        }

        BayerIOSurfaceEntry *e = [[BayerIOSurfaceEntry alloc] init];
        e.pb = pb;            // entry takes ownership of the +1 ref
        e.mtlBuf = mtlBuf;
        e.strideBytes = bytesPerRow;
        e.inUse = NO;
        [pool.entries addObject:e];
    }

    fprintf(stderr, "BayerIOSurfacePool: %u entries, %ux%u, stride=%zu bytes (natural=%zu) -> %s\n",
            count, sensorW, sensorH,
            (size_t)pool.entries[0].strideBytes, (size_t)sensorW * 2,
            (pool.entries[0].strideBytes == (size_t)sensorW * 2)
                ? "tight" : "padded (kernel honors stride)");
    return pool;
}

// Block until an entry is free, then return it. Thread-safe.
- (BayerIOSurfaceEntry *)acquire {
    dispatch_semaphore_wait(self.slots, DISPATCH_TIME_FOREVER);
    [self.lock lock];
    BayerIOSurfaceEntry *picked = nil;
    for (BayerIOSurfaceEntry *e in self.entries) {
        if (!e.inUse) { e.inUse = YES; picked = e; break; }
    }
    [self.lock unlock];
    // Semaphore guarantees one is available; assert defensively.
    if (!picked) {
        fprintf(stderr, "BayerIOSurfacePool: BUG - slot signaled but no free entry\n");
    }
    return picked;
}

- (void)release:(BayerIOSurfaceEntry *)entry {
    if (!entry) return;
    [self.lock lock];
    entry.inUse = NO;
    [self.lock unlock];
    dispatch_semaphore_signal(self.slots);
}
@end

// Pick the kCVPixelFormatType_14Bayer_* CFA constant matching our enum.
// DNGInfo->cfaPattern: 0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR.
#if GPR_HAVE_SUPER_RES_METAL
static OSType bayerFormatForCFA_pipeline(uint32_t cfa) {
    switch (cfa) {
        case 0: return kCVPixelFormatType_14Bayer_RGGB;
        case 1: return kCVPixelFormatType_14Bayer_GBRG;
        case 2: return kCVPixelFormatType_14Bayer_GRBG;
        case 3: return kCVPixelFormatType_14Bayer_BGGR;
        default: return kCVPixelFormatType_14Bayer_RGGB;
    }
}
#endif

// Per-frame job moving through the pipeline. The bayer pointers are owned by
// the job and freed by whoever last touches them (the producer of the next
// buffer when it no longer needs the previous, or the writer at the end).
@interface FrameJob : NSObject
@property (nonatomic, assign) int idx;          // 0-based frame index (PTS order)
@property (nonatomic, copy)   NSString *path;   // for logging
@property (nonatomic, assign) double t_submit;
@property (nonatomic, assign) double t_read;
@property (nonatomic, assign) double t_decode;
@property (nonatomic, assign) double t_cnn;
@property (nonatomic, assign) double t_demosaic;
@property (nonatomic, assign) double t_write;
// Bayer buffer the next stage consumes. The size and meaning depends on the
// stage: after read+decode it's the decoded-bayer buffer (codec dims), after
// CNN it's native-dim bayer.
@property (nonatomic, assign) uint16_t *bayer;
@property (nonatomic, assign) uint32_t bayerW;
@property (nonatomic, assign) uint32_t bayerH;
@property (nonatomic, assign) BOOL bayerOwned;  // YES = free() on consume
// Zero-copy CNN→demosaic handoff: when set, the CNN wrote its output directly
// into this pool entry's IOSurface, and the demosaic stage will consume it
// via CIRawDemosaic's encodeFromBayerPixelBuffer:. Returned to the pool by
// the demosaic stage on completion.
@property (nonatomic, strong) BayerIOSurfaceEntry *bayerPoolEntry;
// Optional: a CVPixelBuffer the demosaic stage materialized. Retained from
// pool until appended.
@property (nonatomic, assign) CVPixelBufferRef pb;
@end

@implementation FrameJob
- (void)dealloc {
    if (_bayer && _bayerOwned) { free(_bayer); _bayer = NULL; }
    if (_pb) { CVPixelBufferRelease(_pb); _pb = NULL; }
    // If the demosaic stage was bypassed (error or abort) and we still hold a
    // pool entry, mark it free again so the pool doesn't leak slots. We can't
    // signal the semaphore from here without a pool ref; the pipeline's abort
    // paths handle slot signaling separately. Best-effort: just clear inUse so
    // a future acquire that wakes can pick it up. This is defensive — the
    // pipeline release paths should normally call -[pool release:] explicitly.
}
@end

// Simple FIFO inbox: serial dispatch_queue + dispatch_semaphore for backpressure.
// Producers wait on `slots` before posting; consumers `signal` slots when done.
@interface StageInbox : NSObject
@property (nonatomic, strong) dispatch_queue_t queue;
@property (nonatomic, strong) dispatch_semaphore_t slots;  // available slots
@end
@implementation StageInbox
@end

static StageInbox *make_inbox(const char *label, int capacity) {
    StageInbox *b = [[StageInbox alloc] init];
    b.queue = dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
    b.slots = dispatch_semaphore_create(capacity);
    return b;
}

@interface GPRPipeline () {
    NSString *_ckptPath;
    NSString *_outPath;
    int _fps;
    BOOL _aaOn;
    BOOL _noCNN;
    BOOL _noCodec;
    BOOL _timing;
    BOOL _gprInput;
    NSString *_cnnBackend;
    NSString *_demosaicMode;
    NSString *_outResolution;
    NSString *_lookMode;
    NSString *_metaDngPath;
    NSString *_cnnScale;        // "2x" (default) or "1x" (BIBO_1x — output @ codec dims)
    BOOL _cnnScale1x;           // YES if _cnnScale == "1x"

    DNGInfo _info;
    DNGInfo _codecInfo;
    DNGInfo _cnnInfo;

    // Output (ProRes) dims.
    uint32_t _outW;
    uint32_t _outH;
    // Demosaic intermediate dims (== full demosaic res before optional downscale).
    uint32_t _demoW;
    uint32_t _demoH;

    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    Demosaic *_demosaic;
    Downscale *_downscale;
    // Intermediate pool for the demosaic→downscale path. nil when downscale is
    // not needed (target == demo dims) and Demosaic writes straight into the
    // ProResWriter's pixel buffer.
    CVPixelBufferPoolRef _interPool;
    CIRawDemosaic *_ciDemosaic;
    ProResWriter *_writer;
    GPRCodec *_codec;
#if GPR_HAVE_SUPER_RES_COREML
    SuperResCNN *_cnn;
#endif
#if GPR_HAVE_SUPER_RES_METAL
    SuperResMetal *_cnnMetal;
#endif
    // Zero-copy bayer-share pool: (CVPixelBuffer, MTLBuffer) pairs shared
    // between SuperResMetal output and CIRawDemosaic input. nil when the
    // zero-copy path is unavailable (non-core-image demosaic, no CNN, no
    // SuperResMetal, or IOSurface/MTLBuffer setup failed) — in that case the
    // pipeline falls back to the CPU-copy code path.
    BayerIOSurfacePool *_bayerSharePool;

    // Reorder buffer for writer (frames may finish demosaic out of order).
    NSMutableDictionary<NSNumber *, FrameJob *> *_writePending;
    int _nextWriteIdx;
    NSLock *_writeLock;

    BOOL _skipErrors;
    atomic_int _errorCount;
    NSLock *_errAbortLock;
    BOOL _aborted;
}
@end

@implementation GPRPipeline

- (void)setSkipErrors:(BOOL)skip { _skipErrors = skip; }

// Record a failure. In skip-errors mode, just bumps the counter and logs.
// In strict mode, sets _aborted on first failure; running stages should
// short-circuit on _aborted.
- (void)reportError:(NSString *)msg frame:(int)idx {
    atomic_fetch_add(&_errorCount, 1);
    fprintf(stderr, "  frame %d ERROR: %s\n", idx, [msg UTF8String]);
    if (!_skipErrors) {
        [_errAbortLock lock];
        _aborted = YES;
        [_errAbortLock unlock];
    }
}

- (BOOL)isAborted {
    [_errAbortLock lock];
    BOOL a = _aborted;
    [_errAbortLock unlock];
    return a;
}

// Resolve the output dims for a preset string. Preserves source aspect from
// (demoW, demoH). Heights are rounded to even (ProRes prefers even dims).
static void resolveOutputDims(NSString *preset,
                              uint32_t demoW, uint32_t demoH,
                              uint32_t *outW, uint32_t *outH)
{
    NSString *p = [preset lowercaseString];
    uint32_t targetW = 0;
    if      ([p isEqualToString:@"2k"])  targetW = 2048;
    else if ([p isEqualToString:@"uhd"]) targetW = 3840;
    else if ([p isEqualToString:@"4k"])  targetW = 4096;
    else if ([p isEqualToString:@"6k"])  targetW = 6144;
    else /* 8k / native */               targetW = 0;

    if (targetW == 0 || targetW >= demoW) {
        // Native (no downscale).
        *outW = demoW;
        *outH = demoH;
        return;
    }
    // Preserve aspect.
    uint32_t targetH = (uint32_t)((uint64_t)demoH * targetW / demoW);
    // Round to even.
    targetH &= ~1u;
    if ((targetW & 1u) != 0) targetW &= ~1u;
    *outW = targetW;
    *outH = targetH;
}

- (nullable instancetype)initWithFirstFrame:(NSString *)firstFrame
                                metaDngPath:(nullable NSString *)metaDngPath
                                   ckptPath:(NSString *)ckptPath
                                    outPath:(NSString *)outPath
                                        fps:(int)fps
                                       aaOn:(BOOL)aaOn
                                      noCNN:(BOOL)noCNN
                                    noCodec:(BOOL)noCodec
                                     timing:(BOOL)timing
                                 cnnBackend:(NSString *)cnnBackend
                                demosaicMode:(NSString *)demosaicMode
                              outResolution:(NSString *)outResolution
                                   cnnScale:(NSString *)cnnScale
                                   lookMode:(nullable NSString *)lookMode
{
    self = [super init];
    if (!self) return nil;
    _ckptPath = ckptPath;
    _outPath = outPath;
    _fps = fps;
    _aaOn = aaOn;
    _noCNN = noCNN;
    _noCodec = noCodec;
    _timing = timing;
    _gprInput = pathIsGPR(firstFrame);
    _cnnBackend = cnnBackend ?: GPR_DEFAULT_CNN_BACKEND;
    if (!_noCNN && !GPRCNNBackendAvailable(_cnnBackend)) {
        fprintf(stderr, "error: CNN backend '%s' is unavailable in this build\n",
                [_cnnBackend UTF8String]);
        return nil;
    }
    _demosaicMode = demosaicMode ?: @"metal-bilinear";
    _outResolution = outResolution ?: @"8k";
    _lookMode = lookMode ?: @"none";
    _cnnScale = cnnScale ?: @"2x";
    _cnnScale1x = [[_cnnScale lowercaseString] isEqualToString:@"1x"];
    // For CIRAWFilter we need the source-DNG's color matrices. For .dng input
    // mode the first frame itself is the template; for .gpr it's metaDngPath.
    _metaDngPath = metaDngPath ? metaDngPath : (pathIsGPR(firstFrame) ? nil : firstFrame);

    if (_gprInput) {
        if (!metaDngPath) {
            fprintf(stderr, "GPRPipeline: GPR input requires --meta-dng <path-to-source-DNG>\n");
            return nil;
        }
        uint16_t *bayer = [DNGReader readBayerFromPath:metaDngPath info:&_info];
        if (!bayer) {
            fprintf(stderr, "GPRPipeline: cannot read metaDng %s\n", [metaDngPath UTF8String]);
            return nil;
        }
        free(bayer);

        GPRFileInfo gi = {0};
        if (![GPRFileReader readHeaderFromPath:firstFrame info:&gi]) {
            fprintf(stderr, "GPRPipeline: bad header in %s\n", [firstFrame UTF8String]);
            return nil;
        }
        _info.width  = gi.encWidth;
        _info.height = gi.encHeight;
        _noCodec = YES;
        _codecInfo = _info;
        _codecInfo.width  = gi.decWidth;
        _codecInfo.height = gi.decHeight;
        fprintf(stderr, "GPRPipeline: GPR input %ux%u → decoded %ux%u (decimate=%u, container=%s)\n",
                gi.encWidth, gi.encHeight, gi.decWidth, gi.decHeight, gi.decimate,
                gi.containerGPR ? "TIFF/GPR" : "FUSD");
    } else {
        uint16_t *bayer = [DNGReader readBayerFromPath:firstFrame info:&_info];
        if (!bayer) {
            fprintf(stderr, "GPRPipeline: cannot read first frame %s\n", [firstFrame UTF8String]);
            return nil;
        }
        free(bayer);
        _codecInfo = _info;
        _codecInfo.width  = _info.width  / 2;
        _codecInfo.height = _info.height / 2;
    }
    _cnnInfo = _info;

    uint32_t demoW = _info.width, demoH = _info.height;
    if (_gprInput) {
        if (_noCNN)         { demoW = _codecInfo.width; demoH = _codecInfo.height; }
        else if (_cnnScale1x) { demoW = _codecInfo.width; demoH = _codecInfo.height; }
        else                { demoW = _info.width;      demoH = _info.height; }
    } else if (_noCodec || !_noCNN) {
        if (_cnnScale1x && !_noCNN) {
            // 1× CNN output is at codec dims (or _info dims if --no-codec).
            demoW = _noCodec ? _info.width  : _codecInfo.width;
            demoH = _noCodec ? _info.height : _codecInfo.height;
        } else {
            demoW = _info.width;
            demoH = _info.height;
        }
    } else {
        demoW = _codecInfo.width;
        demoH = _codecInfo.height;
    }

    _device = MTLCreateSystemDefaultDevice();
    if (!_device) { fprintf(stderr, "no Metal device\n"); return nil; }
    _queue = [_device newCommandQueue];

    // Resolve target output dims from preset, preserving source aspect from
    // (demoW, demoH). 8k = native demoW×demoH (no scale).
    uint32_t outW = demoW, outH = demoH;
    resolveOutputDims(_outResolution, demoW, demoH, &outW, &outH);
    _demoW = demoW; _demoH = demoH;
    _outW = outW;   _outH = outH;
    BOOL needDownscale = (outW != demoW || outH != demoH);

    if ([_demosaicMode isEqualToString:@"core-image"]) {
        // CIRAWFilter handles demosaic + scale + crop in one CIContext render.
        // We just pass the target dims and let it do the work — no downscale
        // post-pass is needed for this backend.
        _ciDemosaic = [[CIRawDemosaic alloc] initWithDevice:_device
                                                sensorWidth:demoW
                                               sensorHeight:demoH
                                                   outWidth:outW
                                                  outHeight:outH
                                                       info:&_info
                                            templateDngPath:_metaDngPath
                                                   lookMode:_lookMode];
        if (!_ciDemosaic) { fprintf(stderr, "ciraw demosaic init failed\n"); return nil; }
    } else {
        _demosaic = [[Demosaic alloc] initWithDevice:_device
                                        width:demoW
                                       height:demoH
                                  cfaPattern:_info.cfaPattern
                                  blackLevel:_info.blackLevel
                                  whiteLevel:_info.whiteLevel
                                         wbR:_info.wbR
                                         wbG:_info.wbG
                                         wbB:_info.wbB
                                      rgbCam:_info.rgb_cam];
        if (!_demosaic) { fprintf(stderr, "demosaic init failed\n"); return nil; }

        // For the Metal-bilinear path, when target dims differ from demosaic
        // dims, we render into an intermediate BGRA8 CVPixelBuffer at demo dims
        // and run a separate downscale pass into the writer's target-dim pb.
        if (needDownscale) {
            _downscale = [[Downscale alloc] initWithDevice:_device
                                                   inWidth:demoW
                                                  inHeight:demoH
                                                  outWidth:outW
                                                 outHeight:outH];
            if (!_downscale) { fprintf(stderr, "downscale init failed\n"); return nil; }

            // Build a CVPixelBufferPool for the intermediate full-res BGRA8
            // image. Match the writer's IOSurface-backed Metal-compatible
            // attributes so the Demosaic kernel can write into it.
            NSDictionary *poolAttrs = @{};
            NSDictionary *pixelAttrs = @{
                (NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (NSString *)kCVPixelBufferWidthKey: @(demoW),
                (NSString *)kCVPixelBufferHeightKey: @(demoH),
                (NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
                (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
            };
            CVReturn r = CVPixelBufferPoolCreate(
                NULL,
                (__bridge CFDictionaryRef)poolAttrs,
                (__bridge CFDictionaryRef)pixelAttrs,
                &_interPool);
            if (r != kCVReturnSuccess || !_interPool) {
                fprintf(stderr, "GPRPipeline: intermediate pool create failed (r=%d)\n", r);
                return nil;
            }
        }
    }

    _writer = [[ProResWriter alloc] initWithPath:_outPath
                                            width:outW
                                           height:outH
                                              fps:_fps
                                           device:_device];
    if (!_writer) { fprintf(stderr, "writer init failed\n"); return nil; }

    if (!_noCodec) {
        setenv("GPR_INCLUDE_LL", "1", 1);
        setenv("GPR_ROW_DECIMATE", "2", 1);
        setenv("GPR_COL_DECIMATE", "2", 1);
        setenv("GPR_DROP_HIGHPASS", "1", 1);
        if (_aaOn) setenv("GPR_DECIMATE_AA", "1", 1);
        else       unsetenv("GPR_DECIMATE_AA");

        int pixel_format;
        if (_info.cfaPattern == 0) pixel_format = (_info.whiteLevel > 16383) ? 4 : 1;
        else                       pixel_format = (_info.whiteLevel > 16383) ? 5 : 3;
        _codec = [[GPRCodec alloc] initWithWidth:_info.width
                                          height:_info.height
                                    pixelFormat:pixel_format
                                        quality:3];
        if (!_codec) { fprintf(stderr, "codec init failed\n"); return nil; }
    } else if (_gprInput) {
        int pixel_format;
        if (_info.cfaPattern == 0) pixel_format = (_info.whiteLevel > 16383) ? 4 : 1;
        else                       pixel_format = (_info.whiteLevel > 16383) ? 5 : 3;
        _codec = [[GPRCodec alloc] initWithWidth:_info.width
                                          height:_info.height
                                    pixelFormat:pixel_format
                                        quality:3];
    }

    if (!_noCNN) {
        if ([_cnnBackend isEqualToString:@"mpsgraph"] || [_cnnBackend isEqualToString:@"metal"]) {
#if GPR_HAVE_SUPER_RES_METAL
            uint32_t inBayerW = _gprInput ? _codecInfo.width : _codecInfo.width;
            uint32_t inBayerH = _gprInput ? _codecInfo.height : _codecInfo.height;
            if (_noCodec && !_gprInput) { inBayerW = _info.width; inBayerH = _info.height; }
            uint32_t Wp = inBayerW / 2;
            uint32_t Hp = inBayerH / 2;
            uint32_t Wpp = (Wp + 7) & ~7u;
            uint32_t Hpp = (Hp + 7) & ~7u;
            SuperResMetalBackend be = [_cnnBackend isEqualToString:@"metal"]
                ? SuperResMetalBackendHybrid : SuperResMetalBackendMPSGraph;
            BOOL useSubpixel = !_cnnScale1x;
            _cnnMetal = [[SuperResMetal alloc] initWithWeightsDir:_ckptPath
                                                            device:_device
                                                          inPlaneH:Hpp
                                                          inPlaneW:Wpp
                                                            backend:be
                                                   useSubpixelHead:useSubpixel];
            if (!_cnnMetal) { fprintf(stderr, "CNN(metal) init failed\n"); return nil; }
#endif
        } else {
#if GPR_HAVE_SUPER_RES_COREML
            _cnn = [[SuperResCNN alloc] initWithMLPackagePath:_ckptPath device:_device];
            if (!_cnn) { fprintf(stderr, "CNN init failed\n"); return nil; }
#endif
        }
    }

    // -- IOSurface zero-copy pool between SuperResMetal output and CIRawDemosaic
    //    input. Requires: core-image demosaic AND SuperResMetal CNN AND CNN on.
    //    Pool dims = (demoW, demoH) == CIRawDemosaic's sensorW × sensorH ==
    //    CNN output dims. Sized to 5 entries (one per in-flight frame in the
    //    4-deep pipeline plus a hot-spare slot).
#if GPR_HAVE_SUPER_RES_METAL
    if (_ciDemosaic && _cnnMetal && !_noCNN) {
        OSType bayerFormat = bayerFormatForCFA_pipeline(_info.cfaPattern);
        _bayerSharePool = [BayerIOSurfacePool poolWithDevice:_device
                                                  sensorWidth:demoW
                                                 sensorHeight:demoH
                                                  bayerFormat:bayerFormat
                                                        count:5];
        if (!_bayerSharePool) {
            fprintf(stderr, "GPRPipeline: BayerIOSurfacePool init failed — "
                            "falling back to CPU memcpy path\n");
        }
    }

#endif

    _writePending = [NSMutableDictionary dictionary];
    _nextWriteIdx = 0;
    _writeLock = [[NSLock alloc] init];
    _errAbortLock = [[NSLock alloc] init];
    atomic_init(&_errorCount, 0);
    _aborted = NO;
    _skipErrors = NO;

    fprintf(stderr, "  pipeline: %s%ux%u → ",
            _gprInput ? "GPR " : "",
            _info.width, _info.height);
    if (_gprInput)        fprintf(stderr, "decode→%ux%u → ", _codecInfo.width, _codecInfo.height);
    else if (!_noCodec)   fprintf(stderr, "codec→%ux%u → ", _codecInfo.width, _codecInfo.height);
    if (!_noCNN) {
        uint32_t cnnW = _cnnScale1x ? _codecInfo.width  : _info.width;
        uint32_t cnnH = _cnnScale1x ? _codecInfo.height : _info.height;
        fprintf(stderr, "CNN[%s]→%ux%u → ",
                _cnnScale1x ? "1x" : "2x", cnnW, cnnH);
    }
    if (_downscale)
        fprintf(stderr, "demosaic[%s]→%ux%u → downscale→ProRes(%ux%u@%dfps) [%s]\n",
                [_demosaicMode UTF8String], _demoW, _demoH, outW, outH, _fps,
                "pipelined 4-deep");
    else
        fprintf(stderr, "demosaic[%s]→ProRes(%ux%u@%dfps) [pipelined 4-deep]\n",
                [_demosaicMode UTF8String], outW, outH, _fps);
    return self;
}

- (void)dealloc {
    if (_interPool) { CVPixelBufferPoolRelease(_interPool); _interPool = NULL; }
}

// ============================================================================
// Pipelined stage implementations (shared by GPR + DNG runners)
// ============================================================================

// CNN stage: run the CNN on job.bayer (decoded codec dims) and replace
// job.bayer with the CNN output Bayer.
//   - 2× super-res (cnnScale=2x, default): output at native (_info) dims.
//   - 1× clean-bayer (cnnScale=1x, BIBO_1x): output at the same dims as input.
- (int)runCNNStage:(FrameJob *)job {
    if (_noCNN) return 0;
#if GPR_HAVE_CNN
    // CNN output dims depend on mode.
    uint32_t cnnOutW = _cnnScale1x ? job.bayerW : _info.width;
    uint32_t cnnOutH = _cnnScale1x ? job.bayerH : _info.height;

    // Zero-copy path: when we have an IOSurface share pool, acquire a pool
    // entry and write the CNN output directly into its MTLBuffer (which
    // aliases the IOSurface CIRAWFilter will consume next). No CPU buffer
    // for the CNN output, no parallel memcpy out, no per-row memcpy on the
    // demosaic side.
#if GPR_HAVE_SUPER_RES_METAL
    if (_bayerSharePool && _cnnMetal) {
        BayerIOSurfaceEntry *entry = [_bayerSharePool acquire];
        if (!entry) {
            fprintf(stderr, "CNN: pool acquire returned nil (frame %d)\n", job.idx);
            return -1;
        }
        int crc = [_cnnMetal runOnBayer:job.bayer
                                  width:job.bayerW height:job.bayerH
                           outMTLBuffer:entry.mtlBuf
                         outStrideBytes:entry.strideBytes
                               outWidth:cnnOutW
                              outHeight:cnnOutH
                             blackLevel:_info.blackLevel
                             whiteLevel:_info.whiteLevel];
        if (crc != 0) {
            fprintf(stderr, "CNN rc=%d (frame %d)\n", crc, job.idx);
            [_bayerSharePool release:entry];
            return crc;
        }
        // Drop the input bayer — CNN consumed it.
        if (job.bayerOwned && job.bayer) free(job.bayer);
        job.bayer = NULL;
        job.bayerOwned = NO;
        job.bayerW = cnnOutW;
        job.bayerH = cnnOutH;
        job.bayerPoolEntry = entry;
        return 0;
    }

#endif

    // CPU-copy fallback path (no IOSurface pool — metal-bilinear demosaic,
    // CoreML CNN, etc.).
    uint16_t *cnnBuf = malloc((size_t)cnnOutW * cnnOutH * 2);
    int crc = -1;
#if GPR_HAVE_SUPER_RES_METAL
    if (_cnnMetal) {
        crc = [_cnnMetal runOnBayer:job.bayer
                              width:job.bayerW height:job.bayerH
                           outBayer:cnnBuf
                           outWidth:cnnOutW
                          outHeight:cnnOutH
                         blackLevel:_info.blackLevel
                         whiteLevel:_info.whiteLevel];
    }
#endif
#if GPR_HAVE_SUPER_RES_COREML
    if (_cnn) {
        crc = [_cnn runOnBayer:job.bayer
                         width:job.bayerW height:job.bayerH
                      outBayer:cnnBuf
                      outWidth:cnnOutW
                     outHeight:cnnOutH
                    blackLevel:_info.blackLevel
                    whiteLevel:_info.whiteLevel];
    }
#endif
    if (crc != 0) {
        fprintf(stderr, "CNN rc=%d (frame %d)\n", crc, job.idx);
        free(cnnBuf);
        return crc;
    }
    if (job.bayerOwned && job.bayer) free(job.bayer);
    job.bayer = cnnBuf;
    job.bayerOwned = YES;
    job.bayerW = cnnOutW;
    job.bayerH = cnnOutH;
    return 0;
#else
    return -1;
#endif
}

// Demosaic + grab CVPixelBuffer. Encodes one MTLCommandBuffer; this method
// blocks until completion (so the next stage can safely append to ordered
// writer inbox with the bayer freed).
- (int)runDemosaicStage:(FrameJob *)job {
    CVPixelBufferRef pb = [_writer pixelBuffer];
    if (!pb) {
        fprintf(stderr, "no pixel buffer (frame %d)\n", job.idx);
        return -1;
    }
    // Retain across the inbox handoff (writer queue releases).
    CVPixelBufferRetain(pb);
    if (_ciDemosaic && job.bayerPoolEntry) {
        // Zero-copy path: the CNN already wrote the bayer directly into this
        // pool entry's IOSurface. Skip the per-row memcpy entirely.
        [_ciDemosaic encodeFromBayerPixelBuffer:job.bayerPoolEntry.pb
                                  outPixelBuffer:pb];
        // Release the pool entry now that CIRAWFilter has drained it.
        // CIContext.render is synchronous on macOS so we know the IOSurface
        // has been consumed before this returns.
        [_bayerSharePool release:job.bayerPoolEntry];
        job.bayerPoolEntry = nil;
    } else if (_ciDemosaic) {
        // CIRAWFilter path: CIContext drives its own Metal submissions;
        // -encode: returns once submission is in flight, but we still need
        // a hard sync so the writer sees a complete frame. CIContext.render
        // is synchronous on macOS, so no extra wait is needed.
        [_ciDemosaic encode:nil
                      bayer:job.bayer
                      width:job.bayerW
                     height:job.bayerH
             outPixelBuffer:pb];
    } else if (_downscale && _interPool) {
        // Two-stage Metal path: demosaic to full-res intermediate, then
        // downscale into the writer's target-dim pb.
        CVPixelBufferRef inter = NULL;
        CVReturn r = CVPixelBufferPoolCreatePixelBuffer(NULL, _interPool, &inter);
        if (r != kCVReturnSuccess || !inter) {
            fprintf(stderr, "intermediate pool alloc r=%d (frame %d)\n", r, job.idx);
            CVPixelBufferRelease(pb);
            return -1;
        }
        id<MTLCommandBuffer> cb = [_queue commandBuffer];
        [_demosaic encode:cb bayer:job.bayer width:job.bayerW height:job.bayerH outPixelBuffer:inter];
        [_downscale encode:cb inPixelBuffer:inter outPixelBuffer:pb];
        [cb commit];
        [cb waitUntilCompleted];
        CVPixelBufferRelease(inter);
    } else {
        // Single-pass Metal demosaic straight into the writer's pb (target ==
        // demo dims, no downscale needed).
        id<MTLCommandBuffer> cb = [_queue commandBuffer];
        [_demosaic encode:cb bayer:job.bayer width:job.bayerW height:job.bayerH outPixelBuffer:pb];
        [cb commit];
        [cb waitUntilCompleted];
    }

    // Bayer is no longer needed.
    if (job.bayerOwned && job.bayer) { free(job.bayer); job.bayer = NULL; }
    job.bayer = NULL;
    job.bayerOwned = NO;
    job.pb = pb;
    return 0;
}

// Writer stage: insert into reorder buffer, drain in PTS order.
- (void)deliverToWriter:(FrameJob *)job {
    NSMutableArray<FrameJob *> *flushable = [NSMutableArray array];
    [_writeLock lock];
    _writePending[@(job.idx)] = job;
    while (1) {
        FrameJob *next = _writePending[@(_nextWriteIdx)];
        if (!next) break;
        [_writePending removeObjectForKey:@(_nextWriteIdx)];
        [flushable addObject:next];
        _nextWriteIdx++;
    }
    [_writeLock unlock];

    for (FrameJob *j in flushable) {
        double tw0 = now_ms();
        int wrc = [_writer appendPixelBuffer:j.pb frameIndex:j.idx];
        if (wrc != 0) fprintf(stderr, "append rc=%d (frame %d)\n", wrc, j.idx);
        CVPixelBufferRelease(j.pb);
        j.pb = NULL;
        double tw1 = now_ms();
        j.t_write = tw1 - tw0;

        if (self->_timing) {
            fprintf(stderr,
                "  frame %d  read=%.1fms decode=%.1fms cnn=%.1fms demosaic=%.1fms write=%.1fms total=%.1fms\n",
                j.idx, j.t_read, j.t_decode, j.t_cnn, j.t_demosaic, j.t_write,
                (now_ms() - j.t_submit));
        } else {
            fprintf(stderr, "  frame %d  total=%.1fms\n", j.idx, now_ms() - j.t_submit);
        }
    }
}

// ============================================================================
// GPR runner — pipelined.
// ============================================================================
- (int)runFramesGPR:(NSArray<NSString *> *)framePaths {
    double t_total_start = now_ms();
    NSUInteger N = framePaths.count;

    // Inbox capacities tuned for ~4 frames in flight total. Reader can stay
    // 2 ahead of CNN; CNN 2 ahead of demosaic; demosaic 4 ahead of writer.
    // GPR2PRORES_CNN_INBOX / GPR2PRORES_DEMOSAIC_INBOX env vars allow tuning
    // for perf measurement.
    int cnn_cap = _noCNN ? 4 : 2;
    int demo_cap = 2;
    int write_cap = 4;
    {
        const char *e;
        if ((e = getenv("GPR2PRORES_CNN_INBOX"))      && atoi(e) > 0) cnn_cap   = atoi(e);
        if ((e = getenv("GPR2PRORES_DEMOSAIC_INBOX")) && atoi(e) > 0) demo_cap  = atoi(e);
        if ((e = getenv("GPR2PRORES_WRITER_INBOX"))   && atoi(e) > 0) write_cap = atoi(e);
    }
    StageInbox *cnnInbox      = make_inbox("gpr2prores.cnn",      cnn_cap);
    StageInbox *demosaicInbox = make_inbox("gpr2prores.demosaic", demo_cap);
    StageInbox *writerInbox   = make_inbox("gpr2prores.writer",   write_cap);

    // Reader is its own queue (concurrent reads if filesystem allows; serial
    // is fine since mmap+decode is fast). Keep serial so the GPRCodec decode
    // context is touched by one thread at a time (decode is stateless in C
    // but stays simple).
    dispatch_queue_t readerQueue = dispatch_queue_create("gpr2prores.reader", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t allDone = dispatch_semaphore_create(0);
    __block atomic_int writtenCount = 0;
    int totalN = (int)N;

    for (NSUInteger i = 0; i < N; i++) {
        NSString *path = framePaths[i];
        FrameJob *job = [[FrameJob alloc] init];
        job.idx = (int)i;
        job.path = path;
        job.t_submit = now_ms();

        // Reader stage: acquire a slot on cnnInbox first (backpressure: don't
        // get too far ahead of the CNN).
        dispatch_semaphore_wait(cnnInbox.slots, DISPATCH_TIME_FOREVER);
        dispatch_async(readerQueue, ^{
            if ([self isAborted]) {
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }
            double t0 = now_ms();
            GPRFileInfo gi = {0};
            NSData *encData = [GPRFileReader readEncodedFromPath:path info:&gi];
            if (encData && encData.length > 0) {
                volatile uint8_t v = ((const uint8_t *)encData.bytes)[0]; (void)v;
            }
            double t1 = now_ms();
            job.t_read = t1 - t0;
            if (!encData) {
                [self reportError:[NSString stringWithFormat:@"read fail: %@", path] frame:job.idx];
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }
            // Header sanity (truncated mid-decode? bad fields?).
            if (gi.decWidth == 0 || gi.decHeight == 0 || gi.decWidth > 16384 || gi.decHeight > 16384) {
                [self reportError:[NSString stringWithFormat:@"bad header (decW=%u decH=%u) in %@",
                                   gi.decWidth, gi.decHeight, path] frame:job.idx];
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }

            // Decode in place on a per-job-allocated buffer.
            size_t dec_buf_sz = (size_t)gi.decWidth * gi.decHeight * 2;
            uint16_t *decBuf = malloc(dec_buf_sz);
            if (!decBuf) {
                [self reportError:[NSString stringWithFormat:@"malloc %zu failed", dec_buf_sz] frame:job.idx];
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }
            int dw = (int)gi.decWidth, dh = (int)gi.decHeight;
            int drc = gi.containerGPR
                ? [self->_codec decodeGPRContainer:encData.bytes size:encData.length
                                          outBayer:decBuf
                                          outPitch:(size_t)gi.decWidth * 2
                                          outWidth:&dw outHeight:&dh]
                : [self->_codec decode:encData.bytes size:encData.length
                              outBayer:decBuf
                              outPitch:(size_t)gi.decWidth * 2
                              outWidth:&dw outHeight:&dh];
            double t2 = now_ms();
            job.t_decode = t2 - t1;
            if (drc != 0) {
                [self reportError:[NSString stringWithFormat:@"codec decode rc=%d (possibly truncated)", drc] frame:job.idx];
                free(decBuf);
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }
            job.bayer = decBuf;
            job.bayerW = (uint32_t)dw;
            job.bayerH = (uint32_t)dh;
            job.bayerOwned = YES;

            // Hand to CNN queue. Reserve a demosaic slot before letting CNN
            // start, so CNN won't get more than `demosaicInbox.cap` ahead.
            dispatch_semaphore_wait(demosaicInbox.slots, DISPATCH_TIME_FOREVER);
            dispatch_async(cnnInbox.queue, ^{
                // Reader had reserved the cnn slot; release it now that we're
                // in the cnn stage (the slot tracks "frames awaiting CNN start").
                dispatch_semaphore_signal(cnnInbox.slots);

                if ([self isAborted]) {
                    dispatch_semaphore_signal(demosaicInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }
                double tc0 = now_ms();
                int rc = [self runCNNStage:job];
                double tc1 = now_ms();
                job.t_cnn = tc1 - tc0;
                if (rc != 0) {
                    [self reportError:[NSString stringWithFormat:@"CNN rc=%d", rc] frame:job.idx];
                    dispatch_semaphore_signal(demosaicInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }

                // Hand to demosaic queue. Reserve a writer slot.
                dispatch_semaphore_wait(writerInbox.slots, DISPATCH_TIME_FOREVER);
                dispatch_async(demosaicInbox.queue, ^{
                    dispatch_semaphore_signal(demosaicInbox.slots);

                    if ([self isAborted]) {
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                        return;
                    }
                    double td0 = now_ms();
                    int rc2 = [self runDemosaicStage:job];
                    double td1 = now_ms();
                    job.t_demosaic = td1 - td0;
                    if (rc2 != 0) {
                        [self reportError:[NSString stringWithFormat:@"demosaic rc=%d", rc2] frame:job.idx];
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                        return;
                    }

                    // Hand to writer queue.
                    dispatch_async(writerInbox.queue, ^{
                        [self deliverToWriter:job];
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                    });
                });
            });
        });
    }

    // Wait for all frames to write.
    dispatch_semaphore_wait(allDone, DISPATCH_TIME_FOREVER);

    [_writer finish];
    double t_total = now_ms() - t_total_start;
    int errs = atomic_load(&_errorCount);
    fprintf(stderr, "DONE  total=%.1fs  output=%s  effective fps=%.2f  errors=%d\n",
            t_total / 1000.0, [_outPath UTF8String],
            framePaths.count / (t_total / 1000.0), errs);
    if (errs > 0 && !_skipErrors) return 2;
    if (errs > 0 && _skipErrors) {
        fprintf(stderr, "  (continued past %d failed frames; use without --skip-errors for strict mode)\n", errs);
    }
    return 0;
}

// ============================================================================
// DNG runner — pipelined. Same shape as GPR but reader does DNG read + encode
// + decode (when --no-codec is off). The GPRCodec's encoder is stateful so
// the encode step must remain serial within the reader queue (which it is).
// ============================================================================
- (int)runFramesDNG:(NSArray<NSString *> *)framePaths {
    double t_total_start = now_ms();
    NSUInteger N = framePaths.count;

    StageInbox *cnnInbox      = make_inbox("gpr2prores.cnn",      _noCNN ? 4 : 2);
    StageInbox *demosaicInbox = make_inbox("gpr2prores.demosaic", 2);
    StageInbox *writerInbox   = make_inbox("gpr2prores.writer",   4);
    dispatch_queue_t readerQueue = dispatch_queue_create("gpr2prores.reader", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t allDone = dispatch_semaphore_create(0);
    __block atomic_int writtenCount = 0;
    int totalN = (int)N;

    for (NSUInteger i = 0; i < N; i++) {
        NSString *path = framePaths[i];
        FrameJob *job = [[FrameJob alloc] init];
        job.idx = (int)i;
        job.path = path;
        job.t_submit = now_ms();

        dispatch_semaphore_wait(cnnInbox.slots, DISPATCH_TIME_FOREVER);
        dispatch_async(readerQueue, ^{
            if ([self isAborted]) {
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }
            double t0 = now_ms();
            DNGInfo info_local;
            uint16_t *bayer = [DNGReader readBayerFromPath:path info:&info_local];
            double t1 = now_ms();
            job.t_read = t1 - t0;
            if (!bayer) {
                [self reportError:[NSString stringWithFormat:@"DNG read fail: %@", path] frame:job.idx];
                dispatch_semaphore_signal(cnnInbox.slots);
                if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                    dispatch_semaphore_signal(allDone);
                }
                return;
            }

            uint16_t *currentBayer = bayer;
            BOOL ownsCurrent = YES;
            uint32_t currentW = info_local.width;
            uint32_t currentH = info_local.height;

            if (!self->_noCodec) {
                uint8_t *vc5 = NULL; size_t vc5_size = 0;
                int erc = [self->_codec encodeRawBayer:currentBayer
                                                bytes:(size_t)currentW * currentH * 2
                                              vc5Out:&vc5
                                             vc5Size:&vc5_size];
                if (erc != 0) {
                    [self reportError:[NSString stringWithFormat:@"codec encode rc=%d", erc] frame:job.idx];
                    free(bayer);
                    dispatch_semaphore_signal(cnnInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }
                uint16_t *codecBuf = malloc((size_t)self->_codecInfo.width * self->_codecInfo.height * 2);
                if (!codecBuf) {
                    [self reportError:@"codecBuf malloc failed" frame:job.idx];
                    free(bayer);
                    dispatch_semaphore_signal(cnnInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }
                int dw = 0, dh = 0;
                int drc = [self->_codec decode:vc5 size:vc5_size
                                       outBayer:codecBuf
                                      outPitch:(size_t)self->_codecInfo.width * 2
                                       outWidth:&dw outHeight:&dh];
                if (drc != 0) {
                    [self reportError:[NSString stringWithFormat:@"codec decode rc=%d", drc] frame:job.idx];
                    free(codecBuf);
                    free(bayer);
                    dispatch_semaphore_signal(cnnInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }
                // Replace currentBayer (free original)
                if (ownsCurrent) free(currentBayer);
                currentBayer = codecBuf;
                currentW = (uint32_t)dw;
                currentH = (uint32_t)dh;
                ownsCurrent = YES;
            }
            double t2 = now_ms();
            job.t_decode = t2 - t1;

            job.bayer = currentBayer;
            job.bayerW = currentW;
            job.bayerH = currentH;
            job.bayerOwned = ownsCurrent;

            dispatch_semaphore_wait(demosaicInbox.slots, DISPATCH_TIME_FOREVER);
            dispatch_async(cnnInbox.queue, ^{
                dispatch_semaphore_signal(cnnInbox.slots);
                if ([self isAborted]) {
                    dispatch_semaphore_signal(demosaicInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }
                double tc0 = now_ms();
                int rc = [self runCNNStage:job];
                double tc1 = now_ms();
                job.t_cnn = tc1 - tc0;
                if (rc != 0) {
                    [self reportError:[NSString stringWithFormat:@"CNN rc=%d", rc] frame:job.idx];
                    dispatch_semaphore_signal(demosaicInbox.slots);
                    if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                        dispatch_semaphore_signal(allDone);
                    }
                    return;
                }

                dispatch_semaphore_wait(writerInbox.slots, DISPATCH_TIME_FOREVER);
                dispatch_async(demosaicInbox.queue, ^{
                    dispatch_semaphore_signal(demosaicInbox.slots);
                    if ([self isAborted]) {
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                        return;
                    }
                    double td0 = now_ms();
                    int rc2 = [self runDemosaicStage:job];
                    double td1 = now_ms();
                    job.t_demosaic = td1 - td0;
                    if (rc2 != 0) {
                        [self reportError:[NSString stringWithFormat:@"demosaic rc=%d", rc2] frame:job.idx];
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                        return;
                    }

                    dispatch_async(writerInbox.queue, ^{
                        [self deliverToWriter:job];
                        dispatch_semaphore_signal(writerInbox.slots);
                        if (atomic_fetch_add(&writtenCount, 1) + 1 == totalN) {
                            dispatch_semaphore_signal(allDone);
                        }
                    });
                });
            });
        });
    }

    dispatch_semaphore_wait(allDone, DISPATCH_TIME_FOREVER);

    [_writer finish];
    double t_total = now_ms() - t_total_start;
    int errs = atomic_load(&_errorCount);
    fprintf(stderr, "DONE  total=%.1fs  output=%s  effective fps=%.2f  errors=%d\n",
            t_total / 1000.0, [_outPath UTF8String],
            framePaths.count / (t_total / 1000.0), errs);
    if (errs > 0 && !_skipErrors) return 2;
    if (errs > 0 && _skipErrors) {
        fprintf(stderr, "  (continued past %d failed frames; use without --skip-errors for strict mode)\n", errs);
    }
    return 0;
}

- (int)runFrames:(NSArray<NSString *> *)framePaths {
    return _gprInput ? [self runFramesGPR:framePaths]
                     : [self runFramesDNG:framePaths];
}

@end
