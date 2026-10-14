// gpr_mov_tool.m — pack/unpack a sequence of .gpr files into / from a single
// MOV container with a custom GPR1 codec_tag.
//
// usage:
//   gpr_mov_tool pack    <in_dir_of_gpr> <out.mov>
//                          [--fps N] [--audio WAV] [--tc-start HH:MM:SS:FF]
//                          [--tc-drop] [--meta-dir DIR]
//   gpr_mov_tool unpack  <in.mov>        <out_dir>  [--prefix P]
//   gpr_mov_tool info    <in.mov>
//
// New options:
//   --audio WAV         embed audio from a WAV file (s16/s24/f32 PCM mono/stereo).
//   --tc-start STR      set start timecode "HH:MM:SS:FF" or "HH:MM:SS;FF" (drop).
//   --tc-drop           force drop-frame TC (otherwise inferred from separator).
//   --meta-dir DIR      directory of sibling DNGs (frame_NNNN.dng) used to
//                       write per-frame EXIF to the companion .exif file.

#import <Foundation/Foundation.h>
#import "GPRMovWriter.h"
#import "GPRMovReader.h"
#import "GPRFileReader.h"
#import "DNGExif.h"

#define TOOL_VERSION "1.0.0"

static void print_usage(FILE *out, const char *argv0) {
    fprintf(out,
        "gpr_mov_tool %s\n"
        "USAGE:\n"
        "  %s pack    <in_dir_of_gpr> <out.mov> [opts]\n"
        "      [--fps N] (default 24)\n"
        "      [--audio WAV] embed PCM audio from WAV file\n"
        "      [--tc-start HH:MM:SS:FF] start timecode (use ';' before FF for drop-frame)\n"
        "      [--tc-drop] force drop-frame flag\n"
        "      [--meta-dir DIR] pull per-frame EXIF from frame_NNNN.dng in DIR\n"
        "\n"
        "  %s unpack  <in.mov> <out_dir> [--prefix P]\n"
        "      Unpacks each video frame as <out_dir>/<prefix>_NNN.gpr.\n"
        "\n"
        "  %s info    <in.mov>\n"
        "      Print container summary: tracks, frames, timecode, EXIF presence.\n",
        TOOL_VERSION, argv0, argv0, argv0);
}

// Try to locate a DNG file matching `gprPath` in `metaDir`. The convention is
// frame_NNNN.dng beside frame_NNN.gpr. Falls back to the same basename swap.
static NSString *exifPathForGPR(NSString *gprPath, NSString *metaDir) {
    if (!metaDir) return nil;
    NSString *base = [[gprPath lastPathComponent] stringByDeletingPathExtension];
    // Try exact-basename match first.
    NSString *cand = [metaDir stringByAppendingPathComponent:
                        [base stringByAppendingPathExtension:@"dng"]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:cand]) return cand;
    // Try a numeric-suffix match (e.g. frame_001 → frame_0001).
    NSScanner *sc = [NSScanner scannerWithString:base];
    NSCharacterSet *digits = [NSCharacterSet decimalDigitCharacterSet];
    [sc scanUpToCharactersFromSet:digits intoString:NULL];
    NSInteger n = 0;
    if ([sc scanInteger:&n]) {
        for (int width = 3; width <= 6; width++) {
            NSString *guess = [NSString stringWithFormat:@"frame_%0*ld.dng", width, (long)n];
            NSString *gp = [metaDir stringByAppendingPathComponent:guess];
            if ([[NSFileManager defaultManager] fileExistsAtPath:gp]) return gp;
        }
    }
    return nil;
}

static int do_pack(NSString *inDir, NSString *outPath, int fps,
                   NSString *audioPath, NSString *tcStart, BOOL tcDrop,
                   NSString *metaDir) {
    NSError *err = nil;
    NSArray<NSString *> *contents =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:inDir error:&err];
    if (!contents) {
        fprintf(stderr, "pack error: cannot list %s\n", [inDir UTF8String]);
        return 1;
    }
    NSMutableArray<NSString *> *gprs = [NSMutableArray array];
    for (NSString *f in [contents sortedArrayUsingSelector:@selector(compare:)]) {
        if ([f.lowercaseString hasSuffix:@".gpr"])
            [gprs addObject:[inDir stringByAppendingPathComponent:f]];
    }
    if (gprs.count == 0) { fprintf(stderr, "pack error: no .gpr files in %s\n", [inDir UTF8String]); return 1; }

    GPRFileInfo gi = {0};
    if (![GPRFileReader readHeaderFromPath:gprs[0] info:&gi]) {
        fprintf(stderr, "pack error: cannot read header of %s\n", [gprs[0] UTF8String]);
        return 1;
    }
    GPRMovWriter *w = [[GPRMovWriter alloc] initWithPath:outPath
                                                    width:gi.decWidth
                                                   height:gi.decHeight
                                                      fps:fps];
    if (!w) { fprintf(stderr, "pack error: writer init failed\n"); return 1; }

    if (tcStart && ![w addTimecodeStart:tcStart dropFrame:tcDrop]) {
        fprintf(stderr, "pack error: timecode setup failed\n");
        return 1;
    }
    if (audioPath && ![w addAudioFromWAV:audioPath]) {
        fprintf(stderr, "pack error: audio setup failed for %s\n", [audioPath UTF8String]);
        return 1;
    }

    size_t total = 0;
    int idx = 0, errs = 0;
    for (NSString *p in gprs) {
        NSData *d = [NSData dataWithContentsOfFile:p options:NSDataReadingMappedAlways error:nil];
        if (!d) { fprintf(stderr, "pack: cannot read %s (skip)\n", [p UTF8String]); errs++; continue; }

        DNGExifInfo *exif = nil;
        if (metaDir) {
            NSString *dngP = exifPathForGPR(p, metaDir);
            if (dngP) exif = [DNGExif readFromPath:dngP];
        }
        int rc = [w appendEncodedBytes:d.bytes length:d.length exif:exif];
        if (rc != 0) {
            fprintf(stderr, "pack error: append %s rc=%d\n", [p UTF8String], rc);
            errs++;
            break;
        }
        total += d.length;
        idx++;
    }
    if ([w finish] != 0) errs++;
    fprintf(stderr, "pack: %d frames, %zu bytes payload → %s%s\n",
            idx, total, [outPath UTF8String], errs ? " (with errors)" : "");
    return errs ? 2 : 0;
}

static int do_unpack(NSString *inPath, NSString *outDir, NSString *prefix) {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDirectory = NO;
    if ([fm fileExistsAtPath:outDir isDirectory:&isDirectory]) {
        if (!isDirectory) return 1;
    } else if (![fm createDirectoryAtPath:outDir withIntermediateDirectories:YES attributes:nil error:nil]) {
        return 1;
    }
    GPRMovInfo info = {0};
    GPRMovReader *r = [[GPRMovReader alloc] initWithPath:inPath info:&info];
    if (!r) return 1;
    int idx = 0;
    while (1) {
        NSData *d = [r nextFrame];
        if (!d) break;
        NSString *out = [outDir stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"%@_%03d.gpr", prefix, idx]];
        if (![d writeToFile:out atomically:NO]) {
            fprintf(stderr, "unpack: cannot write %s\n", out.UTF8String);
            return 1;
        }
        idx++;
    }
    fprintf(stderr, "unpack: %d frames → %s\n", idx, [outDir UTF8String]);
    return r.readFailed || idx == 0 || (info.numFrames > 0 && idx != info.numFrames) ? 1 : 0;
}

static int do_info(NSString *inPath) {
    GPRMovInfo info = {0};
    GPRMovReader *r = [[GPRMovReader alloc] initWithPath:inPath info:&info];
    if (!r) return 1;
    int nFrames = 0;
    DNGExifInfo *firstExif = nil;
    while (1) {
        DNGExifInfo *e = nil;
        NSData *d = [r nextFrameWithExif:&e];
        if (!d) break;
        if (nFrames == 0 && e) firstExif = e;
        nFrames++;
    }
    fprintf(stdout, "container : %s\n", [inPath UTF8String]);
    fprintf(stdout, "video     : %ux%u @ %d fps, %d frames\n",
            info.width, info.height, info.fps, nFrames);
    fprintf(stdout, "audio     : %s\n", info.hasAudio ? "yes" : "no");
    fprintf(stdout, "timecode  : %s%s\n", info.hasTimecode ? "yes " : "no", info.timecodeStart);
    fprintf(stdout, "exif      : %s\n", info.hasExif ? "yes (per-frame)" : "no");
    if (firstExif) {
        fprintf(stdout, "  ISO=%g  shutter=%g  f/%g  focal=%gmm  lens=%s\n",
                firstExif.iso, firstExif.exposureTime, firstExif.aperture, firstExif.focalLength,
                firstExif.lensModel ? [firstExif.lensModel UTF8String] : "<n/a>");
        if (firstExif.asShotNeutral.count == 3) {
            fprintf(stdout, "  AsShotNeutral = [%g %g %g]\n",
                    firstExif.asShotNeutral[0].doubleValue,
                    firstExif.asShotNeutral[1].doubleValue,
                    firstExif.asShotNeutral[2].doubleValue);
        }
    }
    return r.readFailed || nFrames == 0 || (info.numFrames > 0 && nFrames != info.numFrames) ? 1 : 0;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc < 2) { print_usage(stderr, argv[0]); return 1; }
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
            print_usage(stdout, argv[0]); return 0;
        }
        if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
            fprintf(stdout, "gpr_mov_tool %s\n", TOOL_VERSION); return 0;
        }

        NSString *mode = @(argv[1]);
        if ([mode isEqualToString:@"info"]) {
            if (argc < 3) { print_usage(stderr, argv[0]); return 1; }
            return do_info(@(argv[2]));
        }
        if (argc < 4) { print_usage(stderr, argv[0]); return 1; }

        if ([mode isEqualToString:@"pack"]) {
            NSString *inDir   = @(argv[2]);
            NSString *outPath = @(argv[3]);
            int fps = 24;
            NSString *audioPath = nil;
            NSString *tcStart = nil;
            BOOL tcDrop = NO;
            NSString *metaDir = nil;
            for (int i = 4; i < argc; i++) {
                if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
                    fps = atoi(argv[++i]);
                    if (fps <= 0 || fps > 240) {
                        fprintf(stderr, "error: --fps must be in (0,240]\n");
                        return 1;
                    }
                } else if (!strcmp(argv[i], "--audio") && i + 1 < argc) {
                    audioPath = @(argv[++i]);
                    if (![[NSFileManager defaultManager] fileExistsAtPath:audioPath]) {
                        fprintf(stderr, "error: --audio file not found: %s\n", [audioPath UTF8String]);
                        return 1;
                    }
                } else if (!strcmp(argv[i], "--tc-start") && i + 1 < argc) {
                    tcStart = @(argv[++i]);
                } else if (!strcmp(argv[i], "--tc-drop")) {
                    tcDrop = YES;
                } else if (!strcmp(argv[i], "--meta-dir") && i + 1 < argc) {
                    metaDir = @(argv[++i]);
                    BOOL d = NO;
                    if (![[NSFileManager defaultManager] fileExistsAtPath:metaDir isDirectory:&d] || !d) {
                        fprintf(stderr, "error: --meta-dir not a directory: %s\n", [metaDir UTF8String]);
                        return 1;
                    }
                } else {
                    fprintf(stderr, "error: unknown pack option: %s\n", argv[i]);
                    print_usage(stderr, argv[0]);
                    return 1;
                }
            }
            return do_pack(inDir, outPath, fps, audioPath, tcStart, tcDrop, metaDir);
        } else if ([mode isEqualToString:@"unpack"]) {
            NSString *inPath = @(argv[2]);
            NSString *outDir = @(argv[3]);
            NSString *prefix = @"frame";
            for (int i = 4; i + 1 < argc; i++) {
                if (!strcmp(argv[i], "--prefix")) prefix = @(argv[++i]);
            }
            return do_unpack(inPath, outDir, prefix);
        }
        fprintf(stderr, "error: unknown mode: %s\n", argv[1]);
        print_usage(stderr, argv[0]);
        return 1;
    }
}
