// CIDNGBuilder.m — see header.
//
// Camera identity and color-science tags come from the template DNG verbatim.
// We parse the template's IFD0 and any SubIFDs to extract those tag values as
// raw bytes, then re-emit them in our synthesized DNG.
//
// DNG layout produced here:
//   TIFF header (8 B) → IFD0 (raw image) → tag-extra blobs → strip data.
//
// The static portion (header through end of tag-extra blobs) is computed
// once at -init time into `_staticHead`. Per-frame `dngFromBayerBytes:` is
// then a single NSData concatenation of (_staticHead || bayerBytes).
//
// CIRAWFilter accepts this minimal DNG and renders via its full RAW pipeline.

#import "CIDNGBuilder.h"

// TIFF tag types
#define TIFF_BYTE      1
#define TIFF_ASCII     2
#define TIFF_SHORT     3
#define TIFF_LONG      4
#define TIFF_RATIONAL  5
#define TIFF_SBYTE     6
#define TIFF_UNDEFINED 7
#define TIFF_SSHORT    8
#define TIFF_SLONG     9
#define TIFF_SRATIONAL 10
#define TIFF_FLOAT     11
#define TIFF_DOUBLE    12

// Tag IDs
#define T_NewSubFileType            0x00FE
#define T_ImageWidth                0x0100
#define T_ImageLength               0x0101
#define T_BitsPerSample             0x0102
#define T_Compression               0x0103
#define T_PhotometricInterpretation 0x0106
#define T_Make                      0x010F
#define T_Model                     0x0110
#define T_StripOffsets              0x0111
#define T_SamplesPerPixel           0x0115
#define T_RowsPerStrip              0x0116
#define T_StripByteCounts           0x0117
#define T_XResolution               0x011A
#define T_YResolution               0x011B
#define T_ResolutionUnit            0x0128
#define T_Software                  0x0131
#define T_CFARepeatPatternDim       0x828D
#define T_CFAPattern                0x828E
#define T_DNGVersion                0xC612
#define T_DNGBackwardVersion        0xC613
#define T_UniqueCameraModel         0xC614
#define T_LocalizedCameraModel      0xC615
#define T_BlackLevel                0xC61A
#define T_WhiteLevel                0xC61D
#define T_ColorMatrix1              0xC621
#define T_ColorMatrix2              0xC622
#define T_AsShotNeutral             0xC628
#define T_CalibrationIlluminant1    0xC65A
#define T_CalibrationIlluminant2    0xC65B
#define T_SubIFDs                   0x014A
#define T_ForwardMatrix1            0xC714
#define T_ForwardMatrix2            0xC715

// In-memory IFD entry, before serialization. Uses __unsafe_unretained because
// these structs are short-lived stack-allocated arrays that we memcpy around;
// ARC's strong-pointer rules don't play nicely with C struct memcpy. The
// actual NSData refs are retained by the autorelease pool / locals for the
// duration of init.
typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value;                              // inline 4-byte value
    __unsafe_unretained NSData *blob;            // when non-nil, the actual data
} IFDEntry;

// Append 4-byte LE integer.
static inline void put_u32(NSMutableData *d, uint32_t v) {
    uint8_t b[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff};
    [d appendBytes:b length:4];
}
static inline void put_u16(NSMutableData *d, uint16_t v) {
    uint8_t b[2] = {v & 0xff, (v >> 8) & 0xff};
    [d appendBytes:b length:2];
}

static NSData *blob_rational(uint32_t num, uint32_t den) {
    NSMutableData *d = [NSMutableData dataWithCapacity:8];
    put_u32(d, num); put_u32(d, den);
    return d;
}

static NSData *blob_rationals3(double a, double b, double c) {
    NSMutableData *d = [NSMutableData dataWithCapacity:24];
    const uint32_t D = 1000000;
    put_u32(d, (uint32_t)llround(a * D)); put_u32(d, D);
    put_u32(d, (uint32_t)llround(b * D)); put_u32(d, D);
    put_u32(d, (uint32_t)llround(c * D)); put_u32(d, D);
    return d;
}

// Encode a 3x3 matrix as 9 SRATIONALs (signed num/den).
static NSData *blob_srational_matrix(const float m[9]) {
    NSMutableData *d = [NSMutableData dataWithCapacity:72];
    const int32_t D = 1000000;
    for (int i = 0; i < 9; i++) {
        int32_t n = (int32_t)llround((double)m[i] * D);
        put_u32(d, (uint32_t)n);
        put_u32(d, (uint32_t)D);
    }
    return d;
}

static NSData *blob_ascii(const char *s) {
    size_t len = strlen(s) + 1;          // NUL-terminated, per TIFF
    return [NSData dataWithBytes:s length:len];
}

// Build IFD entry whose value fits in 4 bytes (BYTE/SHORT/LONG counts ≤ ...).
// We trust the caller to size things correctly.
static IFDEntry inline_entry(uint16_t tag, uint16_t type, uint32_t count, uint32_t v) {
    return (IFDEntry){tag, type, count, v, nil};
}
// Build IFD entry with arbitrary external blob; offset is patched at serialize time.
static IFDEntry blob_entry(uint16_t tag, uint16_t type, uint32_t count, NSData *blob) {
    return (IFDEntry){tag, type, count, 0, blob};
}

// CFA pattern bytes for our `info->cfaPattern` enum (0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR).
// CFAPattern values: 0=R, 1=G, 2=B.
static void cfa_bytes(uint32_t pattern, uint8_t out[4]) {
    switch (pattern) {
        case 0: out[0] = 0; out[1] = 1; out[2] = 1; out[3] = 2; break; // RGGB
        case 1: out[0] = 1; out[1] = 2; out[2] = 0; out[3] = 1; break; // GBRG
        case 2: out[0] = 1; out[1] = 0; out[2] = 2; out[3] = 1; break; // GRBG
        case 3: out[0] = 2; out[1] = 1; out[2] = 1; out[3] = 0; break; // BGGR
        default: out[0] = 0; out[1] = 1; out[2] = 1; out[3] = 2; break;
    }
}

// ============================================================================
// Template DNG tag extraction.
//
// Result: dict of @(tag) → @{ @"type": @(uint16_t), @"count": @(uint32_t),
//                             @"payload": NSData }. NSDictionary handles ARC.
// ============================================================================
static size_t typeSize(uint16_t t) {
    switch (t) {
        case TIFF_BYTE: case TIFF_ASCII: case TIFF_SBYTE: case TIFF_UNDEFINED: return 1;
        case TIFF_SHORT: case TIFF_SSHORT: return 2;
        case TIFF_LONG: case TIFF_SLONG: case TIFF_FLOAT: return 4;
        case TIFF_RATIONAL: case TIFF_SRATIONAL: case TIFF_DOUBLE: return 8;
        default: return 0;
    }
}

// Read tags from the IFD starting at ifdOffset, recurse into SubIFDs.
// Tags we want are camera identity plus the color/rendering tags below.
static void walkIFD(NSData *file, uint32_t ifdOffset,
                    NSMutableDictionary<NSNumber *, NSDictionary *> *out,
                    NSMutableSet<NSNumber *> *visited, int depth) {
    if (depth > 8) return;
    if (ifdOffset + 2 > file.length) return;
    if ([visited containsObject:@(ifdOffset)]) return;
    [visited addObject:@(ifdOffset)];
    const uint8_t *b = file.bytes;
    uint16_t n = b[ifdOffset] | (b[ifdOffset+1] << 8);
    if (n > 200) return;  // sanity bail on garbage
    uint32_t p = ifdOffset + 2;
    for (int i = 0; i < n; i++) {
        if (p + 12 > file.length) return;
        uint16_t tag   = b[p] | (b[p+1] << 8);
        uint16_t type  = b[p+2] | (b[p+3] << 8);
        uint32_t count = b[p+4] | (b[p+5] << 8) | (b[p+6] << 16) | (b[p+7] << 24);
        uint32_t value = b[p+8] | (b[p+9] << 8) | (b[p+10] << 16) | (b[p+11] << 24);
        size_t sz = typeSize(type);
        size_t total = sz * (size_t)count;
        const uint8_t *payloadPtr = NULL;
        if (sz == 0 || count == 0) { payloadPtr = NULL; }
        else if (total <= 4) {
            payloadPtr = b + p + 8;
        } else {
            if (value + total > file.length) { p += 12; continue; }
            payloadPtr = b + value;
        }

        // Capture interesting tags.
        if (tag == T_Make || tag == T_Model ||
            tag == T_UniqueCameraModel || tag == T_LocalizedCameraModel ||
            tag == T_ColorMatrix1 || tag == T_ColorMatrix2 ||
            tag == T_ForwardMatrix1 || tag == T_ForwardMatrix2 ||
            tag == T_CalibrationIlluminant1 || tag == T_CalibrationIlluminant2 ||
            tag == T_AsShotNeutral || tag == T_BlackLevel || tag == T_WhiteLevel ||
            tag == T_CFAPattern || tag == T_CFARepeatPatternDim) {
            if (payloadPtr && total > 0) {
                out[@(tag)] = @{
                    @"type":    @(type),
                    @"count":   @(count),
                    @"payload": [NSData dataWithBytes:payloadPtr length:total],
                };
            }
        }

        // Recurse into SubIFDs.
        if (tag == T_SubIFDs) {
            for (uint32_t k = 0; k < count && k < 16; k++) {
                uint32_t subOff;
                if (count == 1 && total <= 4) {
                    subOff = value;
                } else {
                    if (value + 4*k + 4 > file.length) continue;
                    subOff = b[value + 4*k]
                           | (b[value + 4*k + 1] << 8)
                           | (b[value + 4*k + 2] << 16)
                           | (b[value + 4*k + 3] << 24);
                }
                walkIFD(file, subOff, out, visited, depth + 1);
            }
        }

        p += 12;
    }
}

// Returns dict of tag → @{ @"type": @(uint16), @"count": @(uint32), @"payload": NSData }.
static NSDictionary<NSNumber *, NSDictionary *> *extractTagsFromTemplate(NSString *path) {
    if (!path) return nil;
    NSData *file = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedAlways error:nil];
    if (!file || file.length < 8) return nil;
    const uint8_t *b = file.bytes;
    if (!(b[0] == 'I' && b[1] == 'I' && b[2] == 0x2A && b[3] == 0)) {
        return nil;
    }
    uint32_t ifd0 = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
    NSMutableDictionary<NSNumber *, NSDictionary *> *out = [NSMutableDictionary dictionary];
    NSMutableSet<NSNumber *> *visited = [NSMutableSet set];
    walkIFD(file, ifd0, out, visited, 0);
    return out;
}

@implementation CIDNGBuilder {
    NSData     *_staticHead;       // header + IFD + extras + (room for) strip pos info
    DNGInfo     _info;
    uint32_t    _width;
    uint32_t    _height;
}

- (instancetype)initWithInfo:(const DNGInfo *)info
                       width:(uint32_t)width
                      height:(uint32_t)height
             templateDngPath:(nullable NSString *)templateDngPath
{
    self = [super init];
    if (!self) return nil;
    _info = *info;
    _width = width;
    _height = height;

    NSDictionary<NSNumber *, NSDictionary *> *tmpl = extractTagsFromTemplate(templateDngPath);
    NSData *(^extractedPayload)(uint16_t) = ^NSData *(uint16_t tagId) {
        NSDictionary *d = tmpl[@(tagId)];
        return d ? d[@"payload"] : nil;
    };
    uint32_t (^extractedCount)(uint16_t) = ^uint32_t(uint16_t tagId) {
        NSDictionary *d = tmpl[@(tagId)];
        return d ? [d[@"count"] unsignedIntValue] : 0;
    };

    // Build the IFD entries.
    uint8_t cfa[4];
    cfa_bytes(info->cfaPattern, cfa);

    // ColorMatrix1/2 + AsShotNeutral: prefer the template DNG's own values.
    // If unavailable, fall back to inverting rgb_cam (approximate).
    NSData *blob_cm1 = extractedPayload(T_ColorMatrix1);
    NSData *blob_cm2 = extractedPayload(T_ColorMatrix2);
    uint32_t cm1_count = blob_cm1 ? extractedCount(T_ColorMatrix1) : 9;
    uint32_t cm2_count = blob_cm2 ? extractedCount(T_ColorMatrix2) : 9;
    if (!blob_cm1 || !blob_cm2) {
        float cam_rgb[9];
        float a[3][3];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) a[r][c] = info->rgb_cam[r][c];
        float det = a[0][0]*(a[1][1]*a[2][2] - a[1][2]*a[2][1])
                  - a[0][1]*(a[1][0]*a[2][2] - a[1][2]*a[2][0])
                  + a[0][2]*(a[1][0]*a[2][1] - a[1][1]*a[2][0]);
        if (fabsf(det) < 1e-9f) {
            for (int i = 0; i < 9; i++) cam_rgb[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        } else {
            float inv[3][3];
            inv[0][0] =  (a[1][1]*a[2][2] - a[1][2]*a[2][1]) / det;
            inv[0][1] = -(a[0][1]*a[2][2] - a[0][2]*a[2][1]) / det;
            inv[0][2] =  (a[0][1]*a[1][2] - a[0][2]*a[1][1]) / det;
            inv[1][0] = -(a[1][0]*a[2][2] - a[1][2]*a[2][0]) / det;
            inv[1][1] =  (a[0][0]*a[2][2] - a[0][2]*a[2][0]) / det;
            inv[1][2] = -(a[0][0]*a[1][2] - a[0][2]*a[1][0]) / det;
            inv[2][0] =  (a[1][0]*a[2][1] - a[1][1]*a[2][0]) / det;
            inv[2][1] = -(a[0][0]*a[2][1] - a[0][1]*a[2][0]) / det;
            inv[2][2] =  (a[0][0]*a[1][1] - a[0][1]*a[1][0]) / det;
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) cam_rgb[r*3+c] = inv[r][c];
        }
        NSData *fallback = blob_srational_matrix(cam_rgb);
        if (!blob_cm1) { blob_cm1 = fallback; cm1_count = 9; }
        if (!blob_cm2) { blob_cm2 = fallback; cm2_count = 9; }
    }
    NSData *blob_fm1 = extractedPayload(T_ForwardMatrix1);
    NSData *blob_fm2 = extractedPayload(T_ForwardMatrix2);
    uint32_t fm1_count = blob_fm1 ? extractedCount(T_ForwardMatrix1) : 0;
    uint32_t fm2_count = blob_fm2 ? extractedCount(T_ForwardMatrix2) : 0;

    NSData *blob_asn = extractedPayload(T_AsShotNeutral);
    uint32_t asn_count = blob_asn ? extractedCount(T_AsShotNeutral) : 3;
    if (!blob_asn) {
        double anR = (info->wbR > 0) ? (1.0 / info->wbR) : 1.0;
        double anG = (info->wbG > 0) ? (1.0 / info->wbG) : 1.0;
        double anB = (info->wbB > 0) ? (1.0 / info->wbB) : 1.0;
        blob_asn = blob_rationals3(anR, anG, anB);
        asn_count = 3;
    }
    uint16_t illum1 = 17, illum2 = 21;
    {
        NSData *ci1 = extractedPayload(T_CalibrationIlluminant1);
        NSData *ci2 = extractedPayload(T_CalibrationIlluminant2);
        if (ci1 && ci1.length >= 2) {
            const uint8_t *p = ci1.bytes; illum1 = p[0] | (p[1] << 8);
        }
        if (ci2 && ci2.length >= 2) {
            const uint8_t *p = ci2.bytes; illum2 = p[0] | (p[1] << 8);
        }
    }
    NSData *blob_make  = extractedPayload(T_Make);
    NSData *blob_model = extractedPayload(T_Model);
    NSData *blob_ucm   = extractedPayload(T_UniqueCameraModel);
    NSData *blob_lcm   = extractedPayload(T_LocalizedCameraModel);
    if (!blob_make)  blob_make  = blob_ascii("NIKON CORPORATION");
    if (!blob_model) blob_model = blob_ascii("NIKON Z 8");
    if (!blob_ucm)   blob_ucm   = blob_ascii("NIKON Z 8");
    if (!blob_lcm)   blob_lcm   = blob_model;
    NSData *blob_sw    = blob_ascii("gpr2prores");
    NSData *blob_xres  = blob_rational(72, 1);
    NSData *blob_yres  = blob_rational(72, 1);
    // CFARepeatPatternDim SHORT[2] = 4 bytes → must be inline.
    uint32_t cfa_dim_inline = (uint32_t)2 | ((uint32_t)2 << 16);
    // DNGVersion / DNGBackwardVersion BYTE[4] = 4 bytes → must be inline.
    uint32_t dngver_inline =
        (uint32_t)1 | ((uint32_t)4 << 8) | ((uint32_t)0 << 16) | ((uint32_t)0 << 24);
    uint32_t dngbwv_inline =
        (uint32_t)1 | ((uint32_t)3 << 8) | ((uint32_t)0 << 16) | ((uint32_t)0 << 24);
    NSData *blob_blvl    = blob_rational(info->blackLevel, 1);

    // CFA pattern is 4 bytes — inline.
    uint32_t cfa_inline =
        (uint32_t)cfa[0] | ((uint32_t)cfa[1] << 8) |
        ((uint32_t)cfa[2] << 16) | ((uint32_t)cfa[3] << 24);

    // Keep tags in ascending order per TIFF spec.
    // We may dynamically include ForwardMatrix1/2 (0xC714/0xC715) when the
    // template provides them; sort positions are >0xC613 (DNGBackwardVersion)
    // and >0xC65A (CalibrationIlluminant1/2) — so they go after AsShotNeutral
    // and before CalibrationIlluminant1.
    IFDEntry entries[] = {
        inline_entry(T_NewSubFileType,            TIFF_LONG, 1, 0),
        inline_entry(T_ImageWidth,                TIFF_LONG, 1, width),
        inline_entry(T_ImageLength,               TIFF_LONG, 1, height),
        inline_entry(T_BitsPerSample,             TIFF_SHORT, 1, 16),
        inline_entry(T_Compression,               TIFF_SHORT, 1, 1),
        inline_entry(T_PhotometricInterpretation, TIFF_SHORT, 1, 32803),
        blob_entry  (T_Make,                      TIFF_ASCII, (uint32_t)blob_make.length, blob_make),
        blob_entry  (T_Model,                     TIFF_ASCII, (uint32_t)blob_model.length, blob_model),
        inline_entry(T_StripOffsets,              TIFF_LONG, 1, 0),  // patched later
        inline_entry(T_SamplesPerPixel,           TIFF_SHORT, 1, 1),
        inline_entry(T_RowsPerStrip,              TIFF_LONG, 1, height),
        inline_entry(T_StripByteCounts,           TIFF_LONG, 1, (uint32_t)((size_t)width*height*2)),
        blob_entry  (T_XResolution,               TIFF_RATIONAL, 1, blob_xres),
        blob_entry  (T_YResolution,               TIFF_RATIONAL, 1, blob_yres),
        inline_entry(T_ResolutionUnit,            TIFF_SHORT, 1, 2),
        blob_entry  (T_Software,                  TIFF_ASCII, (uint32_t)blob_sw.length, blob_sw),
        inline_entry(T_CFARepeatPatternDim,       TIFF_SHORT, 2, cfa_dim_inline),
        inline_entry(T_CFAPattern,                TIFF_BYTE,  4, cfa_inline),
        inline_entry(T_DNGVersion,                TIFF_BYTE,  4, dngver_inline),
        inline_entry(T_DNGBackwardVersion,        TIFF_BYTE,  4, dngbwv_inline),
        blob_entry  (T_UniqueCameraModel,         TIFF_ASCII, (uint32_t)blob_ucm.length, blob_ucm),
        blob_entry  (T_LocalizedCameraModel,      TIFF_ASCII, (uint32_t)blob_lcm.length, blob_lcm),
        blob_entry  (T_BlackLevel,                TIFF_RATIONAL, 1, blob_blvl),
        inline_entry(T_WhiteLevel,                TIFF_LONG, 1, info->whiteLevel),
        blob_entry  (T_ColorMatrix1,              TIFF_SRATIONAL, cm1_count, blob_cm1),
        blob_entry  (T_ColorMatrix2,              TIFF_SRATIONAL, cm2_count, blob_cm2),
        blob_entry  (T_AsShotNeutral,             TIFF_RATIONAL, asn_count, blob_asn),
        inline_entry(T_CalibrationIlluminant1,    TIFF_SHORT, 1, illum1),
        inline_entry(T_CalibrationIlluminant2,    TIFF_SHORT, 1, illum2),
    };
    int Nbase = (int)(sizeof(entries) / sizeof(entries[0]));
    // Append optional ForwardMatrix1/2 (0xC714/0xC715) — sort after illuminants.
    IFDEntry ent[Nbase + 2];
    memcpy(ent, entries, sizeof(entries));
    int N = Nbase;
    if (blob_fm1) ent[N++] = blob_entry(T_ForwardMatrix1, TIFF_SRATIONAL, fm1_count, blob_fm1);
    if (blob_fm2) ent[N++] = blob_entry(T_ForwardMatrix2, TIFF_SRATIONAL, fm2_count, blob_fm2);

    // Note: DNGVersion/DNGBackwardVersion *do* fit in 4 bytes, so they could
    // be inline (BYTE[4]). Either works; for simplicity we used blobs above.

    // Compute byte layout:
    //   off=0:      TIFF header (8 bytes)
    //   off=8:      IFD0 — 2 + N*12 + 4 bytes
    //   off=8+ifd:  extra blobs (each tag's blob appended in tag order)
    //   off=...:    strip data (raw bayer)
    size_t ifdSize = 2 + (size_t)N * 12 + 4;
    size_t headerSize = 8 + ifdSize;

    // Assign offsets for each blob (append-in-order).
    size_t cursor = headerSize;
    uint32_t blobOffsets[64];
    for (int i = 0; i < N; i++) {
        if (ent[i].blob) {
            blobOffsets[i] = (uint32_t)cursor;
            cursor += ent[i].blob.length;
            // Word-align (TIFF requires word boundaries for offsets to type
            // entries with >1 byte size, in practice; we pad to even).
            if (cursor & 1) cursor++;
        } else {
            blobOffsets[i] = 0;
        }
    }
    // Strip data goes here.
    uint32_t stripOffset = (uint32_t)cursor;

    // Patch StripOffsets entry's inline value.
    for (int i = 0; i < N; i++) {
        if (ent[i].tag == T_StripOffsets) {
            ent[i].value = stripOffset;
            break;
        }
    }

    // Now serialize the static head: header + IFD + blobs.
    NSMutableData *head = [NSMutableData dataWithCapacity:stripOffset];

    // TIFF header: "II*\0", then offset to IFD0 = 8.
    [head appendBytes:"II*\0" length:4];
    put_u32(head, 8);

    // IFD0: numEntries, entries, nextIFD=0.
    put_u16(head, (uint16_t)N);
    for (int i = 0; i < N; i++) {
        put_u16(head, ent[i].tag);
        put_u16(head, ent[i].type);
        put_u32(head, ent[i].count);
        if (ent[i].blob) {
            put_u32(head, blobOffsets[i]);
        } else {
            // inline value (4-byte LE) — caller pre-packed the bytes.
            put_u32(head, ent[i].value);
        }
    }
    put_u32(head, 0);  // next IFD

    // Append blobs in tag order, padding each to even byte boundary.
    for (int i = 0; i < N; i++) {
        if (ent[i].blob) {
            NSCAssert(head.length == blobOffsets[i],
                      @"blob offset mismatch tag=0x%x (have %lu, want %u)",
                      ent[i].tag, (unsigned long)head.length, blobOffsets[i]);
            [head appendData:ent[i].blob];
            if (head.length & 1) {
                uint8_t zero = 0;
                [head appendBytes:&zero length:1];
            }
        }
    }

    NSCAssert(head.length == stripOffset, @"strip offset mismatch (have %lu, want %u)",
              (unsigned long)head.length, stripOffset);

    _staticHead = [head copy];
    fprintf(stderr, "CIDNGBuilder: %s template metadata, %d-tag head, %lu bytes (strip at %u)\n",
            tmpl ? "using" : "fallback (no)",
            N, (unsigned long)_staticHead.length, stripOffset);
    return self;
}

- (NSData *)dngFromBayerBytes:(const void *)bayerBytes length:(size_t)length {
    if (length != (size_t)_width * _height * 2) {
        fprintf(stderr, "CIDNGBuilder: bayer length %zu != %ux%ux2 = %zu\n",
                length, _width, _height, (size_t)_width * _height * 2);
        return nil;
    }
    // Concatenate: static head + strip bytes.
    NSMutableData *out = [NSMutableData dataWithCapacity:_staticHead.length + length];
    [out appendData:_staticHead];
    [out appendBytes:bayerBytes length:length];
    return out;
}

@end
