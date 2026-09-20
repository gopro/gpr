/*! @file gpr_conversion_tests.cpp
 *
 *  @brief Exhaustive conversion tests for the GPR SDK.
 *
 *  Exercises every public gpr_convert_* entry point over the bundled sample
 *  files and validates the produced output (container magic, parseable
 *  metadata, dimensions, size invariants and round-trip equality) rather than
 *  merely checking that a call returned. The suite links the SDK directly and
 *  drives it through its C API, the same way gpr_tools does. gpr_tools' own
 *  CLI conversion layer (dng_convert_main) is covered as well, in particular
 *  the consolidated --preview argument.
 *
 *  Each test case runs in an isolated child process (where the platform
 *  supports fork) so that a crash in one conversion is reported as a failure
 *  and the remaining cases still run. Test output goes to stdout; the SDK's own
 *  stderr logging is silenced inside child processes for readability.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#define GPR_TESTS_HAVE_FORK 1
#else
#define GPR_TESTS_HAVE_FORK 0
#endif

#include "gpr.h"
#include "gpr_flat_write_stream.h"
#include "gpr_lens_profiles.h"
#include "main_c.h"              // gpr_tools' CLI conversion layer (dng_convert_main)
#include "program_options_lite.h" // gpr_tools' command-line scanner

#if !defined(GPR_TESTS_DATA_DIR)
#define GPR_TESTS_DATA_DIR "data/samples"
#endif

#if GPR_JPEG_AVAILABLE
// tiny_jpeg is compiled as C; declare the one entry point we use with C linkage
// instead of pulling the (non-extern-"C") header into this C++ translation unit.
extern "C" int tje_encode_with_func( void (*func)(void*, void*, int), void* context,
                                      const int quality, const int width, const int height,
                                      const int num_components, const unsigned char* src_data );
#endif

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static gpr_allocator g_alloc = { malloc, free };

// Per-case counters (within a single, possibly forked, case body).
static int g_checks   = 0;
static int g_failures = 0;

// Suite-level counters (parent process).
static int g_cases         = 0;
static int g_cases_failed  = 0;
static int g_cases_crashed = 0;
static int g_xfail         = 0;   // known-broken cases that failed/crashed as expected
static int g_xpass         = 0;   // known-broken cases that unexpectedly passed (promote them!)

static void check( bool cond, const char* msg )
{
    g_checks++;
    if( !cond )
    {
        g_failures++;
        std::fprintf( stdout, "      [CHECK FAIL] %s\n", msg );
    }
}

// RAII wrapper around gpr_buffer (allocated by the SDK via malloc).
struct Buffer
{
    gpr_buffer b;
    Buffer()            { b.buffer = NULL; b.size = 0; }
    ~Buffer()           { release(); }
    void release()      { if( b.buffer ) free( b.buffer ); b.buffer = NULL; b.size = 0; }
    bool valid() const  { return b.buffer != NULL && b.size > 0; }
private:
    Buffer( const Buffer& );
    Buffer& operator=( const Buffer& );
};

struct RgbBuffer
{
    gpr_rgb_buffer b;
    RgbBuffer()  { b.buffer = NULL; b.size = 0; b.width = 0; b.height = 0; }
    ~RgbBuffer() { if( b.buffer ) free( b.buffer ); }
private:
    RgbBuffer( const RgbBuffer& );
    RgbBuffer& operator=( const RgbBuffer& );
};

// ---------------------------------------------------------------------------
// Case runner with crash isolation
// ---------------------------------------------------------------------------

// Runs one named test case. The body performs conversions and calls check().
// On platforms with fork, the body runs in a child process so a segfault/abort
// is reported as a crashed case instead of taking down the whole suite.
//
// expect_fail marks a case that exercises a currently-broken SDK path: a
// failure/crash is recorded as an expected "known issue" (xfail) and does not
// fail the suite, while an unexpected pass (xpass) is flagged so the marker can
// be removed once the bug is fixed.
static void run_case( const std::string& name, std::function<void()> body, bool expect_fail = false )
{
    g_cases++;
    std::fprintf( stdout, "  - %s%s\n", name.c_str(), expect_fail ? "  [known issue]" : "" );
    std::fflush( stdout );

    bool crashed   = false;
    bool failed    = false;
    int  crash_sig = 0;

#if GPR_TESTS_HAVE_FORK
    pid_t pid = fork();
    if( pid == 0 )
    {
        // Child: silence the SDK's stderr logging; keep stdout for test output.
        if( freopen( "/dev/null", "w", stderr ) == NULL ) { /* ignore */ }

        g_checks = 0;
        g_failures = 0;
        try { body(); }
        catch( ... )
        {
            g_failures++;
            std::fprintf( stdout, "      [CHECK FAIL] uncaught exception\n" );
        }
        std::fflush( stdout );
        _exit( g_failures == 0 ? 0 : 1 );
    }

    int status = 0;
    waitpid( pid, &status, 0 );
    if( WIFSIGNALED( status ) ) { crashed = true; crash_sig = WTERMSIG( status ); }
    else                        { failed  = ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 ); }
#else
    g_checks = 0;
    g_failures = 0;
    try { body(); }
    catch( ... ) { g_failures++; std::fprintf( stdout, "      [CHECK FAIL] uncaught exception\n" ); }
    failed = ( g_failures != 0 );
#endif

    if( expect_fail )
    {
        if( crashed || failed ) { g_xfail++; std::fprintf( stdout, "    => XFAIL (known issue)%s\n", crashed ? " (crash)" : "" ); }
        else                    { g_xpass++; std::fprintf( stdout, "    => XPASS (unexpected pass; remove known-issue marker)\n" ); }
    }
    else if( crashed ) { g_cases_crashed++; std::fprintf( stdout, "    => CRASHED (signal %d)\n", crash_sig ); }
    else if( failed )  { g_cases_failed++;  std::fprintf( stdout, "    => FAILED\n" ); }
}

// ---------------------------------------------------------------------------
// File IO
// ---------------------------------------------------------------------------

static bool load_file( const char* path, Buffer& out )
{
    FILE* f = fopen( path, "rb" );
    if( !f ) return false;
    fseek( f, 0, SEEK_END );
    long sz = ftell( f );
    fseek( f, 0, SEEK_SET );
    if( sz <= 0 ) { fclose( f ); return false; }
    out.b.buffer = malloc( (size_t)sz );
    out.b.size   = (size_t)sz;
    size_t rd = fread( out.b.buffer, 1, (size_t)sz, f );
    fclose( f );
    return rd == (size_t)sz;
}

// ---------------------------------------------------------------------------
// Validators
// ---------------------------------------------------------------------------

static bool is_tiff_container( const gpr_buffer& buf )
{
    if( !buf.buffer || buf.size < 8 ) return false;
    const unsigned char* p = (const unsigned char*)buf.buffer;
    bool little = ( p[0] == 'I' && p[1] == 'I' && p[2] == 0x2A && p[3] == 0x00 );
    bool big    = ( p[0] == 'M' && p[1] == 'M' && p[2] == 0x00 && p[3] == 0x2A );
    return little || big;
}

// Parse a DNG/GPR buffer's metadata; returns dimensions via out params.
static bool parse_dims( const gpr_buffer& in, unsigned int& w, unsigned int& h )
{
    gpr_parameters params;
    gpr_parameters_set_defaults( &params );

    gpr_buffer tmp = in; // gpr_parameters_parse_dng takes a non-const pointer; share memory
    bool ok = gpr_parameters_parse_dng( &g_alloc, &tmp, &params );

    if( ok ) { w = params.input_width; h = params.input_height; }

    gpr_parameters_destroy( &params, g_alloc.Free );
    return ok;
}

static unsigned int tiff_u16( const unsigned char* p, bool le )
{
    return le ? ( p[0] | (p[1] << 8) ) : ( (p[0] << 8) | p[1] );
}
static unsigned int tiff_u32( const unsigned char* p, bool le )
{
    return le ? ( p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24) )
              : ( ((unsigned)p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3] );
}

// Returns true if any IFD (including SubIFDs) sets Compression (259) to ccVc5 (9).
// An independent ground truth (reads the TIFF tag directly) that the test cross-checks
// against the public gpr_check_vc5() at every call site.
static bool tiff_has_vc5_compression( const gpr_buffer& buf )
{
    if( !buf.buffer || buf.size < 8 ) return false;
    const unsigned char* d = (const unsigned char*)buf.buffer;
    const size_t n = buf.size;
    bool le = ( d[0] == 'I' );

    std::vector<unsigned int> ifd_offsets;
    ifd_offsets.push_back( tiff_u32( d + 4, le ) );

    for( size_t idx = 0; idx < ifd_offsets.size(); ++idx )
    {
        unsigned int off = ifd_offsets[idx];
        if( off == 0 || (size_t)off + 2 > n ) continue;

        unsigned int count = tiff_u16( d + off, le );
        size_t entry = (size_t)off + 2;
        if( entry + (size_t)count * 12 > n ) continue;

        for( unsigned int e = 0; e < count; ++e, entry += 12 )
        {
            unsigned int tag  = tiff_u16( d + entry, le );
            unsigned int type = tiff_u16( d + entry + 2, le );
            unsigned int cnt  = tiff_u32( d + entry + 4, le );

            if( tag == 259 /* Compression */ && type == 3 /* SHORT */ )
            {
                if( tiff_u16( d + entry + 8, le ) == 9 /* ccVc5 */ ) return true;
            }
            else if( tag == 330 /* SubIFDs */ )
            {
                if( cnt == 1 )
                    ifd_offsets.push_back( tiff_u32( d + entry + 8, le ) );
                else
                {
                    size_t p = tiff_u32( d + entry + 8, le );
                    for( unsigned int k = 0; k < cnt && p + 4 <= n; ++k, p += 4 )
                        ifd_offsets.push_back( tiff_u32( d + p, le ) );
                }
            }
        }
    }
    return false;
}

// Locates an embedded JPEG preview: any IFD (IFD0, chained IFDs, or SubIFDs) whose
// Compression (259) is 7 (JPEG). The raw image itself is never JPEG compressed (vc5 uses
// code 9, uncompressed DNG uses 1), so a JPEG IFD can only be a preview. When found and
// the IFD holds a single strip, the strip's bounds are returned so callers can compare
// the embedded bytes against the JPEG they supplied.
// width/height are the preview IFD's own ImageWidth/ImageLength - what a reader sizes the
// thumbnail from, so worth reading back rather than trusting the JPEG inside the strip.
static bool tiff_find_jpeg_preview( const gpr_buffer& buf, size_t* strip_off, size_t* strip_size,
                                    unsigned int* width = NULL, unsigned int* height = NULL )
{
    if( !buf.buffer || buf.size < 8 ) return false;
    const unsigned char* d = (const unsigned char*)buf.buffer;
    const size_t n = buf.size;
    bool le = ( d[0] == 'I' );

    std::vector<unsigned int> ifd_offsets;
    ifd_offsets.push_back( tiff_u32( d + 4, le ) );

    // idx < 64 bounds the walk in case a malformed next-IFD pointer forms a cycle.
    for( size_t idx = 0; idx < ifd_offsets.size() && idx < 64; ++idx )
    {
        unsigned int off = ifd_offsets[idx];
        if( off == 0 || (size_t)off + 2 > n ) continue;

        unsigned int count = tiff_u16( d + off, le );
        size_t entry = (size_t)off + 2;
        if( entry + (size_t)count * 12 + 4 > n ) continue;

        bool         is_jpeg = false;
        size_t       offsets = 0, byte_counts = 0;
        unsigned int image_width = 0, image_height = 0;

        for( unsigned int e = 0; e < count; ++e, entry += 12 )
        {
            unsigned int tag  = tiff_u16( d + entry, le );
            unsigned int type = tiff_u16( d + entry + 2, le );
            unsigned int cnt  = tiff_u32( d + entry + 4, le );
            unsigned int val  = ( type == 3 /* SHORT */ ) ? tiff_u16( d + entry + 8, le )
                                                          : tiff_u32( d + entry + 8, le );

            if( tag == 259 /* Compression */ && val == 7 /* JPEG */ )
                is_jpeg = true;
            else if( tag == 256 /* ImageWidth */ )
                image_width = val;
            else if( tag == 257 /* ImageLength */ )
                image_height = val;
            else if( tag == 273 /* StripOffsets */ && cnt == 1 )
                offsets = val;
            else if( tag == 279 /* StripByteCounts */ && cnt == 1 )
                byte_counts = val;
            else if( tag == 330 /* SubIFDs */ )
            {
                if( cnt == 1 )
                    ifd_offsets.push_back( val );
                else
                {
                    size_t p = tiff_u32( d + entry + 8, le );
                    for( unsigned int k = 0; k < cnt && p + 4 <= n; ++k, p += 4 )
                        ifd_offsets.push_back( tiff_u32( d + p, le ) );
                }
            }
        }

        if( is_jpeg )
        {
            if( strip_off )  *strip_off  = offsets;
            if( strip_size ) *strip_size = byte_counts;
            if( width )      *width      = image_width;
            if( height )     *height     = image_height;
            return true;
        }

        // Additional previews are written as IFDs chained after IFD0; follow the chain.
        ifd_offsets.push_back( tiff_u32( d + entry, le ) );
    }
    return false;
}

// One GainMap opcode extracted from OpcodeList2, as written to the file.
struct GainMapEntry
{
    unsigned int         area_top, area_left;   // CFA plane origin
    const unsigned char* gains;                 // raw gain samples (big-endian real32)
    size_t               gain_size;             // in bytes
};

// Big-endian reads: OpcodeList payloads are always big-endian per the DNG spec,
// regardless of the TIFF container's byte order.
static unsigned int be_u32( const unsigned char* p )
{
    return ( (unsigned)p[0]<<24 ) | ( p[1]<<16 ) | ( p[2]<<8 ) | p[3];
}

// Extracts the GainMap opcodes from OpcodeList2 (tag 51009, searched across all
// IFDs/SubIFDs). Returns the number found (up to max_entries); entries point into buf.
static int tiff_parse_gain_maps( const gpr_buffer& buf, GainMapEntry* entries, int max_entries )
{
    if( !buf.buffer || buf.size < 8 ) return 0;
    const unsigned char* d = (const unsigned char*)buf.buffer;
    const size_t n = buf.size;
    bool le = ( d[0] == 'I' );

    const unsigned char* list = NULL;
    size_t list_size = 0;

    std::vector<unsigned int> ifd_offsets;
    ifd_offsets.push_back( tiff_u32( d + 4, le ) );

    for( size_t idx = 0; idx < ifd_offsets.size() && idx < 64 && !list; ++idx )
    {
        unsigned int off = ifd_offsets[idx];
        if( off == 0 || (size_t)off + 2 > n ) continue;

        unsigned int count = tiff_u16( d + off, le );
        size_t entry = (size_t)off + 2;
        if( entry + (size_t)count * 12 > n ) continue;

        for( unsigned int e = 0; e < count; ++e, entry += 12 )
        {
            unsigned int tag = tiff_u16( d + entry, le );
            unsigned int cnt = tiff_u32( d + entry + 4, le );
            unsigned int val = tiff_u32( d + entry + 8, le );

            if( tag == 51009 /* OpcodeList2 */ && cnt > 4 && (size_t)val + cnt <= n )
            {
                list = d + val;
                list_size = cnt;
                break;
            }
            else if( tag == 330 /* SubIFDs */ )
            {
                if( cnt == 1 )
                    ifd_offsets.push_back( val );
                else
                {
                    size_t p = val;
                    for( unsigned int k = 0; k < cnt && p + 4 <= n; ++k, p += 4 )
                        ifd_offsets.push_back( tiff_u32( d + p, le ) );
                }
            }
        }
    }

    if( !list || list_size < 4 ) return 0;

    // Payload: opcode count, then per opcode: id, version, flags, byte size, data.
    int found = 0;
    unsigned int opcode_count = be_u32( list );
    size_t p = 4;
    for( unsigned int i = 0; i < opcode_count && p + 16 <= list_size; ++i )
    {
        unsigned int id   = be_u32( list + p );
        unsigned int size = be_u32( list + p + 12 );
        p += 16;
        if( p + size > list_size ) break;

        if( id == 9 /* GainMap */ && found < max_entries )
        {
            const unsigned char* data = list + p;
            // Data: area spec (top, left, bottom, right, plane, planes, rowPitch,
            // colPitch; 32 bytes), then map header (pointsV, pointsH, spacing/origin
            // real64 x4, mapPlanes; 44 bytes), then pointsV*pointsH*mapPlanes real32 gains.
            if( size >= 76 )
            {
                unsigned int pv = be_u32( data + 32 );
                unsigned int ph = be_u32( data + 36 );
                unsigned int mp = be_u32( data + 72 );
                size_t gain_size = (size_t)pv * ph * mp * 4;
                if( 76 + gain_size <= size )
                {
                    entries[found].area_top  = be_u32( data );
                    entries[found].area_left = be_u32( data + 4 );
                    entries[found].gains     = data + 76;
                    entries[found].gain_size = gain_size;
                    found++;
                }
            }
        }
        p += size;
    }
    return found;
}

// The output's embedded preview against the exact bytes it should be carrying. Shared by every
// case that hands the writer a JPEG or expects one to survive a conversion.
static void check_embedded_preview( const Buffer& out, const unsigned char* expect,
                                    size_t expect_size, const char* msg )
{
    size_t off = 0, sz = 0;
    const bool found = tiff_find_jpeg_preview( out.b, &off, &sz );

    check( found, "output contains a JPEG preview IFD" );

    if( found )
        check( sz == expect_size && off + sz <= out.b.size &&
               std::memcmp( (const unsigned char*)out.b.buffer + off, expect, sz ) == 0, msg );
}

static void validate_dng_like( const Buffer& out, unsigned int expect_w, unsigned int expect_h, bool expect_vc5 )
{
    check( out.valid(), "output non-empty" );
    if( !out.valid() ) return;

    check( is_tiff_container( out.b ), "is TIFF container" );

    unsigned int w = 0, h = 0;
    bool parsed = parse_dims( out.b, w, h );
    check( parsed, "metadata parses" );
    if( parsed )
    {
        check( w == expect_w, "width matches source" );
        check( h == expect_h, "height matches source" );
    }

    check( tiff_has_vc5_compression( out.b ) == expect_vc5, "vc5 compression flag as expected (TIFF tag)" );

    // gpr_check_vc5's declaration in gpr.h previously did not match its definition (mismatched
    // signature and C/C++ linkage), so it could not be linked from a C consumer. Now that it is
    // fixed, cross-check it against the independent TIFF-tag reading above.
    gpr_buffer tmp = out.b;
    check( gpr_check_vc5( &g_alloc, &tmp ) == expect_vc5, "vc5 compression flag as expected (gpr_check_vc5)" );
}

static void validate_raw( const Buffer& out, unsigned int w, unsigned int h )
{
    check( out.valid(), "output non-empty" );
    if( !out.valid() ) return;
    check( out.b.size == (size_t)w * h * 2, "size == width*height*2" );
}

static void validate_rgb( const RgbBuffer& rgb, int bits )
{
    check( rgb.b.buffer != NULL && rgb.b.size > 0, "output non-empty" );
    if( rgb.b.buffer == NULL ) return;
    check( rgb.b.width > 0 && rgb.b.height > 0, "width/height > 0" );
    size_t channels_bytes = ( bits == 8 ) ? 3 : 6;
    check( rgb.b.size == rgb.b.width * rgb.b.height * channels_bytes, "size == width*height*channels" );
}

#if GPR_JPEG_AVAILABLE
static void jpg_sink( void* context, void* data, int size )
{
    std::vector<unsigned char>* out = (std::vector<unsigned char>*)context;
    out->insert( out->end(), (unsigned char*)data, (unsigned char*)data + size );
}

// Encodes a small RGB gradient as a baseline JPEG, used as a caller-supplied preview image.
// The size is a parameter so a case can tell two of these apart.
static bool make_test_jpeg( std::vector<unsigned char>& jpg, int w = 32, int h = 24 )
{
    std::vector<unsigned char> rgb( (size_t)w * h * 3 );
    for( int y = 0; y < h; ++y )
        for( int x = 0; x < w; ++x )
        {
            unsigned char* p = &rgb[ ( (size_t)y * w + x ) * 3 ];
            p[0] = (unsigned char)( x * 8 );
            p[1] = (unsigned char)( y * 10 );
            p[2] = 0x80;
        }
    return tje_encode_with_func( jpg_sink, &jpg, 2, w, h, 3, rgb.data() ) != 0;
}
#endif

// ---------------------------------------------------------------------------
// Helpers for the gpr_tools --preview cases
// ---------------------------------------------------------------------------

// Reads the pixel dimensions out of a JPEG stream's SOF header. Baseline
// streams only (which is what the SDK embeds); returns false on anything else.
static bool jpeg_sof_dims( const unsigned char* p, size_t n, unsigned int* w, unsigned int* h )
{
    if( n < 4 || p[0] != 0xFF || p[1] != 0xD8 ) return false;
    size_t i = 2;
    while( i + 4 <= n )
    {
        if( p[i] != 0xFF ) return false;
        const unsigned char m = p[i+1];
        if( m == 0xD8 || m == 0x01 || ( m >= 0xD0 && m <= 0xD7 ) ) { i += 2; continue; }
        if( m == 0xDA ) return false;   // entropy-coded data reached without a SOF
        if( m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC )
        {
            if( i + 9 > n ) return false;
            *h = ( (unsigned int)p[i+5] << 8 ) | p[i+6];
            *w = ( (unsigned int)p[i+7] << 8 ) | p[i+8];
            return true;
        }
        i += 2 + ( ( (size_t)p[i+2] << 8 ) | p[i+3] );
    }
    return false;
}

// Byte offset where a baseline JPEG's entropy-coded scan data begins (just past
// the SOS header), or 0 if malformed. Everything before that offset is tables and
// headers, fully determined by the encoder settings and image dimensions.
static size_t jpeg_scan_data_offset( const unsigned char* p, size_t n )
{
    if( n < 4 || p[0] != 0xFF || p[1] != 0xD8 ) return 0;
    size_t i = 2;
    while( i + 4 <= n )
    {
        if( p[i] != 0xFF ) return 0;
        const unsigned char m = p[i+1];
        if( m == 0xD8 || m == 0x01 || ( m >= 0xD0 && m <= 0xD7 ) ) { i += 2; continue; }
        const size_t len = ( (size_t)p[i+2] << 8 ) | p[i+3];
        if( len < 2 ) return 0;
        if( m == 0xDA )
        {
            const size_t scan = i + 2 + len;
            return ( scan < n ) ? scan : 0;
        }
        i += 2 + len;
    }
    return 0;
}

static bool save_file( const char* path, const void* data, size_t size )
{
    FILE* f = fopen( path, "wb" );
    if( !f ) return false;
    const size_t wr = fwrite( data, 1, size, f );
    fclose( f );
    return wr == size;
}

// A conversion that failed must not have left an output file behind; a stray one is removed
// so the next case starts clean.
static void check_no_output( const std::string& path )
{
    FILE* f = fopen( path.c_str(), "rb" );
    check( f == NULL, "no output file written" );
    if( f ) { fclose( f ); std::remove( path.c_str() ); }
}

// Scratch-file path for one case. Cases clean up after themselves; the pid
// keeps two suites running against the same temp dir out of each other's way.
static std::string scratch_path( const char* name )
{
    const char* dir = getenv( "TMPDIR" );
    if( !dir || !dir[0] ) dir = getenv( "TEMP" );
    std::string p = ( dir && dir[0] ) ? dir : ".";
    if( p[p.size()-1] != '/' && p[p.size()-1] != '\\' ) p += '/';
#if GPR_TESTS_HAVE_FORK
    char prefix[48];
    std::snprintf( prefix, sizeof(prefix), "gpr_tests_%ld_", (long)getpid() );
    p += prefix;
#else
    p += "gpr_tests_";
#endif
    return p + name;
}

// dng_convert_params as gpr_tools' main() builds them: every string present
// (dng_convert_main dereferences them unconditionally), only the --preview
// value varying per case.
static dng_convert_params preview_cli_params( const char* input, const char* output, const char* preview )
{
    dng_convert_params p;
    std::memset( &p, 0, sizeof(p) );
    p.input_file_path     = input;
    p.input_pixel_format  = "";
    p.output_file_path    = output;
    p.output_format       = "";
    p.metadata_file_path  = "";
    p.gpmf_file_path      = "";
    p.rgb_file_resolution = "";
    p.rgb_file_bits       = 8;
    p.jpg_quality         = 2;
    p.preview             = preview;
    return p;
}

// ---------------------------------------------------------------------------
// Shared per-sample state (set up in the parent; inherited by forked children)
// ---------------------------------------------------------------------------

static Buffer        g_gpr;       // the source sample
static std::string   g_sample_path; // path the source sample was loaded from
static gpr_parameters g_params;   // parsed metadata for the source
static unsigned int  g_W = 0, g_H = 0;

// Helpers that re-derive an intermediate format from the source GPR, so each
// case is self-contained and independently crash-isolated.
static bool make_raw( Buffer& raw ) { return gpr_convert_gpr_to_raw( &g_alloc, &g_gpr.b, &raw.b); }
static bool make_dng( Buffer& dng ) { return gpr_convert_gpr_to_dng( &g_alloc, &g_params, &g_gpr.b, &dng.b); }
static bool make_vc5( Buffer& vc5 ) { return gpr_convert_gpr_to_vc5( &g_alloc, &g_gpr.b, &vc5.b); }

// ---------------------------------------------------------------------------
// The conversion matrix for one sample
// ---------------------------------------------------------------------------

static void run_sample( const std::string& sample_path )
{
    const std::string name = sample_path.substr( sample_path.find_last_of('/') + 1 );
    std::fprintf( stdout, "\n== %s ==\n", name.c_str() );

    g_gpr.release();
    g_sample_path = sample_path;
    if( !load_file( sample_path.c_str(), g_gpr ) )
    {
        run_case( name + ": load sample", []{ check( false, "sample file could not be read" ); } );
        return;
    }

    // Parse once up front (also a tested case); children inherit g_params.
    gpr_parameters_set_defaults( &g_params );
    {
        gpr_buffer in = g_gpr.b;
        if( !gpr_parameters_parse_dng( &g_alloc, &in, &g_params ) )
        {
            run_case( name + ": parse metadata", []{ check( false, "gpr_parameters_parse_dng failed" ); } );
            return;
        }
    }
    g_W = g_params.input_width;
    g_H = g_params.input_height;

    run_case( name + ": gpr_parameters_parse_dng", []{
        check( g_W > 0 && g_H > 0, "input dimensions positive" );
        check( g_params.tuning_info.dgain_saturation_level.level_red >
               g_params.tuning_info.static_black_level.r_black, "white level > black level" );
        check( g_params.tuning_info.wb_gains.r_gain > 0 &&
               g_params.tuning_info.wb_gains.g_gain > 0 &&
               g_params.tuning_info.wb_gains.b_gain > 0, "white-balance gains positive" );
        check( g_params.tuning_info.pixel_format >= PIXEL_FORMAT_RGGB_12 &&
               g_params.tuning_info.pixel_format <= PIXEL_FORMAT_BGGR_14, "pixel format in range" );
        check( tiff_has_vc5_compression( g_gpr.b ), "source GPR detected as VC5 (TIFF tag)" );
        gpr_buffer tmp = g_gpr.b;
        check( gpr_check_vc5( &g_alloc, &tmp ), "source GPR detected as VC5 (gpr_check_vc5)" );
    });

    run_case( name + ": gpr_parameters_parse_dng_file", []{
        gpr_parameters params;
        gpr_parameters_set_defaults( &params );
        check( gpr_parameters_parse_dng_file( &g_alloc, g_sample_path.c_str(), &params ),
               "file-based parse returns true" );
        check( params.input_width == g_W && params.input_height == g_H,
               "dimensions match buffer-based parse" );
        check( params.tuning_info.pixel_format == g_params.tuning_info.pixel_format,
               "pixel format matches buffer-based parse" );
        gpr_parameters_destroy( &params, g_alloc.Free );

        gpr_parameters_set_defaults( &params );
        check( !gpr_parameters_parse_dng_file( &g_alloc, "no/such/file.GPR", &params ),
               "nonexistent path returns false" );
        gpr_parameters_destroy( &params, g_alloc.Free );
    });

    // -------- decode paths (GPR -> *) ------------------------------------
    run_case( name + ": gpr_to_raw", []{
        Buffer raw; check( make_raw( raw ), "conversion returns true" );
        validate_raw( raw, g_W, g_H );
    });

    run_case( name + ": gpr_to_dng", []{
        Buffer dng; check( make_dng( dng ), "conversion returns true" );
        validate_dng_like( dng, g_W, g_H, /*vc5=*/false );
    });

    // -------- warp (OpcodeList3 WarpRectilinear) fidelity ----------------
    // HERO5 and FUSION embed a camera-original WarpRectilinear (chromatic
    // aberration correction: per-plane linear k0 only); later cameras carry no
    // OpcodeList3 at all. Either way the parsed warp must survive a conversion
    // to DNG and to GPR unchanged (the legacy code reduced it to two scalars,
    // dropping the green-plane coefficient, center and flags).
    run_case( name + ": warp round-trips through gpr_to_dng / gpr_to_gpr", []{
        const gpr_warp_rectilinear& src = g_params.tuning_info.warp;

        const std::string base = g_sample_path.substr( g_sample_path.find_last_of('/') + 1 );
        const bool expect_warp = ( base == "GOPR2657.GPR" ||
                                   base == "GPFR7066.GPR" ||
                                   base == "GPBK7066.GPR" );

        if( expect_warp )
        {
            check( gpr_warp_rectilinear_is_valid( &src ), "source warp parsed" );
            check( src.planes == 3, "source warp has 3 planes" );
            check( gpr_warp_rectilinear_is_ca_only( &src ), "camera-original warp is CA-only" );
            check( src.radial[0][0] > 0.9 && src.radial[0][0] < 1.1, "red k0 plausible" );
            check( src.radial[1][0] > 0.9 && src.radial[1][0] < 1.1, "green k0 plausible" );
            check( src.center_x >= 0.0 && src.center_x <= 1.0 &&
                   src.center_y >= 0.0 && src.center_y <= 1.0, "center normalized" );
        }
        else
        {
            check( !gpr_warp_rectilinear_is_valid( &src ), "source has no warp" );
        }

        // gpr_warp_rectilinear is padding-free (two uint32 then doubles) and both
        // sides start from gpr_parameters_set_defaults, so memcmp is exact.
        Buffer dng;
        check( make_dng( dng ), "gpr_to_dng returns true" );
        {
            gpr_parameters out;
            gpr_parameters_set_defaults( &out );
            gpr_buffer tmp = dng.b;
            check( gpr_parameters_parse_dng( &g_alloc, &tmp, &out ), "output DNG parses" );
            check( std::memcmp( &out.tuning_info.warp, &src, sizeof(src) ) == 0,
                   "warp identical after GPR->DNG" );
            gpr_parameters_destroy( &out, g_alloc.Free );
        }

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &g_params, &g_gpr.b, &gpr2.b ),
               "gpr_to_gpr returns true" );
        {
            gpr_parameters out;
            gpr_parameters_set_defaults( &out );
            gpr_buffer tmp = gpr2.b;
            check( gpr_parameters_parse_dng( &g_alloc, &tmp, &out ), "output GPR parses" );
            check( std::memcmp( &out.tuning_info.warp, &src, sizeof(src) ) == 0,
                   "warp identical after GPR->GPR" );
            gpr_parameters_destroy( &out, g_alloc.Free );
        }
    });

    // Apple ImageIO/CIRAWFilter drops the entire gain map unless the two green CFA
    // planes' GainMap opcodes carry byte-identical gains (GoPro's factory maps differ
    // between the greens by float16 quantization noise), leaving uncorrected dark
    // corners in Preview/Photos. The DNG writer forces the greens equal; assert that
    // holds in the written OpcodeList2.
    //
    // The greens are identified the way the writer identifies them - by each opcode's
    // own area spec, whose (top,left) parity names its cell in the 2x2 CFA tile - and
    // never by position in the list. An earlier version of both this test and the writer
    // assumed a fixed index pair per pixel format; on HERO13 GBRG files, whose opcodes
    // are stored R,G,G,B, that pair named red and blue, so the writer equalized those
    // two and the test happily confirmed it while the actual greens stayed mismatched.
    if( g_params.tuning_info.gain_map.size > 0 )
    {
        run_case( name + ": gpr_to_dng (green GainMaps byte-identical)", []{
            Buffer dng; check( make_dng( dng ), "conversion returns true" );

            GainMapEntry gm[4];
            check( tiff_parse_gain_maps( dng.b, gm, 4 ) == 4, "OpcodeList2 has 4 GainMap opcodes" );

            bool greens_on_main_diagonal;
            switch( g_params.tuning_info.pixel_format )
            {
                case PIXEL_FORMAT_GBRG_12:      // G B / R G
                case PIXEL_FORMAT_GBRG_12P:
                    greens_on_main_diagonal = true;
                    break;
                default:                        // RGGB: R G / G B,  BGGR: B G / G R
                    greens_on_main_diagonal = false;
                    break;
            }

            int greens[4], others[4];
            int green_count = 0, other_count = 0;

            for( int i = 0; i < 4; ++i )
            {
                if( ( ( gm[i].area_top & 1 ) == ( gm[i].area_left & 1 ) ) == greens_on_main_diagonal )
                    greens[green_count++] = i;
                else
                    others[other_count++] = i;
            }

            check( green_count == 2, "area specs name exactly two green planes" );
            if( green_count != 2 ) return;

            const GainMapEntry& ga = gm[greens[0]];
            const GainMapEntry& gb = gm[greens[1]];

            check( ga.gain_size > 0 &&
                   ga.gain_size == gb.gain_size &&
                   std::memcmp( ga.gains, gb.gains, ga.gain_size ) == 0,
                   "green gain arrays byte-identical" );
            check( ga.area_top != gb.area_top || ga.area_left != gb.area_left,
                   "green planes keep distinct CFA origins" );

            // The non-green planes must come through untouched, as must the green the
            // gains are copied from; only the second green is rewritten. Overwriting a
            // red or blue plane is the signature of the old index guess, and it corrupts
            // that plane's shading for any reader that does honour the opcode list.
            // Compared against the source rather than against each other: HERO6/7/9 ship
            // near-flat maps whose red and blue planes are legitimately byte-identical.
            GainMapEntry src[4];
            check( tiff_parse_gain_maps( g_gpr.b, src, 4 ) == 4, "source GPR has 4 GainMap opcodes" );

            int untouched[3];
            int untouched_count = 0;
            for( int k = 0; k < other_count; ++k ) untouched[untouched_count++] = others[k];
            untouched[untouched_count++] = greens[0];

            for( int k = 0; k < untouched_count; ++k )
            {
                const GainMapEntry& out = gm[untouched[k]];
                const GainMapEntry& in  = src[untouched[k]];

                check( out.area_top == in.area_top && out.area_left == in.area_left,
                       "unrewritten plane keeps its source CFA origin" );
                check( out.gain_size == in.gain_size &&
                       std::memcmp( out.gains, in.gains, out.gain_size ) == 0,
                       "unrewritten plane keeps its source gains" );
            }
        });
    }

    run_case( name + ": gpr_to_vc5", []{
        Buffer vc5; check( make_vc5( vc5 ), "conversion returns true" );
        check( vc5.valid(), "output non-empty" );
    });

    run_case( name + ": gpr_to_gpr", []{
        Buffer gpr2; check( gpr_convert_gpr_to_gpr( &g_alloc, &g_params, &g_gpr.b, &gpr2.b), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );
    });

    // With no auto-generated preview requested, gpr_to_gpr must repackage the vc5 bitstream
    // instead of decoding and re-encoding; the output's bitstream is then byte-identical to
    // the input's.
    run_case( name + ": gpr_to_gpr (vc5 repackage, no re-encode)", []{
        gpr_parameters params = g_params;   // shallow copy; g_params owns the buffers
        params.enable_preview = false;

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &params, &g_gpr.b, &gpr2.b), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );

        Buffer vc5_in, vc5_out;
        check( make_vc5( vc5_in ), "vc5 extract from input" );
        check( gpr_convert_gpr_to_vc5( &g_alloc, &gpr2.b, &vc5_out.b), "vc5 extract from output" );
        check( vc5_in.b.size == vc5_out.b.size &&
               std::memcmp( vc5_in.b.buffer, vc5_out.b.buffer, vc5_in.b.size ) == 0,
               "vc5 bitstream byte-identical (repackaged, not re-encoded)" );
    });

    // -------- preview control (an omitted gpr_tools --preview maps to enable_preview=false) --------
    // enable_preview = false must suppress every embedded preview: both the auto-generated
    // thumbnail and a caller-supplied JPEG.
    run_case( name + ": gpr_to_gpr (enable_preview=false: no preview written)", []{
        gpr_parameters params = g_params;   // shallow copy; g_params owns the buffers
        params.enable_preview = false;

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &params, &g_gpr.b, &gpr2.b ), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );
        check( !tiff_find_jpeg_preview( gpr2.b, NULL, NULL ), "output contains no JPEG preview IFD" );
    });

#if GPR_JPEG_AVAILABLE
    run_case( name + ": gpr_to_gpr (enable_preview=false overrides supplied JPEG)", []{
        std::vector<unsigned char> jpg;
        check( make_test_jpeg( jpg ), "test JPEG encoded" );

        gpr_parameters params = g_params;
        params.enable_preview = false;
        params.preview_image.jpg_preview.buffer = jpg.data();
        params.preview_image.jpg_preview.size   = jpg.size();

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &params, &g_gpr.b, &gpr2.b ), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );
        check( !tiff_find_jpeg_preview( gpr2.b, NULL, NULL ), "output contains no JPEG preview IFD" );
    });

    // Contrast case: with enable_preview left at its default (true) the supplied JPEG must be
    // embedded byte-identical, proving the flag (not a broken preview path) removed it above.
    run_case( name + ": gpr_to_gpr (enable_preview=true embeds supplied JPEG)", []{
        std::vector<unsigned char> jpg;
        check( make_test_jpeg( jpg ), "test JPEG encoded" );

        gpr_parameters params = g_params;
        params.preview_image.jpg_preview.buffer = jpg.data();
        params.preview_image.jpg_preview.size   = jpg.size();

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &params, &g_gpr.b, &gpr2.b ), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );
        check_embedded_preview( gpr2, jpg.data(), jpg.size(),
                                "embedded preview byte-identical to supplied JPEG" );
    });
#endif

    // -------- preview control, plain DNG output --------
    // A DNG with no preview leaves the raw CFA image in IFD 0, where readers look for the
    // thumbnail, and shows up black in Finder and Lightroom. Unlike the GPR path there is no
    // vc5 encode to produce a thumbnail as a by-product, so the DNG writer decodes one - but
    // only for a caller that asked by setting preview_resolution, and a camera GPR carries no
    // preview of its own for it to pass through instead.
    run_case( name + ": gpr_to_dng (preview off: no preview written)", []{
        check( !tiff_find_jpeg_preview( g_gpr.b, NULL, NULL ), "source GPR has no preview to carry" );

        Buffer dng; check( make_dng( dng ), "conversion returns true" );
        check( !tiff_find_jpeg_preview( dng.b, NULL, NULL ), "no preview at the default resolution (NONE)" );

        // enable_preview overrides a resolution that was asked for, as it does for GPR output.
        gpr_parameters params = g_params;   // shallow copy; g_params owns the buffers
        params.preview_resolution = GPR_RGB_RESOLUTION_SIXTEENTH;
        params.enable_preview     = false;

        Buffer off_dng;
        check( gpr_convert_gpr_to_dng( &g_alloc, &params, &g_gpr.b, &off_dng.b ),
               "conversion returns true" );
        check( !tiff_find_jpeg_preview( off_dng.b, NULL, NULL ), "no preview with enable_preview=false" );
    });

#if GPR_JPEG_AVAILABLE
    run_case( name + ": gpr_to_dng (preview_resolution embeds a generated thumbnail)", []{
        gpr_parameters params = g_params;
        params.preview_resolution = GPR_RGB_RESOLUTION_SIXTEENTH;

        Buffer dng;
        check( gpr_convert_gpr_to_dng( &g_alloc, &params, &g_gpr.b, &dng.b ),
               "conversion returns true" );
        validate_dng_like( dng, g_W, g_H, /*vc5=*/false );

        size_t off = 0, sz = 0;
        unsigned int pw = 0, ph = 0;
        check( tiff_find_jpeg_preview( dng.b, &off, &sz, &pw, &ph ), "output contains a JPEG preview IFD" );

        // A real thumbnail, not a stub: JPEG bytes, at the resolution asked for. Each
        // resolution step is one wavelet level, and every level halves rounding up.
        const unsigned char* p = (const unsigned char*)dng.b.buffer + off;
        check( sz > 2 && off + sz <= dng.b.size && p[0] == 0xFF && p[1] == 0xD8, "preview is a JPEG" );

        unsigned int expect_w = g_W, expect_h = g_H;
        for( int i = 0; i < 4; ++i ) { expect_w = ( expect_w + 1 ) / 2; expect_h = ( expect_h + 1 ) / 2; }
        check( pw == expect_w && ph == expect_h, "preview is a sixteenth of the sensor" );
    });

    // A source that carries a preview keeps it rather than paying for a decode, and a
    // caller-supplied JPEG beats both. Nothing but the GPR written here has a preview to keep,
    // which is why this builds its own input.
    run_case( name + ": gpr_to_dng (source's preview kept, supplied JPEG wins)", []{
        std::vector<unsigned char> jpg, other;
        check( make_test_jpeg( jpg ), "test JPEG encoded" );

        gpr_parameters params = g_params;
        params.preview_image.jpg_preview.buffer = jpg.data();
        params.preview_image.jpg_preview.size   = jpg.size();

        Buffer gpr2;
        check( gpr_convert_gpr_to_gpr( &g_alloc, &params, &g_gpr.b, &gpr2.b ),
               "source GPR with a preview built" );
        check( tiff_find_jpeg_preview( gpr2.b, NULL, NULL ), "source GPR has a preview" );

        Buffer kept;
        check( gpr_convert_gpr_to_dng( &g_alloc, &g_params, &gpr2.b, &kept.b ),
               "conversion returns true" );
        validate_dng_like( kept, g_W, g_H, /*vc5=*/false );
        check_embedded_preview( kept, jpg.data(), jpg.size(), "kept the source's preview" );

        // Same input, but the caller supplies its own and asks for a decode as well: the
        // supplied JPEG is what lands, neither the source's nor a generated one.
        check( make_test_jpeg( other, 16, 12 ), "second test JPEG encoded" );
        params.preview_resolution = GPR_RGB_RESOLUTION_SIXTEENTH;
        params.preview_image.jpg_preview.buffer = other.data();
        params.preview_image.jpg_preview.size   = other.size();

        Buffer supplied;
        check( gpr_convert_gpr_to_dng( &g_alloc, &params, &gpr2.b, &supplied.b ),
               "conversion returns true" );
        check_embedded_preview( supplied, other.data(), other.size(), "supplied JPEG wins" );
    });

    // The metadata rewrite behind the lens correction must not cost the file its thumbnail:
    // nothing is decoded, the JPEG already in the input is copied across. It asks for no
    // preview of its own, so a surviving one can only have come from the input.
    run_case( name + ": dng_to_dng carries the thumbnail across", []{
        gpr_parameters params = g_params;
        params.preview_resolution = GPR_RGB_RESOLUTION_SIXTEENTH;

        Buffer dng, dng2;
        check( gpr_convert_gpr_to_dng( &g_alloc, &params, &g_gpr.b, &dng.b ),
               "gpr_to_dng ok" );

        size_t src_off = 0, src_sz = 0;
        check( tiff_find_jpeg_preview( dng.b, &src_off, &src_sz ), "source has a preview" );

        check( gpr_convert_dng_to_dng( &g_alloc, &g_params, &dng.b, &dng2.b ),
               "dng_to_dng ok" );
        validate_dng_like( dng2, g_W, g_H, /*vc5=*/false );
        check_embedded_preview( dng2, (const unsigned char*)dng.b.buffer + src_off, src_sz,
                                "preview byte-identical to the source's" );
    });
#endif

    // -------- decode to RGB (every resolution, both bit depths) ----------
    run_case( name + ": gpr_to_rgb (all resolutions/depths)", []{
        const GPR_RGB_RESOLUTION res[] = { GPR_RGB_RESOLUTION_HALF, GPR_RGB_RESOLUTION_QUARTER,
                                           GPR_RGB_RESOLUTION_EIGHTH, GPR_RGB_RESOLUTION_SIXTEENTH };
        const int depths[] = { 8, 16 };
        for( int bd = 0; bd < 2; ++bd )
        {
            unsigned int prev_w = 0xFFFFFFFF;
            for( int r = 0; r < 4; ++r )
            {
                RgbBuffer rgb;
                bool ok = gpr_convert_gpr_to_rgb( &g_alloc, res[r], depths[bd], &g_gpr.b, &rgb.b);
                check( ok, "gpr_to_rgb returns true" );
                validate_rgb( rgb, depths[bd] );
                if( rgb.b.width > 0 )
                {
                    check( rgb.b.width < prev_w, "coarser resolution is smaller" );
                    prev_w = rgb.b.width;
                }
#if GPR_JPEG_AVAILABLE
                if( depths[bd] == 8 && rgb.b.buffer )
                {
                    std::vector<unsigned char> jpg;
                    int enc = tje_encode_with_func( jpg_sink, &jpg, 2,
                                                    (int)rgb.b.width, (int)rgb.b.height, 3,
                                                    (const unsigned char*)rgb.b.buffer );
                    check( enc != 0, "jpeg encode ok" );
                    bool markers = jpg.size() > 4 &&
                                   jpg[0] == 0xFF && jpg[1] == 0xD8 &&
                                   jpg[jpg.size()-2] == 0xFF && jpg[jpg.size()-1] == 0xD9;
                    check( markers, "jpeg SOI/EOI markers present" );
                }
#endif
            }
        }
    });

    // -------- DNG -> * ---------------------------------------------------
    run_case( name + ": dng_to_raw (== gpr_to_raw, lossless)", []{
        Buffer dng, raw, dng_raw;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( make_raw( raw ), "gpr_to_raw ok" );
        check( gpr_convert_dng_to_raw( &g_alloc, &dng.b, &dng_raw.b), "dng_to_raw ok" );
        validate_raw( dng_raw, g_W, g_H );
        if( raw.valid() && dng_raw.valid() )
        {
            check( raw.b.size == dng_raw.b.size, "raw sizes match" );
            if( raw.b.size == dng_raw.b.size )
                check( memcmp( raw.b.buffer, dng_raw.b.buffer, raw.b.size ) == 0,
                       "decoded bytes identical via raw and via dng" );
        }
    });

    run_case( name + ": dng_to_dng", []{
        Buffer dng, dng2;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_dng( &g_alloc, &g_params, &dng.b, &dng2.b), "dng_to_dng ok" );
        validate_dng_like( dng2, g_W, g_H, /*vc5=*/false );
    });

    run_case( name + ": dng_to_dng preserves raw pixels", []{
        Buffer dng, dng2, raw1, raw2;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_dng( &g_alloc, &g_params, &dng.b, &dng2.b ), "dng_to_dng ok" );
        check( gpr_convert_dng_to_raw( &g_alloc, &dng.b,  &raw1.b ), "src decode ok" );
        check( gpr_convert_dng_to_raw( &g_alloc, &dng2.b, &raw2.b ), "dst decode ok" );
        check( raw1.b.size == raw2.b.size, "raw sizes match" );
        if( raw1.b.size == raw2.b.size && raw1.valid() && raw2.valid() )
            check( memcmp( raw1.b.buffer, raw2.b.buffer, raw1.b.size ) == 0,
                   "pixels identical after dng_to_dng" );
    });

    // A Bayer phase shift forces dng_to_dng off the image-handoff fast path onto the
    // legacy flat-buffer path; make sure that path still works.
    run_case( name + ": dng_to_dng with phase shift (legacy path)", []{
        Buffer dng, dng2;
        check( make_dng( dng ), "gpr_to_dng ok" );
        unsigned int saved_skip_rows = g_params.input_skip_rows;
        g_params.input_skip_rows = 2;
        bool ok = gpr_convert_dng_to_dng( &g_alloc, &g_params, &dng.b, &dng2.b );
        g_params.input_skip_rows = saved_skip_rows;
        check( ok, "dng_to_dng ok" );
        validate_dng_like( dng2, g_W, g_H, /*vc5=*/false );
    });

    run_case( name + ": dng_to_gpr", []{
        Buffer dng, dng_gpr;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_gpr( &g_alloc, &g_params, &dng.b, &dng_gpr.b), "dng_to_gpr ok" );
        validate_dng_like( dng_gpr, g_W, g_H, /*vc5=*/true );
    });

    run_case( name + ": dng_to_vc5", []{
        Buffer dng, dng_vc5;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_vc5( &g_alloc, &dng.b, &dng_vc5.b), "dng_to_vc5 ok" );
        check( dng_vc5.valid(), "output non-empty" );
    });

    // -------- RAW -> * (params from source metadata) ---------------------
    run_case( name + ": raw_to_dng", []{
        Buffer raw, raw_dng;
        check( make_raw( raw ), "gpr_to_raw ok" );
        check( gpr_convert_raw_to_dng( &g_alloc, &g_params, &raw.b, &raw_dng.b), "raw_to_dng ok" );
        validate_dng_like( raw_dng, g_W, g_H, /*vc5=*/false );
    });

    run_case( name + ": raw_to_gpr", []{
        Buffer raw, raw_gpr;
        check( make_raw( raw ), "gpr_to_raw ok" );
        check( gpr_convert_raw_to_gpr( &g_alloc, &g_params, &raw.b, &raw_gpr.b), "raw_to_gpr ok" );
        validate_dng_like( raw_gpr, g_W, g_H, /*vc5=*/true );
    });

    // -------- VC5 -> * ---------------------------------------------------
    // Both wrap the supplied VC5 bitstream into a DNG container, so both outputs
    // are VC5-compressed (a GPR is just a VC5-compressed DNG).
    run_case( name + ": vc5_to_gpr", []{
        Buffer vc5, vc5_gpr;
        check( make_vc5( vc5 ), "gpr_to_vc5 ok" );
        check( gpr_convert_vc5_to_gpr( &g_alloc, &g_params, &vc5.b, &vc5_gpr.b), "vc5_to_gpr ok" );
        validate_dng_like( vc5_gpr, g_W, g_H, /*vc5=*/true );
    });

    run_case( name + ": vc5_to_dng", []{
        Buffer vc5, vc5_dng;
        check( make_vc5( vc5 ), "gpr_to_vc5 ok" );
        check( gpr_convert_vc5_to_dng( &g_alloc, &g_params, &vc5.b, &vc5_dng.b), "vc5_to_dng ok" );
        validate_dng_like( vc5_dng, g_W, g_H, /*vc5=*/true );
    });

    // -------- negative: uncompressed DNG must not be flagged VC5 ---------
    run_case( name + ": uncompressed DNG not flagged VC5", []{
        Buffer dng;
        check( make_dng( dng ), "gpr_to_dng ok" );
        if( dng.valid() )
        {
            check( tiff_has_vc5_compression( dng.b ) == false, "plain DNG is not VC5 (TIFF tag)" );
            gpr_buffer tmp = dng.b;
            check( gpr_check_vc5( &g_alloc, &tmp ) == false, "plain DNG is not VC5 (gpr_check_vc5)" );
        }
    });

    gpr_parameters_destroy( &g_params, g_alloc.Free );
}

// ---------------------------------------------------------------------------
// gpr_tools --preview (dng_convert_main), GPR -> GPR
//
// The single --preview argument consolidates the former --preview_file_path /
// --preview_resolution / --preview_disable flags: omitted = no embedded
// preview, a jpg file on disk = embedded as-is, one of 2:1/4:1/8:1/16:1 = a
// preview auto-generated at that resolution, anything else = hard failure.
// Run once; the logic under test is sample-independent.
// ---------------------------------------------------------------------------

static std::string  g_cli_sample;
static unsigned int g_cli_W = 0, g_cli_H = 0;

static void run_preview_cli_tests( const std::string& sample_path )
{
    std::fprintf( stdout, "\n== gpr_tools --preview (dng_convert_main) ==\n" );

    g_cli_sample = sample_path;

    // Source dimensions, needed to validate outputs and auto-preview sizes.
    gpr_parameters params;
    gpr_parameters_set_defaults( &params );
    if( !gpr_parameters_parse_dng_file( &g_alloc, sample_path.c_str(), &params ) )
    {
        run_case( "--preview: parse sample", []{ check( false, "sample file could not be parsed" ); } );
        return;
    }
    g_cli_W = params.input_width;
    g_cli_H = params.input_height;
    gpr_parameters_destroy( &params, g_alloc.Free );

    // Omitted (the default: main.cpp passes "") and never set (NULL) both mean
    // that no preview is embedded.
    run_case( "--preview omitted: no preview embedded", []{
        const char* values[] = { "", NULL };
        for( int v = 0; v < 2; ++v )
        {
            const std::string out = scratch_path( "none.GPR" );
            dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), values[v] );
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            Buffer o;
            check( load_file( out.c_str(), o ), "output written" );
            std::remove( out.c_str() );
            validate_dng_like( o, g_cli_W, g_cli_H, /*vc5=*/true );
            check( !tiff_find_jpeg_preview( o.b, NULL, NULL ), "output contains no JPEG preview IFD" );
        }
    });

#if GPR_JPEG_AVAILABLE
    run_case( "--preview=<file.jpg>: embeds that JPEG", []{
        std::vector<unsigned char> jpg;
        check( make_test_jpeg( jpg ), "test JPEG encoded" );

        const std::string jpg_path = scratch_path( "supplied.jpg" );
        check( save_file( jpg_path.c_str(), jpg.data(), jpg.size() ), "preview JPEG written to disk" );

        const std::string out = scratch_path( "supplied.GPR" );
        dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), jpg_path.c_str() );
        check( dng_convert_main( &p ) == 0, "conversion succeeds" );
        std::remove( jpg_path.c_str() );

        Buffer o;
        check( load_file( out.c_str(), o ), "output written" );
        std::remove( out.c_str() );
        validate_dng_like( o, g_cli_W, g_cli_H, /*vc5=*/true );

        size_t off = 0, sz = 0;
        const bool found = tiff_find_jpeg_preview( o.b, &off, &sz );
        check( found, "output contains a JPEG preview IFD" );
        if( found )
            check( sz == jpg.size() && off + sz <= o.b.size &&
                   std::memcmp( (const unsigned char*)o.b.buffer + off, jpg.data(), sz ) == 0,
                   "embedded preview byte-identical to the supplied file" );
    });

    run_case( "--preview=<ratio>: auto-generates preview at that resolution", []{
        const char* ratios[] = { "2:1", "4:1", "8:1", "16:1" };
        for( int r = 0; r < 4; ++r )
        {
            const std::string out = scratch_path( "ratio.GPR" );
            dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), ratios[r] );
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            Buffer o;
            const bool loaded = load_file( out.c_str(), o );
            check( loaded, "output written" );
            std::remove( out.c_str() );
            if( !loaded ) continue;

            size_t off = 0, sz = 0;
            const bool found = tiff_find_jpeg_preview( o.b, &off, &sz );
            check( found, "output contains a JPEG preview IFD" );
            if( found && off + sz <= o.b.size )
            {
                unsigned int w = 0, h = 0;
                check( jpeg_sof_dims( (const unsigned char*)o.b.buffer + off, sz, &w, &h ),
                       "embedded JPEG header parses" );
                // The wavelet decoder rounds up at each halving (e.g. 3000 at 16:1 -> 188).
                const unsigned int divisor = 2u << r;   // 2, 4, 8, 16
                check( w == ( g_cli_W + divisor - 1 ) / divisor &&
                       h == ( g_cli_H + divisor - 1 ) / divisor,
                       "preview dimensions are source/ratio, rounded up" );
            }
        }
    });

    // The auto-generated preview is rendered during encoding from the encoder's own
    // wavelet data, through the same RGB pipeline (white balance, color matrix, tone
    // curve, black level, baseline exposure) and the same tiny_jpeg settings as the
    // decode path. Decoding the produced GPR at the preview's resolution must therefore
    // give the same image back.
    //
    // At 16:1 that equality is bit-exact: the deepest lowpass band is stored losslessly
    // in the vc5 bitstream, so encoder and decoder render identical pixels and the
    // re-encoded JPEG matches the embedded preview byte for byte -- verifying the whole
    // encode-side preview pipeline against the decode path. At 2:1/4:1/8:1 the decoder
    // reconstructs its wavelet level from quantized highpass bands, so the pixels differ
    // by quantization noise; there the checks are that the dimensions match exactly, the
    // JPEG headers are identical up to the scan data, and the compressed sizes agree
    // within 10% (empirically they agree within ~6% across all bundled samples).
    run_case( "--preview=<ratio>: embedded preview matches a decode of the output GPR", []{
        const char* ratios[] = { "2:1", "4:1", "8:1", "16:1" };
        const GPR_RGB_RESOLUTION res[] = { GPR_RGB_RESOLUTION_HALF, GPR_RGB_RESOLUTION_QUARTER,
                                           GPR_RGB_RESOLUTION_EIGHTH, GPR_RGB_RESOLUTION_SIXTEENTH };
        for( int r = 0; r < 4; ++r )
        {
            const std::string out = scratch_path( "roundtrip.GPR" );
            dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), ratios[r] );
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            Buffer o;
            const bool loaded = load_file( out.c_str(), o );
            check( loaded, "output written" );
            std::remove( out.c_str() );
            if( !loaded ) continue;

            size_t off = 0, sz = 0;
            const bool found = tiff_find_jpeg_preview( o.b, &off, &sz );
            check( found && sz > 0 && off + sz <= o.b.size, "output contains a JPEG preview strip" );
            if( !found || sz == 0 || off + sz > o.b.size ) continue;
            const unsigned char* emb = (const unsigned char*)o.b.buffer + off;

            unsigned int pw = 0, ph = 0;
            check( jpeg_sof_dims( emb, sz, &pw, &ph ), "embedded JPEG header parses" );

            // Decode the very GPR that carries the preview, at the preview's resolution.
            RgbBuffer rgb;
            check( gpr_convert_gpr_to_rgb( &g_alloc, res[r], 8, &o.b, &rgb.b ),
                   "decode of the output succeeds" );
            check( rgb.b.buffer != NULL && rgb.b.width == pw && rgb.b.height == ph,
                   "decode dimensions equal the embedded preview's" );
            if( rgb.b.buffer == NULL || rgb.b.width != pw || rgb.b.height != ph ) continue;

            // Re-encode the decoded RGB exactly the way the SDK embeds auto previews:
            // tiny_jpeg at quality 2, no EXIF segment (see gpr_convert_to_gpr's thumbnail path).
            std::vector<unsigned char> dec;
            check( tje_encode_with_func( jpg_sink, &dec, 2, (int)rgb.b.width, (int)rgb.b.height, 3,
                                         (const unsigned char*)rgb.b.buffer ) != 0, "jpeg encode ok" );

            if( res[r] == GPR_RGB_RESOLUTION_SIXTEENTH )
            {
                check( dec.size() == sz && std::memcmp( emb, dec.data(), sz ) == 0,
                       "16:1: decode is byte-identical to the embedded preview" );
            }
            else
            {
                const size_t emb_scan = jpeg_scan_data_offset( emb, sz );
                const size_t dec_scan = jpeg_scan_data_offset( dec.data(), dec.size() );
                check( emb_scan > 0 && emb_scan == dec_scan &&
                       std::memcmp( emb, dec.data(), emb_scan ) == 0,
                       "JPEG headers identical up to the scan data" );

                const size_t big   = ( dec.size() > sz ) ? dec.size() : sz;
                const size_t small = ( dec.size() > sz ) ? sz : dec.size();
                check( big - small <= big / 10,
                       "JPEG sizes within 10% (images differ only by quantization noise)" );
            }
        }
    });
#endif

    run_case( "--preview=<anything else>: conversion fails, no output", []{
        // 1:1 deliberately included: it is a valid --rgb_resolution but not a valid
        // preview. The last entry is a file that exists but is not a jpg.
        const char* bad[] = { "1:1", "3:1", "half", "no/such/preview.jpg", g_cli_sample.c_str() };
        for( size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i )
        {
            const std::string out = scratch_path( "bad.GPR" );
            dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), bad[i] );
            check( dng_convert_main( &p ) != 0, "conversion reports failure" );

            check_no_output( out );
        }
    });
}

// ---------------------------------------------------------------------------
// gpr_tools --lens_correction (dng_convert_main)
//
// Synthesizes a geometric OpcodeList3 WarpRectilinear for DNG output: either
// explicit k0,k1,k2,k3[,cx,cy] coefficients or "auto" (built-in per-camera
// profile). Camera-original CA-only warps are folded in; non-DNG outputs and
// unknown "auto" models must fail without writing anything.
// ---------------------------------------------------------------------------

static std::string g_cli_dir;

static bool parse_warp_of_file( const char* path, gpr_warp_rectilinear& warp )
{
    gpr_parameters params;
    gpr_parameters_set_defaults( &params );
    if( !gpr_parameters_parse_dng_file( &g_alloc, path, &params ) )
        return false;
    warp = params.tuning_info.warp;
    gpr_parameters_destroy( &params, g_alloc.Free );
    return true;
}

static void run_lens_correction_cli_tests( const std::string& data_dir )
{
    std::fprintf( stdout, "\n== gpr_tools --lens_correction (dng_convert_main) ==\n" );

    g_cli_dir = data_dir;

    run_case( "--lens_correction=k0,k1,k2,k3: warp written to DNG output", []{
        const std::string in  = g_cli_dir + "/Hero6/GOPR0024.GPR";
        const std::string out = scratch_path( "lens.DNG" );

        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "1.0,0.2,0,0";
        check( dng_convert_main( &p ) == 0, "conversion succeeds" );

        gpr_warp_rectilinear warp;
        check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
        std::remove( out.c_str() );

        check( warp.planes == 3, "warp has 3 planes" );
        check( warp.flags == 0x02, "warp flags mandatory + skip-for-preview" );
        check( warp.radial[0][0] == 1.0 && warp.radial[0][1] == 0.2 &&
               warp.radial[0][2] == 0.0 && warp.radial[0][3] == 0.0, "radial coefficients as given" );
        check( warp.center_x == 0.5 && warp.center_y == 0.5, "center defaults to 0.5,0.5" );
        check( !gpr_warp_rectilinear_is_ca_only( &warp ), "warp is geometric" );
    });

    run_case( "--lens_correction with explicit center", []{
        const std::string in  = g_cli_dir + "/Hero6/GOPR0024.GPR";
        const std::string out = scratch_path( "lens_center.DNG" );

        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "1.0,0.1,0,0,0.4,0.6";
        check( dng_convert_main( &p ) == 0, "conversion succeeds" );

        gpr_warp_rectilinear warp;
        check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
        std::remove( out.c_str() );

        check( warp.center_x == 0.4 && warp.center_y == 0.6, "center as given" );
    });

    run_case( "--lens_correction folds camera-original CA warp in (HERO5)", []{
        const std::string in  = g_cli_dir + "/Hero5/GOPR2657.GPR";

        gpr_warp_rectilinear ca;
        check( parse_warp_of_file( in.c_str(), ca ), "source parses" );
        check( gpr_warp_rectilinear_is_ca_only( &ca ), "source warp is CA-only" );

        const std::string out = scratch_path( "lens_ca.DNG" );
        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "1.0,0.1,0,0";
        check( dng_convert_main( &p ) == 0, "conversion succeeds" );

        gpr_warp_rectilinear warp;
        check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
        std::remove( out.c_str() );

        check( warp.planes == 3, "warp has 3 planes" );
        for( int plane = 0; plane < 3; plane++ )
        {
            const double scale = ca.radial[plane][0];
            check( warp.radial[plane][0] == scale &&
                   warp.radial[plane][1] == 0.1 * scale, "plane scaled by its CA coefficient" );
        }
        check( warp.radial[0][0] > 1.0, "red CA scale survived" );
    });

    run_case( "--lens_correction=auto: unknown camera model fails, no output", []{
        const std::string in  = g_cli_dir + "/Fusion/GPFR7066.GPR";       // no built-in profile
        const std::string out = scratch_path( "lens_auto.DNG" );

        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "auto";
        check( dng_convert_main( &p ) != 0, "conversion reports failure" );

        check_no_output( out );
    });

    run_case( "--lens_correction=auto: known camera model (HERO6) writes a warp", []{
        const std::string in  = g_cli_dir + "/Hero6/GOPR0024.GPR";
        const std::string out = scratch_path( "lens_auto6.DNG" );

        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "auto";
        check( dng_convert_main( &p ) == 0, "conversion succeeds" );

        gpr_warp_rectilinear warp;
        check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
        std::remove( out.c_str() );

        check( gpr_warp_rectilinear_is_valid( &warp ), "profile warp written" );
        check( warp.planes == 3 && warp.flags == 0x02, "profile warp shape" );

        // The profile's recommended strength (a partial correction) applies by
        // default, so the warp must be weaker than the full-strength profile.
        gpr_warp_rectilinear full;
        double default_strength = 0.0;
        check( gpr_lens_profile_lookup( "HERO6 Black", &full, &default_strength ), "profile lookup" );
        check( default_strength > 0.0 && default_strength < 1.0, "profile default is partial" );
        check( warp.radial[0][1] == full.radial[0][1] * default_strength,
               "default strength applied to profile coefficients" );
    });

    run_case( "--lens_correction_strength overrides and validates", []{
        const std::string in = g_cli_dir + "/Hero6/GOPR0024.GPR";

        gpr_warp_rectilinear full;
        check( gpr_lens_profile_lookup( "HERO6 Black", &full, NULL ), "profile lookup" );

        {   // strength=1 with auto: exactly the full-strength profile
            const std::string out = scratch_path( "lens_s1.DNG" );
            dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
            p.lens_correction = "auto";
            p.lens_correction_strength = "1";
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            gpr_warp_rectilinear warp;
            check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
            std::remove( out.c_str() );
            check( warp.radial[0][1] == full.radial[0][1], "strength=1 leaves profile unscaled" );
        }

        {   // strength=0.5 with explicit coefficients: k1 halved, k0 pulled toward 1
            const std::string out = scratch_path( "lens_s05.DNG" );
            dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
            p.lens_correction = "1.0,0.2,0,0";
            p.lens_correction_strength = "0.5";
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            gpr_warp_rectilinear warp;
            check( parse_warp_of_file( out.c_str(), warp ), "output DNG parses" );
            std::remove( out.c_str() );
            check( warp.radial[0][0] == 1.0 && warp.radial[0][1] == 0.1, "explicit coefficients scaled" );
        }

        {   // invalid values fail, and strength without lens_correction fails
            const char* bad[] = { "-0.1", "1.5", "abc", "0.5x" };
            for( size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i )
            {
                const std::string out = scratch_path( "lens_sbad.DNG" );
                dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
                p.lens_correction = "auto";
                p.lens_correction_strength = bad[i];
                check( dng_convert_main( &p ) != 0, "invalid strength reports failure" );
                check_no_output( out );
            }

            const std::string out = scratch_path( "lens_sonly.DNG" );
            dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
            p.lens_correction_strength = "0.5";
            check( dng_convert_main( &p ) != 0, "strength without lens_correction fails" );
            check_no_output( out );
        }
    });

    // The one-call SDK API (used by apps linking the library directly, e.g. a
    // Photos-extension host) must mirror the CLI's auto behavior.
    run_case( "gpr_parameters_apply_lens_profile (SDK API)", []{
        gpr_warp_rectilinear full;
        double def = 0.0;
        check( gpr_lens_profile_lookup( "HERO6 Black", &full, &def ), "HERO6 profile lookup" );

        {   // negative strength = the profile's recommended strength; reapplying is refused
            gpr_parameters params;
            gpr_parameters_set_defaults( &params );
            check( gpr_parameters_parse_dng_file( &g_alloc, ( g_cli_dir + "/Hero6/GOPR0024.GPR" ).c_str(), &params ),
                   "HERO6 parses" );

            check( gpr_parameters_apply_lens_profile( &params, -1.0 ) == GPR_LENS_PROFILE_APPLIED,
                   "profile applied" );
            check( params.tuning_info.warp.planes == 3 && params.tuning_info.warp.flags == 0x02,
                   "warp shape" );
            check( params.tuning_info.warp.radial[0][1] == full.radial[0][1] * def,
                   "recommended strength applied" );

            check( gpr_parameters_apply_lens_profile( &params, -1.0 ) == GPR_LENS_PROFILE_ALREADY_GEOMETRIC,
                   "second application refused (no double correction)" );
            gpr_parameters_destroy( &params, g_alloc.Free );
        }

        {   // unknown model: NOT_FOUND, params untouched. FUSION carries a
            // camera-original CA-only warp, so "untouched" is checked against a
            // copy rather than by expecting no warp at all: the lookup fails
            // before compose_ca could fold anything into it.
            gpr_parameters params;
            gpr_parameters_set_defaults( &params );
            check( gpr_parameters_parse_dng_file( &g_alloc, ( g_cli_dir + "/Fusion/GPFR7066.GPR" ).c_str(), &params ),
                   "FUSION parses" );

            const gpr_warp_rectilinear before = params.tuning_info.warp;

            check( gpr_parameters_apply_lens_profile( &params, -1.0 ) == GPR_LENS_PROFILE_NOT_FOUND,
                   "no profile for FUSION" );
            check( std::memcmp( &params.tuning_info.warp, &before, sizeof(before) ) == 0,
                   "params untouched" );
            gpr_parameters_destroy( &params, g_alloc.Free );
        }

        {   // MISSION 1 PRO has its own table row. The profile is in normalized radius, so
            // one row serves both photo modes (the 12 MP readout is the 50 MP active area
            // binned 2:1). No MISSION 1 PRO sample ships in data/samples, so the row is
            // checked through the lookup rather than through a parsed file.
            gpr_warp_rectilinear w;
            double def = 0.0;
            check( gpr_lens_profile_lookup( "MISSION 1 PRO", &w, &def ), "profile found for MISSION 1 PRO" );
            check( gpr_warp_rectilinear_is_valid( &w ), "warp installed" );
            check( !gpr_warp_rectilinear_is_ca_only( &w ), "warp is geometric" );
            check( w.radial[0][1] < 0.0, "barrel correction pulls the corners in" );
            check( def > 0.0 && def < 1.0, "profile default is partial" );
        }

        {   // HERO5: strength 0 keeps the camera CA warp; full strength composes it
            gpr_parameters params;
            gpr_parameters_set_defaults( &params );
            check( gpr_parameters_parse_dng_file( &g_alloc, ( g_cli_dir + "/Hero5/GOPR2657.GPR" ).c_str(), &params ),
                   "HERO5 parses" );

            gpr_warp_rectilinear ca = params.tuning_info.warp;
            check( gpr_warp_rectilinear_is_ca_only( &ca ), "source warp is CA-only" );

            check( gpr_parameters_apply_lens_profile( &params, 0.0 ) == GPR_LENS_PROFILE_APPLIED,
                   "strength 0 succeeds" );
            check( std::memcmp( &params.tuning_info.warp, &ca, sizeof(ca) ) == 0,
                   "CA warp untouched at strength 0" );

            gpr_warp_rectilinear h5full;
            check( gpr_lens_profile_lookup( "HERO5 Black", &h5full, NULL ), "HERO5 profile lookup" );
            check( gpr_parameters_apply_lens_profile( &params, 1.0 ) == GPR_LENS_PROFILE_APPLIED,
                   "full strength applied" );
            check( params.tuning_info.warp.radial[0][1] == h5full.radial[0][1] * ca.radial[0][0],
                   "CA composed into geometric warp" );
            gpr_parameters_destroy( &params, g_alloc.Free );
        }
    });

    run_case( "--lens_correction on GPR output fails, no output", []{
        const std::string in  = g_cli_dir + "/Hero6/GOPR0024.GPR";
        const std::string out = scratch_path( "lens.GPR" );

        dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
        p.lens_correction = "1.0,0.1,0,0";
        check( dng_convert_main( &p ) != 0, "conversion reports failure" );

        check_no_output( out );
    });

    run_case( "--lens_correction=<garbage> fails, no output", []{
        const char* bad[] = { "fisheye", "1.0,0.1", "1.0,0.1,0,0,0.5" };
        for( size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i )
        {
            const std::string in  = g_cli_dir + "/Hero6/GOPR0024.GPR";
            const std::string out = scratch_path( "lens_bad.DNG" );

            dng_convert_params p = preview_cli_params( in.c_str(), out.c_str(), "" );
            p.lens_correction = bad[i];
            check( dng_convert_main( &p ) != 0, "conversion reports failure" );

            check_no_output( out );
        }
    });
}

// ---------------------------------------------------------------------------
// gpr_tools --quality (dng_convert_main)
//
// Selects the VC-5 quantizer table used whenever the image is encoded to GPR.
// Omitted means the encoder default (Filmscan-X). A GPR input is repackaged
// unless --quality asks for a re-encode; other output types reject it. Uses
// the sample run_preview_cli_tests loaded (g_cli_sample), so it runs after it.
// ---------------------------------------------------------------------------

static void run_quality_cli_tests()
{
    std::fprintf( stdout, "\n== gpr_tools --quality (dng_convert_main) ==\n" );

    if( g_cli_sample.empty() )
    {
        run_case( "--quality: CLI sample", []{ check( false, "run_preview_cli_tests must run first (no CLI sample loaded)" ); } );
        return;
    }

    run_case( "quality defaults: Film Scan X, and no re-encode, from gpr_parameters_set_defaults", []{
        check( GPR_QUALITY_SETTING_DEFAULT == GPR_QUALITY_SETTING_FSX, "the default level is Film Scan X" );

        gpr_parameters params;
        gpr_parameters_set_defaults( &params );
        check( params.quality == GPR_QUALITY_SETTING_DEFAULT, "gpr_parameters_set_defaults gives the default level" );
        check( params.reencode == false, "gpr_parameters_set_defaults does not ask for a re-encode" );
        gpr_parameters_destroy( &params, g_alloc.Free );
    });

    // A DNG input always encodes. Each level quantizes the highpass bands finer than the one
    // before it, so every step up must produce a larger file: the size is the independent
    // evidence that a different table was applied. Omitting --quality must give the fsx bytes
    // exactly, since that is the default and nothing else about the encode may differ.
    run_case( "--quality=<level> on a DNG input: files grow with quality, omitted equals fsx", []{
        const std::string dng = scratch_path( "quality_src.DNG" );
        dng_convert_params pd = preview_cli_params( g_cli_sample.c_str(), dng.c_str(), "" );
        check( dng_convert_main( &pd ) == 0, "GPR -> DNG succeeds" );

        const std::string out_default = scratch_path( "quality_default.GPR" );
        dng_convert_params p0 = preview_cli_params( dng.c_str(), out_default.c_str(), "" );
        check( dng_convert_main( &p0 ) == 0, "default encode succeeds" );

        Buffer d;
        check( load_file( out_default.c_str(), d ), "default output written" );
        std::remove( out_default.c_str() );

        const char* levels[] = { "low", "medium", "high", "fs1", "fsx", "fs2", "ultra" };
        size_t prev = 0;
        for( size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i )
        {
            const std::string out = scratch_path( "quality_level.GPR" );
            dng_convert_params p = preview_cli_params( dng.c_str(), out.c_str(), "" );
            p.quality = levels[i];
            check( dng_convert_main( &p ) == 0, "conversion succeeds" );

            Buffer o;
            const bool loaded = load_file( out.c_str(), o );
            std::remove( out.c_str() );
            check( loaded, "output written" );
            if( !loaded ) continue;

            validate_dng_like( o, g_cli_W, g_cli_H, /*vc5=*/true );
            check( o.b.size > prev, "larger than the previous level" );
            prev = o.b.size;

            if( std::strcmp( levels[i], "fsx" ) == 0 && d.valid() )
                check( o.b.size == d.b.size && std::memcmp( o.b.buffer, d.b.buffer, o.b.size ) == 0,
                       "fsx output byte-identical to the default output" );
        }
        std::remove( dng.c_str() );
    });

    // A GPR input is repackaged without --quality; with it, the SDK decodes and re-encodes at
    // the level. Every bundled sample is encoded far above CineForm Low, so a `low` re-encode
    // has to come out well under the repackaged file while still being a valid vc5 GPR.
    run_case( "--quality on a GPR input re-encodes it; omitted repackages", []{
        const std::string out_repack = scratch_path( "quality_repack.GPR" );
        dng_convert_params pr = preview_cli_params( g_cli_sample.c_str(), out_repack.c_str(), "" );
        check( dng_convert_main( &pr ) == 0, "repackage succeeds" );

        const std::string out_low = scratch_path( "quality_low.GPR" );
        dng_convert_params pl = preview_cli_params( g_cli_sample.c_str(), out_low.c_str(), "" );
        pl.quality = "low";
        check( dng_convert_main( &pl ) == 0, "re-encode at low succeeds" );

        Buffer r, l;
        const bool loaded = load_file( out_repack.c_str(), r ) && load_file( out_low.c_str(), l );
        std::remove( out_repack.c_str() );
        std::remove( out_low.c_str() );
        check( loaded, "outputs written" );
        if( !loaded ) return;

        validate_dng_like( r, g_cli_W, g_cli_H, /*vc5=*/true );
        validate_dng_like( l, g_cli_W, g_cli_H, /*vc5=*/true );
        check( l.b.size < r.b.size, "low re-encode smaller than the repackaged camera bitstream" );
    });

    run_case( "--quality=<garbage> fails, no output", []{
        const char* bad[] = { "best", "fs3", "1", "fsx " };
        for( size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i )
        {
            const std::string out = scratch_path( "quality_bad.GPR" );
            dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), "" );
            p.quality = bad[i];
            check( dng_convert_main( &p ) != 0, "conversion reports failure" );
            check_no_output( out );
        }
    });

    run_case( "--quality with DNG output fails, no output", []{
        const std::string out = scratch_path( "quality_dng.DNG" );
        dng_convert_params p = preview_cli_params( g_cli_sample.c_str(), out.c_str(), "" );
        p.quality = "fs2";
        check( dng_convert_main( &p ) != 0, "conversion reports failure" );
        check_no_output( out );
    });
}

// ---------------------------------------------------------------------------
// Command-line scanning (program_options_lite)
//
// Unknown options and options missing their value must be flagged as fatal via
// Options::scan_failed (argument_parser::parse turns that into a failed parse,
// so gpr_tools exits instead of silently running with defaults). Positional
// arguments stay non-fatal: documented usage like "-d 1" leaves the "1" as an
// ignored unhandled argument.
// ---------------------------------------------------------------------------

static void run_argument_parser_tests()
{
    namespace po = program_options_lite;

    std::fprintf( stdout, "\n== gpr_tools command-line scanning (program_options_lite) ==\n" );

    struct Opts
    {
        po::Options options;
        std::string preview;
        std::string input;
        bool        verbose;

        Opts()
        {
            options.addOptions()
                ( "preview",   preview, std::string(""), "" )
                ( "input,i",   input,   std::string(""), "" )
                ( "verbose,v", verbose, false,           "" );
            po::setDefaults( options );
        }
    };

    run_case( "known options parse without scan_failed", []{
        Opts o;
        check( !o.options.scan_failed, "scan_failed starts false" );

        const char* argv[] = { "gpr_tools", "--preview=4:1", "-i", "in.GPR", "-v" };
        po::scanArgv( o.options, 5, argv );
        check( !o.options.scan_failed, "valid command line does not set scan_failed" );
        check( o.preview == "4:1" && o.input == "in.GPR" && o.verbose, "values stored" );
    });

    run_case( "unknown options set scan_failed", []{
        {
            Opts o;   // a retired flag, given as a valueless long option
            const char* argv[] = { "gpr_tools", "--preview_disable" };
            po::scanArgv( o.options, 2, argv );
            check( o.options.scan_failed, "unknown long option is flagged" );
        }
        {
            Opts o;   // a retired flag, given with a value
            const char* argv[] = { "gpr_tools", "--preview_file_path=x.jpg" };
            po::scanArgv( o.options, 2, argv );
            check( o.options.scan_failed, "unknown long option with value is flagged" );
        }
        {
            Opts o;
            const char* argv[] = { "gpr_tools", "-z", "5" };
            po::scanArgv( o.options, 3, argv );
            check( o.options.scan_failed, "unknown short option is flagged" );
        }
    });

    run_case( "option missing its value sets scan_failed", []{
        Opts o;
        const char* argv[] = { "gpr_tools", "-i" };
        po::scanArgv( o.options, 2, argv );
        check( o.options.scan_failed, "trailing option without value is flagged" );
    });

    run_case( "positional arguments stay non-fatal", []{
        Opts o;
        const char* argv[] = { "gpr_tools", "-v", "1" };   // like documented "-d 1"
        std::list<const char*> unhandled = po::scanArgv( o.options, 3, argv );
        check( !o.options.scan_failed, "positional argument does not set scan_failed" );
        check( unhandled.size() == 1 && std::strcmp( unhandled.front(), "1" ) == 0,
               "positional argument returned as unhandled" );
    });
}

// -------- gpr_flat_write_stream unit tests -------------------------------
//
// Direct coverage of the contiguous output stream used by gpr_convert_dng_to_dng:
// capacity growth across reallocation, offset-addressed writes (the seek-back
// patching dng_image_writer::WriteDNG relies on), read-back through DoRead, and
// detach() semantics (length vs over-allocated capacity, ownership transfer).
static void run_flat_write_stream_tests()
{
    std::printf( "\n== gpr_flat_write_stream ==\n" );

    run_case( "flat stream: sequential writes across growth", []{
        const size_t total = 200 * 1000;   // several reallocations from a 16-byte start,
        const size_t chunk = 7 * 1000;     // and multiple base-buffer flushes (64 KB)

        gpr_flat_write_stream stream( malloc, free, 16 );

        std::vector<unsigned char> src( total );
        for( size_t i = 0; i < total; i++ )
            src[i] = (unsigned char)( ( i * 131 + 7 ) & 0xFF );

        for( size_t pos = 0; pos < total; pos += chunk )
            stream.Put( &src[pos], (uint32)( pos + chunk <= total ? chunk : total - pos ) );

        gpr_buffer out = { NULL, 0 };
        stream.detach( &out );

        check( out.size == total, "detached size matches bytes written" );
        check( out.buffer != NULL, "detached buffer non-null" );
        if( out.buffer && out.size == total )
            check( memcmp( out.buffer, &src[0], total ) == 0, "detached bytes match written pattern" );
        free( out.buffer );
    });

    run_case( "flat stream: seek-back patching", []{
        const size_t total = 150 * 1000;

        gpr_flat_write_stream stream( malloc, free, 32 );

        std::vector<unsigned char> src( total, 0x5C );
        stream.Put( &src[0], (uint32)total );

        // Patch four bytes near the start and four past the first 64 KB flush
        // boundary, the way WriteDNG back-patches TIFF offsets.
        const unsigned char patch[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        stream.SetWritePosition( 8 );
        stream.Put( patch, 4 );
        stream.SetWritePosition( 100 * 1000 );
        stream.Put( patch, 4 );

        gpr_buffer out = { NULL, 0 };
        stream.detach( &out );

        check( out.size == total, "patching does not change length" );
        if( out.buffer && out.size == total )
        {
            const unsigned char* bytes = (const unsigned char*)out.buffer;
            check( memcmp( bytes + 8, patch, 4 ) == 0, "patch at offset 8 landed" );
            check( memcmp( bytes + 100 * 1000, patch, 4 ) == 0, "patch at offset 100000 landed" );
            check( bytes[7] == 0x5C && bytes[12] == 0x5C, "neighbors of first patch untouched" );
            check( bytes[100 * 1000 - 1] == 0x5C && bytes[100 * 1000 + 4] == 0x5C, "neighbors of second patch untouched" );
        }
        free( out.buffer );
    });

    run_case( "flat stream: read-back through the stream", []{
        const size_t total = 90 * 1000;

        gpr_flat_write_stream stream( malloc, free, 64 );

        std::vector<unsigned char> src( total );
        for( size_t i = 0; i < total; i++ )
            src[i] = (unsigned char)( i & 0xFF );

        stream.Put( &src[0], (uint32)total );
        stream.Flush();

        check( stream.Length() == total, "Length() reports bytes written" );

        std::vector<unsigned char> back( total, 0 );
        stream.SetReadPosition( 0 );
        stream.Get( &back[0], (uint32)total );
        check( memcmp( &back[0], &src[0], total ) == 0, "read-back matches written data" );

        // Read starting mid-stream too
        std::vector<unsigned char> tail( 1000, 0 );
        stream.SetReadPosition( total - 1000 );
        stream.Get( &tail[0], 1000 );
        check( memcmp( &tail[0], &src[total - 1000], 1000 ) == 0, "mid-stream read matches" );
    });

    run_case( "flat stream: detach sizes to length, not capacity", []{
        gpr_flat_write_stream stream( malloc, free, 1 << 20 );   // 1 MB preallocated

        const unsigned char bytes[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        stream.Put( bytes, 10 );

        gpr_buffer out = { NULL, 0 };
        stream.detach( &out );

        check( out.size == 10, "size is bytes written, not preallocated capacity" );
        if( out.buffer && out.size == 10 )
            check( memcmp( out.buffer, bytes, 10 ) == 0, "content correct" );
        free( out.buffer );
        // stream destructor after detach must not double-free (covered by this
        // case exiting cleanly under the fork harness)
    });
}

int main( int argc, char* argv[] )
{
    const char* data_dir = ( argc > 1 ) ? argv[1] : GPR_TESTS_DATA_DIR;

    const char* samples[] = {
        "Hero5/GOPR2657.GPR",
        "Hero6/GOPR0024.GPR",
        "HERO7/GOPR9231.GPR",
        "HERO9/GOPR0002.GPR",
        "Fusion/GPFR7066.GPR",
        "Fusion/GPBK7066.GPR",
    };
    const int num_samples = (int)( sizeof(samples) / sizeof(samples[0]) );

    std::fprintf( stdout, "GPR conversion tests: %d samples, data dir: %s\n", num_samples, data_dir );
#if !GPR_TESTS_HAVE_FORK
    std::fprintf( stdout, "(no process isolation on this platform: a crash will stop the suite)\n" );
#endif

    for( int i = 0; i < num_samples; ++i )
    {
        std::string path = std::string(data_dir) + "/" + samples[i];
        run_sample( path );
    }

    run_preview_cli_tests( std::string(data_dir) + "/Hero6/GOPR0024.GPR" );

    run_lens_correction_cli_tests( data_dir );

    run_quality_cli_tests();

    run_argument_parser_tests();

    run_flat_write_stream_tests();

    std::fprintf( stdout, "\n----------------------------------------\n" );
    std::fprintf( stdout, "Cases run: %d   failed: %d   crashed: %d   xfail(known issues): %d   xpass: %d\n",
                  g_cases, g_cases_failed, g_cases_crashed, g_xfail, g_xpass );

    // The suite passes when no working case failed/crashed and no known-issue
    // case unexpectedly started passing (a stale marker to remove).
    bool ok = ( g_cases_failed == 0 && g_cases_crashed == 0 && g_xpass == 0 );
    std::fprintf( stdout, "%s\n", ok ? "ALL CASES PASSED" : "SOME CASES FAILED" );
    return ok ? 0 : 1;
}
