/*! @file gpr_conversion_tests.cpp
 *
 *  @brief Exhaustive conversion tests for the GPR SDK.
 *
 *  Exercises every public gpr_convert_* entry point over the bundled sample
 *  files and validates the produced output (container magic, parseable
 *  metadata, dimensions, size invariants and round-trip equality) rather than
 *  merely checking that a call returned. The suite links the SDK directly and
 *  drives it through its C API, the same way gpr_tools does.
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

    gpr_buffer tmp = in; // gpr_parse_metadata takes a non-const pointer; share memory
    bool ok = gpr_parse_metadata( &g_alloc, &tmp, &params );

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
// Self-contained so the test does not depend on the public gpr_check_vc5 symbol
// (whose declaration does not match its definition in the current SDK).
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

    check( tiff_has_vc5_compression( out.b ) == expect_vc5, "vc5 compression flag as expected" );
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
#endif

// ---------------------------------------------------------------------------
// Shared per-sample state (set up in the parent; inherited by forked children)
// ---------------------------------------------------------------------------

static Buffer        g_gpr;       // the source sample
static gpr_parameters g_params;   // parsed metadata for the source
static unsigned int  g_W = 0, g_H = 0;

// Helpers that re-derive an intermediate format from the source GPR, so each
// case is self-contained and independently crash-isolated.
static bool make_raw( Buffer& raw ) { return gpr_convert_gpr_to_raw( &g_alloc, &g_gpr.b, &raw.b ); }
static bool make_dng( Buffer& dng ) { return gpr_convert_gpr_to_dng( &g_alloc, &g_params, &g_gpr.b, &dng.b ); }
static bool make_vc5( Buffer& vc5 ) { return gpr_convert_gpr_to_vc5( &g_alloc, &g_gpr.b, &vc5.b ); }

// ---------------------------------------------------------------------------
// The conversion matrix for one sample
// ---------------------------------------------------------------------------

static void run_sample( const std::string& sample_path )
{
    const std::string name = sample_path.substr( sample_path.find_last_of('/') + 1 );
    std::fprintf( stdout, "\n== %s ==\n", name.c_str() );

    g_gpr.release();
    if( !load_file( sample_path.c_str(), g_gpr ) )
    {
        run_case( name + ": load sample", []{ check( false, "sample file could not be read" ); } );
        return;
    }

    // Parse once up front (also a tested case); children inherit g_params.
    gpr_parameters_set_defaults( &g_params );
    {
        gpr_buffer in = g_gpr.b;
        if( !gpr_parse_metadata( &g_alloc, &in, &g_params ) )
        {
            run_case( name + ": parse metadata", []{ check( false, "gpr_parse_metadata failed" ); } );
            return;
        }
    }
    g_W = g_params.input_width;
    g_H = g_params.input_height;

    run_case( name + ": gpr_parse_metadata", []{
        check( g_W > 0 && g_H > 0, "input dimensions positive" );
        check( g_params.tuning_info.dgain_saturation_level.level_red >
               g_params.tuning_info.static_black_level.r_black, "white level > black level" );
        check( g_params.tuning_info.wb_gains.r_gain > 0 &&
               g_params.tuning_info.wb_gains.g_gain > 0 &&
               g_params.tuning_info.wb_gains.b_gain > 0, "white-balance gains positive" );
        check( g_params.tuning_info.pixel_format >= PIXEL_FORMAT_RGGB_12 &&
               g_params.tuning_info.pixel_format <= PIXEL_FORMAT_BGGR_14, "pixel format in range" );
        check( tiff_has_vc5_compression( g_gpr.b ), "source GPR detected as VC5" );
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

    run_case( name + ": gpr_to_vc5", []{
        Buffer vc5; check( make_vc5( vc5 ), "conversion returns true" );
        check( vc5.valid(), "output non-empty" );
    });

    run_case( name + ": gpr_to_gpr", []{
        Buffer gpr2; check( gpr_convert_gpr_to_gpr( &g_alloc, &g_params, &g_gpr.b, &gpr2.b ), "conversion returns true" );
        validate_dng_like( gpr2, g_W, g_H, /*vc5=*/true );
    });

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
                bool ok = gpr_convert_gpr_to_rgb( &g_alloc, res[r], depths[bd], &g_gpr.b, &rgb.b );
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
        check( gpr_convert_dng_to_raw( &g_alloc, &dng.b, &dng_raw.b ), "dng_to_raw ok" );
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
        check( gpr_convert_dng_to_dng( &g_alloc, &g_params, &dng.b, &dng2.b ), "dng_to_dng ok" );
        validate_dng_like( dng2, g_W, g_H, /*vc5=*/false );
    });

    run_case( name + ": dng_to_gpr", []{
        Buffer dng, dng_gpr;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_gpr( &g_alloc, &g_params, &dng.b, &dng_gpr.b ), "dng_to_gpr ok" );
        validate_dng_like( dng_gpr, g_W, g_H, /*vc5=*/true );
    });

    run_case( name + ": dng_to_vc5", []{
        Buffer dng, dng_vc5;
        check( make_dng( dng ), "gpr_to_dng ok" );
        check( gpr_convert_dng_to_vc5( &g_alloc, &dng.b, &dng_vc5.b ), "dng_to_vc5 ok" );
        check( dng_vc5.valid(), "output non-empty" );
    });

    // -------- RAW -> * (params from source metadata) ---------------------
    run_case( name + ": raw_to_dng", []{
        Buffer raw, raw_dng;
        check( make_raw( raw ), "gpr_to_raw ok" );
        check( gpr_convert_raw_to_dng( &g_alloc, &g_params, &raw.b, &raw_dng.b ), "raw_to_dng ok" );
        validate_dng_like( raw_dng, g_W, g_H, /*vc5=*/false );
    });

    run_case( name + ": raw_to_gpr", []{
        Buffer raw, raw_gpr;
        check( make_raw( raw ), "gpr_to_raw ok" );
        check( gpr_convert_raw_to_gpr( &g_alloc, &g_params, &raw.b, &raw_gpr.b ), "raw_to_gpr ok" );
        validate_dng_like( raw_gpr, g_W, g_H, /*vc5=*/true );
    });

    // -------- VC5 -> * ---------------------------------------------------
    // Both wrap the supplied VC5 bitstream into a DNG container, so both outputs
    // are VC5-compressed (a GPR is just a VC5-compressed DNG).
    run_case( name + ": vc5_to_gpr", []{
        Buffer vc5, vc5_gpr;
        check( make_vc5( vc5 ), "gpr_to_vc5 ok" );
        check( gpr_convert_vc5_to_gpr( &g_alloc, &g_params, &vc5.b, &vc5_gpr.b ), "vc5_to_gpr ok" );
        validate_dng_like( vc5_gpr, g_W, g_H, /*vc5=*/true );
    });

    run_case( name + ": vc5_to_dng", []{
        Buffer vc5, vc5_dng;
        check( make_vc5( vc5 ), "gpr_to_vc5 ok" );
        check( gpr_convert_vc5_to_dng( &g_alloc, &g_params, &vc5.b, &vc5_dng.b ), "vc5_to_dng ok" );
        validate_dng_like( vc5_dng, g_W, g_H, /*vc5=*/true );
    });

    // -------- negative: uncompressed DNG must not be flagged VC5 ---------
    run_case( name + ": uncompressed DNG not flagged VC5", []{
        Buffer dng;
        check( make_dng( dng ), "gpr_to_dng ok" );
        if( dng.valid() )
            check( tiff_has_vc5_compression( dng.b ) == false, "plain DNG is not VC5" );
    });

    gpr_parameters_destroy( &g_params, g_alloc.Free );
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

    std::fprintf( stdout, "\n----------------------------------------\n" );
    std::fprintf( stdout, "Cases run: %d   failed: %d   crashed: %d   xfail(known issues): %d   xpass: %d\n",
                  g_cases, g_cases_failed, g_cases_crashed, g_xfail, g_xpass );

    // The suite passes when no working case failed/crashed and no known-issue
    // case unexpectedly started passing (a stale marker to remove).
    bool ok = ( g_cases_failed == 0 && g_cases_crashed == 0 && g_xpass == 0 );
    std::fprintf( stdout, "%s\n", ok ? "ALL CASES PASSED" : "SOME CASES FAILED" );
    return ok ? 0 : 1;
}
