/*! @file standalone.c
 *  @brief Driver that runs LLVMFuzzerTestOneInput on a list of files,
 *         catching SIGSEGV/SIGABRT so we can report which input crashed.
 *
 *  No libFuzzer / sanitizer dependency — builds and runs anywhere
 *  Clang or GCC + libc are available. Useful for:
 *    - sanity-checking the harness compiles cleanly
 *    - running the corpus in CI without sanitizer instrumentation
 *    - quickly testing a single crashing input found by the libFuzzer build
 *
 *  Usage: fuzz_decoder_standalone <file> [file...]
 *
 *  Licensed under Apache-2.0 or MIT.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/stat.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static sigjmp_buf g_crash_jmp;
static volatile sig_atomic_t g_in_test = 0;
static volatile sig_atomic_t g_last_signo = 0;

static void crash_handler(int signo)
{
    if (g_in_test) {
        g_last_signo = signo;
        siglongjmp(g_crash_jmp, 1);
    }
    /* Not inside a test — re-raise default action. */
    signal(signo, SIG_DFL);
    raise(signo);
}

static int read_file(const char *path, uint8_t **out_buf, size_t *out_size)
{
    *out_buf = NULL;
    *out_size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return -1; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? size : 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    if (got != size) { free(buf); return -1; }
    *out_buf = buf;
    *out_size = size;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <input-file> [input-file...]\n"
            "  runs LLVMFuzzerTestOneInput on each file, catching SIGSEGV/SIGABRT.\n"
            "  exits 0 if all files survive, non-zero if any crashed.\n",
            argv[0]);
        return 2;
    }

    /* Install crash handlers. */
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS,  crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);
    signal(SIGILL,  crash_handler);

    int crashes = 0;
    int unreadable = 0;
    int ok = 0;
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        uint8_t *buf = NULL;
        size_t size = 0;
        if (read_file(path, &buf, &size) != 0) {
            fprintf(stderr, "  [error] cannot read %s\n", path);
            unreadable++;
            continue;
        }

        printf("  [%d] %s  (%zu bytes)  ... ", i, path, size);
        fflush(stdout);

        if (sigsetjmp(g_crash_jmp, 1) == 0) {
            g_in_test = 1;
            (void)LLVMFuzzerTestOneInput(buf, size);
            g_in_test = 0;
            printf("ok\n");
            ok++;
        } else {
            g_in_test = 0;
            printf("CRASHED on signal %d\n", (int)g_last_signo);
            crashes++;
        }

        free(buf);
    }

    printf("\nfuzz_decoder_standalone: %d ok, %d crash%s out of %d input%s\n",
           ok, crashes, crashes == 1 ? "" : "es",
           argc - 1, (argc - 1) == 1 ? "" : "s");
    return crashes == 0 && unreadable == 0 ? 0 : 1;
}
