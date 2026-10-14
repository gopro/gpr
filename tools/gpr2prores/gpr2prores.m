// gpr2prores.m — top-level orchestrator.
//
// Two input modes:
//   1. Playback (.gpr files):  reads pre-encoded GPR bitstreams, decodes,
//      runs CNN + demosaic, writes ProRes. This is the production workflow.
//      Requires --meta-dng <DNG> for color/wb/black/white metadata.
//   2. End-to-end (.dng files): reads DNG, encodes + decodes through the
//      codec, runs CNN + demosaic, writes ProRes. Kept for benchmarking and
//      acquisition-time validation.
//
// Dispatch is automatic based on first input's extension.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <limits.h>
#import <sys/statvfs.h>
#include <string.h>
#import "DNGReader.h"
#import "GPRPipeline.h"
#import "CNNConfig.h"

#define GPR2PRORES_VERSION "1.0.0"

static BOOL hasMOVSignature(NSString *path) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (!file) return NO;
    unsigned char header[8];
    size_t count = fread(header, 1, sizeof(header), file);
    fclose(file);
    // Recognize archived MOV wrappers without depending on their old suffixes.
    return count == sizeof(header) && memcmp(header + 4, "ftyp", 4) == 0;
}

static void print_version(void) {
    fprintf(stdout, "gpr2prores %s\n", GPR2PRORES_VERSION);
}

static void print_usage(FILE *out) {
    fprintf(out,
        "gpr2prores %s — decode GPR/DNG raw video and write a ProRes 4444 MOV.\n"
        "\n"
        "USAGE:\n"
        "  gpr2prores [options] INPUT OUTPUT.mov\n"
        "  gpr2prores --help | --version\n"
        "\n"
        "INPUT:\n"
        "  .gpr file or directory of .gpr files (playback mode)\n"
        "  .dng file or directory of .dng files (encode+playback)\n"
        "  .gvid container (neutral raw-video stream; auto-unpacked)\n"
        "  .mov container (GPRr/GPR1-tagged MOV; auto-unpacked)\n"
        "\n"
        "REQUIRED OPTIONS:\n"
        "  --meta-dng PATH     DNG to source color/wb/black/white metadata from\n"
        "                      (required for .gpr input; auto-discovered as a sibling\n"
        "                      .dng file if omitted)\n"
        "\n"
        "OPTIONS:\n"
        "  --max-frames N      process at most N frames\n"
        "  --fps N             output framerate (default 24)\n"
        "  --ckpt PATH         super-res mlpackage or weights dir\n"
        "                      (default $GPR_MODEL_ROOT/super_res.mlpackage)\n"
        "  --aa {on,off}       GPR codec AA filter, DNG mode only (default on)\n"
        "  --no-cnn            skip CNN (decode + demosaic + ProRes only)\n"
        "  --no-codec          skip codec, DNG mode only (direct demosaic + ProRes)\n"
        "  --cnn-backend B     coreml, mpsgraph, or metal (must be built in)\n"
        "                      mpsgraph/metal use --ckpt as a directory of fp16\n"
        "                      .bin weights (see extract_F_weights.py)\n"
        "  --cnn-scale S       2x (default — F super-res CNN, 2× upscale) or\n"
        "                      1x (F_no_sr / BIBO_1x — clean Bayer at codec dims;\n"
        "                          CIRAWFilter does the upscale to --out-resolution)\n"
        "  --demosaic M        metal-bilinear (default) or core-image\n"
        "                      core-image routes through CIRAWFilter\n"
        "  --mission-look      experimental Mission 1 JPEG-look controls for\n"
        "                      core-image demosaic; env overrides:\n"
        "                      GPR_MISSION_LOOK_EXPOSURE, _BOOST, _BOOST_SHADOW,\n"
        "                      _LOCAL_TONE, _SHADOW_BIAS, _CROP_SCALE\n"
        "  --out-resolution R  Output resolution. One of: 2k,uhd,4k,6k,8k (default 8k).\n"
        "                      Width is fixed; height is rounded from source aspect ratio:\n"
        "                        2k=2048  uhd=3840  4k=4096  6k=6144  8k=native (no scale)\n"
        "  --skip-errors       skip frames that fail to decode and continue (errors\n"
        "                      logged to stderr); without this, first error aborts run\n"
        "  --gvid-dispatch P   validate gvid_runtime_dispatch.v1 plan for .gvid playback\n"
        "  --timing            print per-stage timing per frame\n"
        "  --phase0            phase 0: dump input info and exit\n"
        "  --help              show this message\n"
        "  --version           print version and exit\n"
        "\n"
        "EXAMPLES:\n"
        "  # Playback a GPR directory to ProRes 4444\n"
        "  gpr2prores --meta-dng src.dng /clip/gpr/ $TMPDIR/out.mov\n"
        "\n"
        "  # Acquisition-time validation: encode+decode roundtrip on DNG\n"
        "  gpr2prores --aa on /clip/dng/ $TMPDIR/out.mov\n"
        "\n"
        "  # Run on neutral GVID container (auto-unpacks)\n"
        "  gpr2prores --meta-dng src.dng clip.gvid $TMPDIR/out.mov\n"
        "\n"
        "  # Run on a GVID MOV wrapper (auto-unpacks)\n"
        "  gpr2prores --meta-dng src.dng clip.mov $TMPDIR/out.mov\n",
        GPR2PRORES_VERSION);
    fprintf(out, "\nCNN build: coreml=%s, metal/mpsgraph=%s; default=%s\n",
            GPR_HAVE_SUPER_RES_COREML ? "yes" : "no",
            GPR_HAVE_SUPER_RES_METAL ? "yes" : "no",
            [GPR_DEFAULT_CNN_BACKEND UTF8String]);
    if (!GPR_HAVE_CNN)
        fprintf(out, "CNN restoration is disabled; --no-cnn is implicit.\n");
}

static BOOL pathHasExt(NSString *p, NSString *ext) {
    return [[p.lowercaseString pathExtension] isEqualToString:ext];
}

// Look for a sibling .dng file in the same directory as `path`.
// Used to auto-fill --meta-dng when the user forgets but a companion exists.
static NSString *findSiblingDNG(NSString *path) {
    NSString *dir = [path stringByDeletingLastPathComponent];
    if (dir.length == 0) dir = @".";
    NSArray *kids = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
    for (NSString *f in [kids sortedArrayUsingSelector:@selector(compare:)]) {
        if ([f.lowercaseString hasSuffix:@".dng"]) {
            return [dir stringByAppendingPathComponent:f];
        }
    }
    return nil;
}

// Returns free-space bytes on the filesystem containing `path` (or its parent
// directory if `path` doesn't exist yet). Returns -1 on failure.
static int64_t freeBytesForPath(NSString *path) {
    NSString *probe = path;
    if (![[NSFileManager defaultManager] fileExistsAtPath:probe]) {
        probe = [path stringByDeletingLastPathComponent];
        if (probe.length == 0) probe = @".";
    }
    struct statvfs s;
    if (statvfs([probe UTF8String], &s) != 0) return -1;
    return (int64_t)s.f_bavail * (int64_t)s.f_frsize;
}

static uint32_t le32_gpr2prores(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t le64_gpr2prores(const uint8_t *p) {
    return  (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static NSString *makeTempDir(NSString *prefix) {
    const char *tmpEnv = getenv("TMPDIR");
    NSString *base = (tmpEnv && tmpEnv[0])
        ? [NSString stringWithUTF8String:tmpEnv]
        : NSTemporaryDirectory();
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "%s/%s_XXXXXX", [base fileSystemRepresentation], [prefix UTF8String]);
    char *resolved = mkdtemp(tmpl);
    return resolved ? [NSString stringWithUTF8String:resolved] : nil;
}

static BOOL unpackGVID(NSString *inputPath, NSString *outDir, int maxFrames,
                       NSMutableArray<NSDictionary *> *frameHeaders) {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:inputPath];
    if (!fh) {
        fprintf(stderr, "error: cannot open GVID input: %s\n", [inputPath UTF8String]);
        return NO;
    }
    @try {
        NSData *clipHeader = [fh readDataOfLength:32];
        if (clipHeader.length != 32) {
            fprintf(stderr, "error: GVID input is too small: %s\n", [inputPath UTF8String]);
            [fh closeFile];
            return NO;
        }
        const uint8_t *h = clipHeader.bytes;
        uint32_t magic = le32_gpr2prores(h);
        uint8_t version = h[4];
        uint32_t frameCountHint = le32_gpr2prores(h + 28);
        if (magic != 0x44495647u || version != 1) {
            fprintf(stderr, "error: bad GVID header in %s (magic=0x%08x version=%u)\n",
                    [inputPath UTF8String], magic, version);
            [fh closeFile];
            return NO;
        }

        uint32_t framesWritten = 0;
        uint64_t streamOffset = 32;
        NSMutableSet<NSNumber *> *seenTags = [NSMutableSet set];
        while (true) {
            NSData *frameHeader = [fh readDataOfLength:16];
            if (frameHeader.length == 0) break;
            if (frameHeader.length != 16) {
                fprintf(stderr, "error: truncated GVID frame header after %u frames\n", framesWritten);
                [fh closeFile];
                return NO;
            }
            const uint8_t *fhdr = frameHeader.bytes;
            uint32_t frameMagic = le32_gpr2prores(fhdr);
            uint32_t payloadSize = le32_gpr2prores(fhdr + 4);
            uint64_t frameTag = le64_gpr2prores(fhdr + 8);
            uint64_t payloadOffset = streamOffset + 16;
            if (frameMagic != 0x004D5246u) {
                fprintf(stderr, "error: bad GVID frame magic after %u frames (0x%08x)\n",
                        framesWritten, frameMagic);
                [fh closeFile];
                return NO;
            }
            NSNumber *tagNumber = @((unsigned long long)frameTag);
            if ([seenTags containsObject:tagNumber]) {
                fprintf(stderr, "error: duplicate GVID frame tag %llu\n",
                        (unsigned long long)frameTag);
                [fh closeFile];
                return NO;
            }
            [seenTags addObject:tagNumber];
            NSData *payload = [fh readDataOfLength:payloadSize];
            if (payload.length != payloadSize) {
                fprintf(stderr, "error: truncated GVID payload for frame tag %llu\n",
                        (unsigned long long)frameTag);
                [fh closeFile];
                return NO;
            }
            if ((int)framesWritten < maxFrames) {
                NSString *name = [NSString stringWithFormat:@"frame_%06u.gpr", framesWritten];
                NSString *dst = [outDir stringByAppendingPathComponent:name];
                if (![payload writeToFile:dst atomically:NO]) {
                    fprintf(stderr, "error: failed to write unpacked GVID frame: %s\n", [dst UTF8String]);
                    [fh closeFile];
                    return NO;
                }
                if (frameHeaders) {
                    [frameHeaders addObject:@{
                        @"frame_index": @(framesWritten),
                        @"frame_tag": @((unsigned long long)frameTag),
                        @"payload_offset": @((unsigned long long)payloadOffset),
                        @"payload_size": @(payloadSize),
                    }];
                }
            }
            framesWritten++;
            streamOffset = payloadOffset + payloadSize;
        }
        if (frameCountHint != 0 && frameCountHint != framesWritten) {
            fprintf(stderr, "error: GVID frame_count_hint=%u but stream has %u frames\n",
                    frameCountHint, framesWritten);
            [fh closeFile];
            return NO;
        }
        [fh closeFile];
        fprintf(stderr, "GVID input: unpacked %u frames to %s\n", framesWritten, [outDir UTF8String]);
        return YES;
    } @catch (NSException *ex) {
        fprintf(stderr, "error: exception while unpacking GVID %s: %s\n",
                [inputPath UTF8String], [[ex reason] UTF8String]);
        [fh closeFile];
        return NO;
    }
}

static BOOL validateGVIDDispatchPlan(NSString *dispatchPath, NSArray<NSDictionary *> *actualFrameHeaders) {
    NSUInteger frameCount = actualFrameHeaders.count;
    NSData *data = [NSData dataWithContentsOfFile:dispatchPath];
    if (!data) {
        fprintf(stderr, "error: cannot read --gvid-dispatch %s\n", [dispatchPath UTF8String]);
        return NO;
    }
    NSError *err = nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (![json isKindOfClass:[NSDictionary class]]) {
        fprintf(stderr, "error: --gvid-dispatch is not a JSON object: %s\n", [dispatchPath UTF8String]);
        return NO;
    }
    NSDictionary *root = (NSDictionary *)json;
    if (![root[@"schema"] isEqual:@"gvid_runtime_dispatch.v1"]) {
        fprintf(stderr, "error: unsupported --gvid-dispatch schema in %s\n", [dispatchPath UTF8String]);
        return NO;
    }
    NSArray *frames = root[@"frames"];
    if (![frames isKindOfClass:[NSArray class]]) {
        fprintf(stderr, "error: --gvid-dispatch frames must be an array\n");
        return NO;
    }
    if (frames.count < frameCount) {
        fprintf(stderr, "error: --gvid-dispatch has %lu frames but render needs %lu\n",
                (unsigned long)frames.count, (unsigned long)frameCount);
        return NO;
    }
    NSNumber *declaredFrameCount = root[@"frame_count"];
    if (declaredFrameCount &&
        (![declaredFrameCount isKindOfClass:[NSNumber class]] ||
         declaredFrameCount.unsignedIntegerValue < frameCount)) {
        fprintf(stderr, "error: --gvid-dispatch frame_count cannot cover %lu rendered frames\n",
                (unsigned long)frameCount);
        return NO;
    }

    NSUInteger accepted = 0;
    NSUInteger allTargets = 0;
    NSUInteger tiles = 0;
    for (NSUInteger i = 0; i < frameCount; i++) {
        NSDictionary *frame = frames[i];
        if (![frame isKindOfClass:[NSDictionary class]]) {
            fprintf(stderr, "error: --gvid-dispatch frame %lu is not an object\n", (unsigned long)i);
            return NO;
        }
        NSNumber *frameIndex = frame[@"frame_index"];
        if (![frameIndex isKindOfClass:[NSNumber class]] || frameIndex.unsignedIntegerValue != i) {
            fprintf(stderr, "error: --gvid-dispatch frame %lu has frame_index=%s\n",
                    (unsigned long)i,
                    frameIndex ? [[frameIndex stringValue] UTF8String] : "<missing>");
            return NO;
        }
        NSDictionary *actual = actualFrameHeaders[i];
        NSNumber *frameTag = frame[@"frame_tag"];
        NSNumber *payloadOffset = frame[@"payload_offset"];
        NSNumber *payloadSize = frame[@"payload_size"];
        if (![frameTag isKindOfClass:[NSNumber class]] ||
            ![payloadOffset isKindOfClass:[NSNumber class]] ||
            ![payloadSize isKindOfClass:[NSNumber class]] ||
            payloadSize.unsignedLongLongValue == 0) {
            fprintf(stderr, "error: --gvid-dispatch frame %lu missing valid frame_tag/payload_offset/payload_size\n",
                    (unsigned long)i);
            return NO;
        }
        if (frameTag.unsignedLongLongValue != [actual[@"frame_tag"] unsignedLongLongValue] ||
            payloadOffset.unsignedLongLongValue != [actual[@"payload_offset"] unsignedLongLongValue] ||
            payloadSize.unsignedLongLongValue != [actual[@"payload_size"] unsignedLongLongValue]) {
            fprintf(stderr, "error: --gvid-dispatch frame %lu does not match GVID stream header\n",
                    (unsigned long)i);
            return NO;
        }
        NSArray *rawCleanTiles = frame[@"raw_clean_tiles"];
        if (![rawCleanTiles isKindOfClass:[NSArray class]]) {
            fprintf(stderr, "error: --gvid-dispatch frame %lu raw_clean_tiles must be an array\n",
                    (unsigned long)i);
            return NO;
        }
        for (NSDictionary *tile in rawCleanTiles) {
            if (![tile isKindOfClass:[NSDictionary class]]) {
                fprintf(stderr, "error: --gvid-dispatch frame %lu has non-object tile\n", (unsigned long)i);
                return NO;
            }
            NSString *policy = tile[@"policy"];
            if (![policy isKindOfClass:[NSString class]]) {
                fprintf(stderr, "error: --gvid-dispatch frame %lu tile missing policy\n", (unsigned long)i);
                return NO;
            }
            if ([policy isEqualToString:@"accepted_only_raw_clean"]) accepted++;
            else if ([policy isEqualToString:@"all_targets_raw_clean"]) allTargets++;
            else {
                fprintf(stderr, "error: --gvid-dispatch frame %lu tile has unknown policy %s\n",
                        (unsigned long)i, [policy UTF8String]);
                return NO;
            }
            tiles++;
        }
    }
    fprintf(stderr, "GVID dispatch: %s frames=%lu tiles=%lu accepted_only=%lu all_targets=%lu\n",
            [dispatchPath UTF8String],
            (unsigned long)frameCount,
            (unsigned long)tiles,
            (unsigned long)accepted,
            (unsigned long)allTargets);
    return YES;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *inputPath = nil;
        NSString *outputPath = nil;
        NSString *metaDngPath = nil;
        NSString *modelRoot = [[[NSProcessInfo processInfo] environment]
            objectForKey:@"GPR_MODEL_ROOT"];
        if (modelRoot.length == 0) {
            NSString *externalRoot = [[[NSProcessInfo processInfo] environment]
                objectForKey:@"GPR_EXTERNAL_ROOT"];
            if (externalRoot.length == 0) externalRoot = [[NSFileManager defaultManager] currentDirectoryPath];
            modelRoot = [externalRoot stringByAppendingPathComponent:@"models"];
        }
        NSString *ckptPath = [[[NSProcessInfo processInfo] environment]
            objectForKey:@"GPR_SUPER_RES_MLPACKAGE"];
        if (ckptPath.length == 0) {
            ckptPath = [modelRoot stringByAppendingPathComponent:@"super_res.mlpackage"];
        }
        NSString *cnnBackend = GPR_DEFAULT_CNN_BACKEND;
        NSString *demosaicMode = @"metal-bilinear";
        NSString *outResolution = @"8k";
        NSString *cnnScale = @"2x";
        NSString *lookMode = @"none";
        NSString *gvidDispatchPath = nil;
        int fps = 24;
        int maxFrames = INT_MAX;
        BOOL aaOn = YES;
        BOOL noCNN = !GPR_HAVE_CNN;
        BOOL noCodec = NO;
        BOOL timing = NO;
        BOOL phase0 = NO;
        BOOL skipErrors = NO;

        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
                print_usage(stdout); return 0;
            }
            if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
                print_version(); return 0;
            }
            if (!strcmp(a, "--meta-dng") && i + 1 < argc) {
                metaDngPath = @(argv[++i]);
            } else if (!strcmp(a, "--max-frames") && i + 1 < argc) {
                maxFrames = atoi(argv[++i]);
            } else if (!strcmp(a, "--fps") && i + 1 < argc) {
                fps = atoi(argv[++i]);
                if (fps <= 0 || fps > 240) {
                    fprintf(stderr, "error: --fps must be in (0, 240]\n");
                    return 1;
                }
            } else if (!strcmp(a, "--ckpt") && i + 1 < argc) {
                if (!GPR_HAVE_CNN) {
                    fprintf(stderr, "error: --ckpt requires CNN support; rebuild with CNN_BACKENDS=coreml, metal, or both\n");
                    return 1;
                }
                ckptPath = @(argv[++i]);
            } else if (!strcmp(a, "--aa") && i + 1 < argc) {
                const char *v = argv[++i];
                if (strcmp(v, "on") != 0 && strcmp(v, "off") != 0) {
                    fprintf(stderr, "error: --aa expects 'on' or 'off' (got '%s')\n", v);
                    return 1;
                }
                aaOn = !strcmp(v, "on");
            } else if (!strcmp(a, "--cnn-backend") && i + 1 < argc) {
                cnnBackend = @(argv[++i]);
                if (![cnnBackend isEqualToString:@"coreml"] &&
                    ![cnnBackend isEqualToString:@"mpsgraph"] &&
                    ![cnnBackend isEqualToString:@"metal"]) {
                    fprintf(stderr, "error: --cnn-backend expects coreml|mpsgraph|metal (got '%s')\n",
                            [cnnBackend UTF8String]);
                    return 1;
                }
                if (!GPRCNNBackendAvailable(cnnBackend)) {
                    fprintf(stderr, "error: --cnn-backend '%s' is unavailable in this build; rebuild with CNN_BACKENDS=%s or both\n",
                            [cnnBackend UTF8String],
                            [cnnBackend isEqualToString:@"coreml"] ? "coreml" : "metal");
                    return 1;
                }
            } else if (!strcmp(a, "--demosaic") && i + 1 < argc) {
                demosaicMode = @(argv[++i]);
                if (![demosaicMode isEqualToString:@"metal-bilinear"] &&
                    ![demosaicMode isEqualToString:@"core-image"]) {
                    fprintf(stderr, "error: --demosaic expects metal-bilinear|core-image (got '%s')\n",
                            [demosaicMode UTF8String]);
                    return 1;
                }
            } else if (!strcmp(a, "--cnn-scale") && i + 1 < argc) {
                cnnScale = @(argv[++i]);
                NSString *cs = [cnnScale lowercaseString];
                if (![cs isEqualToString:@"1x"] && ![cs isEqualToString:@"2x"]) {
                    fprintf(stderr, "error: --cnn-scale expects 1x|2x (got '%s')\n",
                            [cnnScale UTF8String]);
                    return 1;
                }
                cnnScale = cs;
            } else if (!strcmp(a, "--out-resolution") && i + 1 < argc) {
                outResolution = @(argv[++i]);
                NSString *r = [outResolution lowercaseString];
                if (![r isEqualToString:@"2k"] && ![r isEqualToString:@"uhd"] &&
                    ![r isEqualToString:@"4k"] && ![r isEqualToString:@"6k"] &&
                    ![r isEqualToString:@"8k"]) {
                    fprintf(stderr, "error: --out-resolution expects 2k|uhd|4k|6k|8k (got '%s')\n",
                            [outResolution UTF8String]);
                    return 1;
                }
                outResolution = r;
            } else if (!strcmp(a, "--gvid-dispatch") && i + 1 < argc) {
                gvidDispatchPath = @(argv[++i]);
            } else if (!strcmp(a, "--mission-look")) {
                lookMode = @"mission1";
            } else if (!strcmp(a, "--no-cnn")) noCNN = YES;
            else if (!strcmp(a, "--no-codec")) noCodec = YES;
            else if (!strcmp(a, "--skip-errors")) skipErrors = YES;
            else if (!strcmp(a, "--timing")) timing = YES;
            else if (!strcmp(a, "--phase0")) phase0 = YES;
            else if (a[0] != '-') {
                if (!inputPath) inputPath = @(a);
                else if (!outputPath) outputPath = @(a);
                else {
                    fprintf(stderr, "error: unexpected extra positional argument: %s\n", a);
                    print_usage(stderr);
                    return 1;
                }
            } else {
                fprintf(stderr, "error: unknown option: %s\n", a);
                print_usage(stderr);
                return 1;
            }
        }

        if (!inputPath) { print_usage(stderr); return 1; }

        // Early sanity: Metal device available?
        id<MTLDevice> probe = MTLCreateSystemDefaultDevice();
        if (!probe) {
            fprintf(stderr, "error: no Metal device available — this build requires macOS 14+ on Apple Silicon or AMD\n");
            return 1;
        }
        probe = nil;

        // GVID input: neutral stream of per-frame .gpr payloads. Unpack to a
        // temp dir and reuse the existing .gpr playback path.
        NSString *inputExt = [inputPath.lowercaseString pathExtension];
        BOOL isGVIDContainer = [inputExt isEqualToString:@"gvid"];
        NSString *gvidUnpackDir = nil;
        NSString *originalGvidPath = nil;
        NSMutableArray<NSDictionary *> *gvidFrameHeaders = [NSMutableArray array];
        if (isGVIDContainer) {
            if (![[NSFileManager defaultManager] fileExistsAtPath:inputPath]) {
                fprintf(stderr, "error: input not found: %s\n", [inputPath UTF8String]);
                return 1;
            }
            originalGvidPath = inputPath;
            gvidUnpackDir = makeTempDir(@"gpr2prores_gvid");
            if (!gvidUnpackDir) {
                fprintf(stderr, "error: mkdtemp failed for GVID unpack (errno %d)\n", errno);
                return 1;
            }
            if (!unpackGVID(inputPath, gvidUnpackDir, maxFrames, gvidFrameHeaders)) {
                return 1;
            }
            NSString *sidecar = [inputPath stringByAppendingString:@".meta.json"];
            if ([[NSFileManager defaultManager] fileExistsAtPath:sidecar]) {
                fprintf(stderr, "GVID input: found metadata sidecar %s\n", [sidecar UTF8String]);
            }
            if (!gvidDispatchPath) {
                NSString *autoDispatch = [inputPath stringByAppendingString:@".dispatch.json"];
                if ([[NSFileManager defaultManager] fileExistsAtPath:autoDispatch]) {
                    gvidDispatchPath = autoDispatch;
                }
            }
            inputPath = gvidUnpackDir;
        }

        // MOV container input with a GPRr/GPR1 track. Unpack to a temp dir (fast —
        // mostly memcpy from the mmap'd MOV) and treat as a .gpr dir from there.
        NSString *movExt = [inputPath.lowercaseString pathExtension];
        BOOL isMovContainer = [movExt isEqualToString:@"mov"] || hasMOVSignature(inputPath);
        NSString *movUnpackDir = nil;
        if (isMovContainer) {
            if (![[NSFileManager defaultManager] fileExistsAtPath:inputPath]) {
                fprintf(stderr, "error: input not found: %s\n", [inputPath UTF8String]);
                return 1;
            }
            movUnpackDir = makeTempDir(@"gpr2prores_mov");
            if (!movUnpackDir) {
                fprintf(stderr, "error: mkdtemp failed (errno %d)\n", errno);
                return 1;
            }
            NSString *exeDir = [[NSString stringWithUTF8String:argv[0]] stringByDeletingLastPathComponent];
            if (exeDir.length == 0) exeDir = @".";
            NSString *movTool = [exeDir stringByAppendingPathComponent:@"gpr_mov_tool"];
            if (![[NSFileManager defaultManager] fileExistsAtPath:movTool]) {
                // Try installed location, then PATH-relative.
                NSString *installed = @"/usr/local/bin/gpr_mov_tool";
                if ([[NSFileManager defaultManager] fileExistsAtPath:installed]) movTool = installed;
                else movTool = @"gpr_mov_tool";
            }
            NSTask *task = [NSTask new];
            task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/env"];
            task.arguments = @[movTool, @"unpack", inputPath, movUnpackDir];
            NSError *launchError = nil;
            if (![task launchAndReturnError:&launchError]) {
                fprintf(stderr, "error: cannot launch MOV unpacker: %s\n",
                        launchError.localizedDescription.UTF8String);
                return 1;
            }
            [task waitUntilExit];
            if (task.terminationStatus != 0) {
                fprintf(stderr, "error: gpr_mov_tool unpack failed (status %d) for %s\n",
                        task.terminationStatus, [inputPath UTF8String]);
                return 1;
            }
            fprintf(stderr, "MOV input: unpacked to %s\n", movUnpackDir.UTF8String);
            inputPath = movUnpackDir;
        }

        if (![[NSFileManager defaultManager] fileExistsAtPath:inputPath]) {
            fprintf(stderr, "error: input not found: %s\n", [inputPath UTF8String]);
            return 1;
        }

        // Build frame list.
        NSMutableArray<NSString *> *frames = [NSMutableArray array];
        BOOL isDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:inputPath isDirectory:&isDir];
        if (isDir) {
            NSArray<NSString *> *contents =
                [[NSFileManager defaultManager] contentsOfDirectoryAtPath:inputPath error:nil];
            NSArray<NSString *> *sorted = [contents sortedArrayUsingSelector:@selector(compare:)];
            BOOL anyGPR = NO;
            for (NSString *f in sorted) if ([f.lowercaseString hasSuffix:@".gpr"]) { anyGPR = YES; break; }
            for (NSString *f in sorted) {
                NSString *lower = [f lowercaseString];
                if (anyGPR) {
                    if ([lower hasSuffix:@".gpr"])
                        [frames addObject:[inputPath stringByAppendingPathComponent:f]];
                } else {
                    if ([lower hasSuffix:@".dng"] || [lower hasSuffix:@".nef"])
                        [frames addObject:[inputPath stringByAppendingPathComponent:f]];
                }
            }
        } else {
            [frames addObject:inputPath];
        }
        if ((int)frames.count > maxFrames) {
            [frames removeObjectsInRange:NSMakeRange(maxFrames, frames.count - maxFrames)];
        }
        if (frames.count == 0) { fprintf(stderr, "error: no input frames found in %s\n", [inputPath UTF8String]); return 1; }
        if ([lookMode isEqualToString:@"mission1"] && ![demosaicMode isEqualToString:@"core-image"]) {
            fprintf(stderr, "error: --mission-look requires --demosaic core-image\n");
            return 1;
        }
        if (gvidDispatchPath) {
            if (!originalGvidPath && !isGVIDContainer) {
                fprintf(stderr, "error: --gvid-dispatch is only valid with .gvid input\n");
                return 1;
            }
            if (!validateGVIDDispatchPlan(gvidDispatchPath, gvidFrameHeaders)) {
                return 1;
            }
            fprintf(stderr, "GVID dispatch: policy validated; raw-clean model application is not wired in this renderer yet\n");
        }

        BOOL gprMode = pathHasExt(frames[0], @"gpr");

        // Validate --no-codec + .gpr is invalid (the codec is *required* to decode GPR).
        if (gprMode && noCodec) {
            fprintf(stderr, "error: --no-codec is incompatible with .gpr input (decoder is required)\n");
            return 1;
        }
        if (gprMode && !noCNN && ![[NSFileManager defaultManager] fileExistsAtPath:ckptPath]) {
            fprintf(stderr, "error: super-res checkpoint not found: %s\n", [ckptPath UTF8String]);
            fprintf(stderr, "       (pass --no-cnn to skip super-res, or --ckpt PATH)\n");
            return 1;
        }

        fprintf(stderr, "gpr2prores: %lu %s frames, output=%s\n",
                (unsigned long)frames.count,
                gprMode ? "GPR" : "DNG",
                outputPath ? [outputPath UTF8String] : "<none>");

        if (gprMode && !metaDngPath) {
            NSString *sibling = findSiblingDNG(frames[0]);
            if (sibling) {
                fprintf(stderr, "  --meta-dng not supplied; using sibling DNG: %s\n", [sibling UTF8String]);
                metaDngPath = sibling;
            } else {
                fprintf(stderr, "error: GPR input requires --meta-dng <path-to-source-DNG>\n");
                fprintf(stderr, "       (no .dng files were found alongside %s)\n", [frames[0] UTF8String]);
                return 1;
            }
        }
        if (metaDngPath && ![[NSFileManager defaultManager] fileExistsAtPath:metaDngPath]) {
            fprintf(stderr, "error: --meta-dng path not found: %s\n", [metaDngPath UTF8String]);
            return 1;
        }

        if (phase0) {
            if (gprMode) {
                NSData *d = [NSData dataWithContentsOfFile:frames[0]];
                if (!d || d.length < 48) { fprintf(stderr, "error: phase0 cannot read %s\n", [frames[0] UTF8String]); return 1; }
                const uint8_t *b = d.bytes;
                uint32_t magic = b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24);
                uint32_t w = b[8] | (b[9]<<8) | (b[10]<<16) | (b[11]<<24);
                uint32_t h = b[12] | (b[13]<<8) | (b[14]<<16) | (b[15]<<24);
                uint32_t dec = b[44] | (b[45]<<8) | (b[46]<<16) | (b[47]<<24);
                fprintf(stderr, "Phase 0 (GPR): %s\n  magic=0x%08x w=%u h=%u decimate=%u bytes=%lu\n",
                        [frames[0] UTF8String], magic, w, h, dec, (unsigned long)d.length);
            } else {
                DNGInfo info;
                uint16_t *bayer = [DNGReader readBayerFromPath:frames[0] info:&info];
                if (!bayer) { fprintf(stderr, "error: failed to read %s\n", [frames[0] UTF8String]); return 1; }
                fprintf(stderr, "Phase 0 (DNG): read %s\n  %ux%u, %u bps, CFA=%u, black=%u, white=%u\n",
                        [frames[0] UTF8String], info.width, info.height,
                        info.bitsPerSample, info.cfaPattern, info.blackLevel, info.whiteLevel);
                free(bayer);
            }
            return 0;
        }

        if (!outputPath) { fprintf(stderr, "error: OUTPUT path required\n"); print_usage(stderr); return 1; }

        // Disk-space precheck: estimate ~50 MB/frame for ProRes 4444 at 4K.
        // Skip if we can't stat the target dir.
        int64_t free_bytes = freeBytesForPath(outputPath);
        if (free_bytes > 0) {
            int64_t need = (int64_t)frames.count * 50LL * 1024LL * 1024LL;
            if (free_bytes < need) {
                fprintf(stderr, "warning: estimated need %lld MB, only %lld MB free on output FS — continuing anyway\n",
                        (long long)(need / (1024LL*1024LL)),
                        (long long)(free_bytes / (1024LL*1024LL)));
            }
        }

        GPRPipeline *pipe = [[GPRPipeline alloc] initWithFirstFrame:frames[0]
                                                        metaDngPath:metaDngPath
                                                            ckptPath:ckptPath
                                                            outPath:outputPath
                                                                fps:fps
                                                              aaOn:aaOn
                                                             noCNN:noCNN
                                                           noCodec:noCodec
                                                          timing:timing
                                                       cnnBackend:cnnBackend
                                                     demosaicMode:demosaicMode
                                                    outResolution:outResolution
                                                          cnnScale:cnnScale
                                                          lookMode:lookMode];
        if (!pipe) {
            fprintf(stderr, "error: pipeline init failed (see messages above)\n");
            return 1;
        }
        [pipe setSkipErrors:skipErrors];

        int rc = [pipe runFrames:frames];
        if (rc != 0) {
            fprintf(stderr, "error: pipeline returned rc=%d\n", rc);
        }
        return rc;
    }
}
