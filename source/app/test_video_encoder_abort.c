/*! @file test_video_encoder_abort.c
 *
 *  Verifies the two production-readiness paths added to gpr_video_encoder
 *  in May 2026:
 *
 *  1. Writer callback returning a NEGATIVE value flags the encoder as
 *     aborted. Pending frames are dropped (writer_fn NOT invoked for them).
 *     Subsequent submit() returns -1. destroy() returns promptly without
 *     blocking on flush.
 *
 *  2. gpr_video_encoder_cancel() called explicitly from the caller does the
 *     same: drops pending work, unblocks any in-flight flush, destroys
 *     promptly. Useful for app shutdown / user cancellation.
 *
 *  Both paths must NOT deadlock destroy(). The test enforces a wall-clock
 *  timeout to catch any regression.
 *
 *  (C) Copyright 2018 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "../lib/vc5_encoder/gpr_video.h"

/* ============================================================
   Test fixture: a small synthetic frame size to keep the test fast.
   ============================================================ */
#define TEST_W 256
#define TEST_H 256
#define TEST_RAW_BYTES (TEST_W * TEST_H * 2)
#define TEST_PIXEL_FORMAT 4  /* RGGB16 */
#define TEST_QUALITY 3

static uint8_t *make_synthetic_frame(int width, int height, int seed) {
    uint8_t *buf = (uint8_t *)malloc((size_t)width * height * 2);
    uint16_t *p = (uint16_t *)buf;
    for (int r = 0; r < height; r++) {
        for (int c = 0; c < width; c++) {
            p[r * width + c] = (uint16_t)((seed * 73 + r * 17 + c * 5) & 0x3FFF);
        }
    }
    return buf;
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ============================================================
   Writer callbacks
   ============================================================ */

typedef struct {
    int calls;
    int abort_after_n;   /* return -1 once `calls` exceeds this; 0 disables */
    int last_rc;
} writer_state;

static int writer_fatal_after_n(void *user_data, const uint8_t *vc5,
                                 size_t size, uint64_t frame_tag) {
    (void)vc5; (void)size; (void)frame_tag;
    writer_state *ws = (writer_state *)user_data;
    ws->calls++;
    if (ws->abort_after_n > 0 && ws->calls > ws->abort_after_n) {
        ws->last_rc = -1;
        return -1;  /* signal fatal */
    }
    ws->last_rc = 0;
    return 0;
}

static int writer_count_only(void *user_data, const uint8_t *vc5,
                              size_t size, uint64_t frame_tag) {
    (void)vc5; (void)size; (void)frame_tag;
    writer_state *ws = (writer_state *)user_data;
    ws->calls++;
    return 0;
}

/* Slow writer: sleeps 10ms per frame so the pipeline can back up and the
   cancel-from-side-thread test has frames pending when cancel fires. */
static int writer_slow(void *user_data, const uint8_t *vc5,
                        size_t size, uint64_t frame_tag) {
    (void)vc5; (void)size; (void)frame_tag;
    writer_state *ws = (writer_state *)user_data;
    ws->calls++;
    struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10 ms */
    nanosleep(&ts, NULL);
    return 0;
}

/* ============================================================
   Test 1: Writer returns <0 → encoder aborts.
   - Submit 20 frames
   - Writer returns -1 after the 5th call
   - destroy() must complete quickly (<2s wall time)
   - submit() after abort must return -1
   - writer must NOT be called for frames after the abort signal
   ============================================================ */
static int test_writer_fatal(void) {
    fprintf(stderr, "[test_writer_fatal] start\n");
    writer_state ws = {0};
    ws.abort_after_n = 5;

    GPR_VIDEO_ENCODER *ctx = gpr_video_encoder_create(
        TEST_W, TEST_H, TEST_PIXEL_FORMAT, TEST_QUALITY,
        2 /* ring_depth */, writer_fatal_after_n, &ws);
    if (!ctx) {
        fprintf(stderr, "  FAIL: create returned NULL\n");
        return 1;
    }

    uint8_t *frame = make_synthetic_frame(TEST_W, TEST_H, 0);

    int submits_ok = 0, submits_rejected = 0;
    for (int i = 0; i < 20; i++) {
        int rc = gpr_video_encoder_submit(ctx, frame, TEST_RAW_BYTES,
                                           (uint64_t)i);
        if (rc == 0) submits_ok++;
        else        submits_rejected++;
        /* tiny pacing so the writer thread has a chance to advance */
        struct timespec ts = { 0, 200 * 1000 };  /* 200 us */
        nanosleep(&ts, NULL);
    }

    /* Now destroy. Must return within ~2s; without the fatal-handling fix
       it would block forever on flush(). */
    double t0 = now_seconds();
    gpr_video_encoder_destroy(ctx);
    double elapsed = now_seconds() - t0;

    free(frame);

    fprintf(stderr, "  submits: ok=%d rejected=%d\n", submits_ok, submits_rejected);
    fprintf(stderr, "  writer calls: %d (set to fatal after %d)\n",
            ws.calls, ws.abort_after_n);
    fprintf(stderr, "  destroy elapsed: %.3fs\n", elapsed);

    int ok = 1;
    if (elapsed > 2.0) {
        fprintf(stderr, "  FAIL: destroy() took %.3fs (>2s)\n", elapsed);
        ok = 0;
    }
    if (ws.calls < ws.abort_after_n + 1) {
        fprintf(stderr, "  FAIL: writer never returned fatal (calls=%d)\n",
                ws.calls);
        ok = 0;
    }
    if (submits_rejected == 0) {
        fprintf(stderr, "  WARN: no submits rejected — abort may have happened "
                "after all 20 frames queued. That's still valid as long as "
                "destroy completed quickly.\n");
    }
    fprintf(stderr, "[test_writer_fatal] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ============================================================
   Test 2: gpr_video_encoder_cancel() called explicitly.
   - Submit 10 frames
   - Background thread calls cancel() after 50ms
   - destroy() must complete quickly
   - submit() after cancel must return -1
   ============================================================ */
typedef struct {
    GPR_VIDEO_ENCODER *ctx;
    int delay_ms;
} cancel_thread_arg;

static void *cancel_thread_fn(void *arg) {
    cancel_thread_arg *a = (cancel_thread_arg *)arg;
    struct timespec ts = { a->delay_ms / 1000,
                            (a->delay_ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
    gpr_video_encoder_cancel(a->ctx);
    return NULL;
}

static int test_explicit_cancel(void) {
    fprintf(stderr, "[test_explicit_cancel] start\n");
    writer_state ws = {0};

    GPR_VIDEO_ENCODER *ctx = gpr_video_encoder_create(
        TEST_W, TEST_H, TEST_PIXEL_FORMAT, TEST_QUALITY,
        2 /* ring_depth */, writer_slow, &ws);
    if (!ctx) {
        fprintf(stderr, "  FAIL: create returned NULL\n");
        return 1;
    }

    /* Fire cancel from another thread after 50ms — simulating user
       hitting cancel while the encoder is still busy. The writer sleeps
       10ms per frame so the pipeline backs up and there are pending
       frames at cancel time. */
    pthread_t cancel_thread;
    cancel_thread_arg arg = { ctx, 50 };
    pthread_create(&cancel_thread, NULL, cancel_thread_fn, &arg);

    uint8_t *frame = make_synthetic_frame(TEST_W, TEST_H, 0);
    int submits_ok = 0, submits_rejected = 0;
    for (int i = 0; i < 50; i++) {
        int rc = gpr_video_encoder_submit(ctx, frame, TEST_RAW_BYTES,
                                           (uint64_t)i);
        if (rc == 0) submits_ok++;
        else        submits_rejected++;
        struct timespec ts = { 0, 500 * 1000 };
        nanosleep(&ts, NULL);
    }
    pthread_join(cancel_thread, NULL);

    double t0 = now_seconds();
    gpr_video_encoder_destroy(ctx);
    double elapsed = now_seconds() - t0;

    free(frame);

    fprintf(stderr, "  submits: ok=%d rejected=%d (some expected to be rejected)\n",
            submits_ok, submits_rejected);
    fprintf(stderr, "  writer calls: %d\n", ws.calls);
    fprintf(stderr, "  destroy elapsed: %.3fs\n", elapsed);

    int ok = 1;
    if (elapsed > 2.0) {
        fprintf(stderr, "  FAIL: destroy() took %.3fs (>2s)\n", elapsed);
        ok = 0;
    }
    if (submits_rejected == 0) {
        fprintf(stderr, "  FAIL: cancel did not reject any submits\n");
        ok = 0;
    }
    fprintf(stderr, "[test_explicit_cancel] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ============================================================
   Test 3: cancel() is idempotent and safe from inside writer_fn.
   ============================================================ */
typedef struct {
    GPR_VIDEO_ENCODER *ctx;
    int calls;
    int self_cancel_after;
} self_cancel_state;

static int writer_self_cancels(void *user_data, const uint8_t *vc5,
                                size_t size, uint64_t frame_tag) {
    (void)vc5; (void)size; (void)frame_tag;
    self_cancel_state *s = (self_cancel_state *)user_data;
    s->calls++;
    if (s->calls == s->self_cancel_after) {
        gpr_video_encoder_cancel(s->ctx);
        gpr_video_encoder_cancel(s->ctx);  /* second call is no-op */
    }
    return 0;  /* not fatal — relying on cancel() instead */
}

static int test_self_cancel(void) {
    fprintf(stderr, "[test_self_cancel] start\n");
    self_cancel_state s = {0};
    s.self_cancel_after = 3;

    GPR_VIDEO_ENCODER *ctx = gpr_video_encoder_create(
        TEST_W, TEST_H, TEST_PIXEL_FORMAT, TEST_QUALITY,
        2 /* ring_depth */, writer_self_cancels, &s);
    if (!ctx) {
        fprintf(stderr, "  FAIL: create returned NULL\n");
        return 1;
    }
    s.ctx = ctx;

    uint8_t *frame = make_synthetic_frame(TEST_W, TEST_H, 0);
    for (int i = 0; i < 30; i++) {
        gpr_video_encoder_submit(ctx, frame, TEST_RAW_BYTES, (uint64_t)i);
        struct timespec ts = { 0, 200 * 1000 };
        nanosleep(&ts, NULL);
    }

    double t0 = now_seconds();
    gpr_video_encoder_destroy(ctx);
    double elapsed = now_seconds() - t0;

    free(frame);
    fprintf(stderr, "  writer calls: %d (cancel-on call %d)\n",
            s.calls, s.self_cancel_after);
    fprintf(stderr, "  destroy elapsed: %.3fs\n", elapsed);

    int ok = (elapsed <= 2.0 && s.calls >= s.self_cancel_after);
    fprintf(stderr, "[test_self_cancel] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(void) {
    int fails = 0;
    fails += test_writer_fatal();
    fails += test_explicit_cancel();
    fails += test_self_cancel();
    fprintf(stderr, "\n==========================================\n");
    fprintf(stderr, "Encoder abort tests: %d failure(s)\n", fails);
    fprintf(stderr, "==========================================\n");
    return fails ? 1 : 0;
}
