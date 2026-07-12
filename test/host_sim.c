/*
 * Native simulation harness for the Mark engine — no hardware needed.
 * Fakes the host API (120 BPM project tempo), drives mark_process with
 * deterministic input, and asserts the RC-505-style track lifecycle:
 * record length quantization, grid-aligned second-track starts, overdub,
 * undo/redo swap, reverse, one-shot, all start/stop, presets, and
 * get_param buffer safety.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/mark_core.h"

#define BLOCK 128
/* 120 BPM: 918.75 frames/tick, 88200 frames/measure */
#define FPM 88200u

static float fake_bpm(void) { return 120.0f; }
static void fake_log(const char *msg) { (void)msg; }
static int fake_clock_status(void) { return MOVE_CLOCK_STATUS_UNAVAILABLE; }

static host_api_v1_t host = {
    .api_version = 1,
    .sample_rate = MARK_SR,
    .frames_per_block = BLOCK,
    .log = fake_log,
    .get_clock_status = fake_clock_status,
    .get_bpm = fake_bpm,
};

/* deterministic test signal per absolute input frame */
static int16_t sig(uint64_t k) { return (int16_t)((k % 997) * 31 % 4001 - 2000); }

static uint64_t g_in_frame = 0;

/* Feed n frames; mode 0 = signal, 1 = silence, 2 = constant 1000. */
static void run(mark_t *m, uint64_t n, int mode, int16_t *last_out) {
    static int16_t in[BLOCK * 2], out[BLOCK * 2];
    while (n > 0) {
        int c = n > BLOCK ? BLOCK : (int)n;
        for (int i = 0; i < c; i++) {
            int16_t v = mode == 0 ? sig(g_in_frame) : (mode == 2 ? 1000 : 0);
            in[i * 2] = v;
            in[i * 2 + 1] = v;
            g_in_frame++;
        }
        for (int i = c; i < BLOCK; i++) { in[i * 2] = in[i * 2 + 1] = 0; }
        mark_process(m, in, out, c);
        if (last_out) memcpy(last_out, out, (size_t)c * 2 * sizeof(int16_t));
        n -= (uint64_t)c;
    }
}

static int gp_int(mark_t *m, const char *key) {
    char buf[64];
    int n = mark_get_param(m, key, buf, sizeof(buf));
    assert(n >= 0);
    return atoi(buf);
}

static void gp_str(mark_t *m, const char *key, char *buf, int len) {
    int n = mark_get_param(m, key, buf, len);
    assert(n >= 0);
}

static int tstate(mark_t *m, int ti) {
    char key[16], buf[16];
    snprintf(key, sizeof(key), "t%d_state", ti + 1);
    gp_str(m, key, buf, sizeof(buf));
    return atoi(buf);
}

static uint32_t tlen(mark_t *m, int ti) {
    char key[16], buf[24];
    snprintf(key, sizeof(key), "t%d_len", ti + 1);
    gp_str(m, key, buf, sizeof(buf));
    return (uint32_t)strtoul(buf, NULL, 10);
}

/* ------------------------------------------------------------------ */

static void test_record_quantize(void) {
    mark_t *m = mark_create(&host);
    assert(m);
    g_in_frame = 0;

    /* record ~1.93 measures; stop should extend to exactly 2 measures */
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_REC);          /* immediate: nothing playing */
    run(m, 170000, 0, NULL);
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_REC);          /* still extending to target */
    run(m, 2 * FPM - 170000 + BLOCK, 0, NULL);
    assert(tstate(m, 0) == MK_PLAY);
    assert(tlen(m, 0) == 2 * FPM);

    /* playback must reproduce the recorded signal (unity gains) */
    int16_t out[BLOCK * 2];
    /* consume the remainder of the current block-misaligned position */
    int pos128 = gp_int(m, "tpos");
    (void)pos128;
    char buf[64];
    gp_str(m, "tpos", buf, sizeof(buf));
    /* read current integer position from pos fraction is lossy; instead
     * drive to a known point: play exactly to the loop top by feeding
     * silence until tpos returns 0 for track 1 */
    /* simpler: capture playback over one full loop and compare content */
    uint32_t len = tlen(m, 0);
    /* advance to loop start: pos is (frames since finalize) % len; we fed
     * exactly (2*FPM - 170000 + BLOCK) frames after target hit, but the
     * finalize happened mid-run. Just scan: feed until output matches
     * sig stream start for 64 consecutive frames. */
    uint64_t rec_start_val = 0;   /* first recorded frame was g_in_frame 0 */
    (void)rec_start_val;
    int found = 0;
    for (uint32_t scan = 0; scan < len + BLOCK * 4 && !found; scan += BLOCK) {
        run(m, BLOCK, 1, out);
        if (out[0] == sig(0) && out[2] == sig(1) && out[4] == sig(2)) found = 1;
    }
    assert(found);

    mark_destroy(m);
    printf("ok: record quantize + playback\n");
}

static void test_second_track_aligned(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;

    /* exact 1-measure loop on track 1 */
    mark_set_param(m, "t1_btn", "1");
    run(m, FPM - 20000, 0, NULL);
    mark_set_param(m, "t1_btn", "1");        /* round(0.77) -> 1 measure */
    run(m, 20000 + BLOCK, 0, NULL);
    assert(tstate(m, 0) == MK_PLAY);
    assert(tlen(m, 0) == FPM);

    /* track 2: press mid-measure — must wait for the grid */
    run(m, 30000, 0, NULL);
    mark_set_param(m, "t2_btn", "1");
    assert(tstate(m, 1) == MK_EMPTY);
    char buf[64];
    gp_str(m, "tpend", buf, sizeof(buf));
    assert(strncmp(buf, "0,1", 3) == 0);     /* pending record */

    /* by the next boundary it must be recording, aligned with t1's top */
    run(m, FPM, 0, NULL);
    assert(tstate(m, 1) == MK_REC);
    /* stop after ~0.6 measures -> rounds to 1 measure, keeps recording */
    run(m, 50000, 0, NULL);
    mark_set_param(m, "t2_btn", "1");
    run(m, FPM, 0, NULL);
    assert(tstate(m, 1) == MK_PLAY);
    assert(tlen(m, 1) == FPM);

    /* both tracks aligned: tpos equal (same length, same grid) */
    gp_str(m, "tpos", buf, sizeof(buf));
    int p1, p2;
    sscanf(buf, "%d,%d", &p1, &p2);
    assert(abs(p1 - p2) <= 1);

    mark_destroy(m);
    printf("ok: second track grid-aligned\n");
}

static void test_overdub_undo_redo(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;
    mark_set_param(m, "quantize", "0");      /* exact lengths for math */

    mark_set_param(m, "t1_btn", "1");
    run(m, FPM, 0, NULL);                    /* one measure of signal */
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_PLAY);
    assert(tlen(m, 0) == FPM);

    /* overdub constant 1000 over one full loop */
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_DUB);
    run(m, FPM, 2, NULL);
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_PLAY);
    assert(gp_int(m, "undo_avail") == 1);

    /* play from the loop top; output = sig + 1000 */
    int16_t out[BLOCK * 2];
    run(m, BLOCK, 1, out);                   /* pos wrapped to 0 after dub */
    int16_t expect0 = (int16_t)(sig(0) + 1000);
    assert(out[0] == expect0);

    /* undo: swap job runs muted, then original content returns */
    mark_set_param(m, "undo", "1");
    run(m, FPM, 1, NULL);                    /* let the swap finish + wrap */
    assert(gp_int(m, "undo_avail") == 2);    /* redo available */
    /* scan one loop for the original signal at the top */
    int found = 0;
    for (uint32_t scan = 0; scan < FPM + BLOCK * 4 && !found; scan += BLOCK) {
        run(m, BLOCK, 1, out);
        if (out[0] == sig(0) && out[2] == sig(1)) found = 1;
    }
    assert(found);

    /* redo: the dub comes back */
    mark_set_param(m, "undo", "1");
    run(m, FPM, 1, NULL);
    assert(gp_int(m, "undo_avail") == 1);
    found = 0;
    for (uint32_t scan = 0; scan < FPM + BLOCK * 4 && !found; scan += BLOCK) {
        run(m, BLOCK, 1, out);
        if (out[0] == expect0) found = 1;
    }
    assert(found);

    mark_destroy(m);
    printf("ok: overdub + undo + redo\n");
}

static void test_reverse_oneshot(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;
    mark_set_param(m, "quantize", "0");

    mark_set_param(m, "t1_btn", "1");
    run(m, FPM, 0, NULL);
    mark_set_param(m, "t1_btn", "1");
    mark_set_param(m, "t1_stop", "1");
    assert(tstate(m, 0) == MK_STOP);

    /* reverse: first frame out is the LAST recorded frame */
    mark_set_param(m, "t1_rev", "1");
    mark_set_param(m, "t1_btn", "1");        /* nothing playing: immediate */
    assert(tstate(m, 0) == MK_PLAY);
    int16_t out[BLOCK * 2];
    run(m, BLOCK, 1, out);
    assert(out[0] == sig(FPM - 1));
    /* overdub refused while reversed */
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_PLAY);
    mark_set_param(m, "t1_stop", "1");
    mark_set_param(m, "t1_rev", "0");

    /* one-shot: plays exactly once then stops */
    mark_set_param(m, "t1_shot", "1");
    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_PLAY);
    run(m, FPM + BLOCK, 1, NULL);
    assert(tstate(m, 0) == MK_STOP);

    mark_destroy(m);
    printf("ok: reverse + one-shot\n");
}

static void test_all_and_clear(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;
    mark_set_param(m, "quantize", "0");

    for (int t = 0; t < 2; t++) {
        char key[8];
        snprintf(key, sizeof(key), "t%d_btn", t + 1);
        mark_set_param(m, key, "1");
        run(m, FPM, 0, NULL);
        mark_set_param(m, key, "1");
        snprintf(key, sizeof(key), "t%d_stop", t + 1);
        mark_set_param(m, key, "1");
    }
    assert(tstate(m, 0) == MK_STOP && tstate(m, 1) == MK_STOP);

    mark_set_param(m, "all_btn", "1");
    assert(tstate(m, 0) == MK_PLAY && tstate(m, 1) == MK_PLAY);
    run(m, 1000, 1, NULL);
    mark_set_param(m, "all_btn", "1");
    assert(tstate(m, 0) == MK_STOP && tstate(m, 1) == MK_STOP);

    mark_set_param(m, "t1_clear", "1");
    assert(tstate(m, 0) == MK_EMPTY);
    assert(tlen(m, 0) == 0);
    mark_set_param(m, "t2_clear", "1");

    mark_destroy(m);
    printf("ok: all start/stop + clear\n");
}

static void test_state_blob(void) {
    mark_t *m = mark_create(&host);
    mark_set_param(m, "t2_level", "150");
    mark_set_param(m, "t3_pan", "10");
    mark_set_param(m, "t4_rev", "1");
    mark_set_param(m, "t5_shot", "1");
    mark_set_param(m, "quantize", "0");
    mark_set_param(m, "dub_mode", "1");
    mark_set_param(m, "master", "80");

    char blob[512];
    gp_str(m, "state", blob, sizeof(blob));

    mark_t *m2 = mark_create(&host);
    mark_set_param(m2, "state", blob);
    assert(gp_int(m2, "t2_level") == 150);
    assert(gp_int(m2, "t3_pan") == 10);
    assert(gp_int(m2, "t4_rev") == 1);
    assert(gp_int(m2, "t5_shot") == 1);
    assert(gp_int(m2, "quantize") == 0);
    assert(gp_int(m2, "dub_mode") == 1);
    assert(gp_int(m2, "master") == 80);

    /* tiny output buffers must not crash or overflow (smack OOB lesson) */
    for (int sz = 2; sz <= 64; sz *= 2) {
        char small[64];
        memset(small, 0x7f, sizeof(small));
        int n = mark_get_param(m, "state", small, sz);
        assert(n < sz);
        n = mark_get_param(m, "status", small, sz);
        assert(n < sz);
    }

    mark_destroy(m2);
    mark_destroy(m);
    printf("ok: state blob round-trip + buffer safety\n");
}

static void test_monitor(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;
    int16_t out[BLOCK * 2];
    run(m, BLOCK, 2, out);
    assert(out[0] == 1000);                  /* monitor on: passthrough */
    mark_set_param(m, "monitor", "0");
    run(m, BLOCK, 2, out);
    assert(out[0] == 0);                     /* muted */
    mark_destroy(m);
    printf("ok: monitor gate\n");
}

static void test_clocked_grid(void) {
    mark_t *m = mark_create(&host);
    g_in_frame = 0;

    /* drive a 120 BPM MIDI clock: 918.75 frames/tick. Feed ticks and
     * audio interleaved; record start must wait for the measure flag. */
    uint8_t start = 0xFA, tick = 0xF8;
    mark_on_midi(m, &start, 1, 3);
    double next_tick = 918.75;
    uint64_t fed = 0;

    mark_set_param(m, "t1_btn", "1");
    assert(tstate(m, 0) == MK_EMPTY);        /* pending: clock is running */

    /* feed 2 measures of clocked audio */
    while (fed < 2 * FPM) {
        run(m, BLOCK, 0, NULL);
        fed += BLOCK;
        while ((double)fed >= next_tick) {
            mark_on_midi(m, &tick, 1, 3);
            next_tick += 918.75;
        }
    }
    assert(tstate(m, 0) == MK_REC);          /* fired on a measure boundary */

    mark_destroy(m);
    printf("ok: clocked measure grid\n");
}

int main(void) {
    test_record_quantize();
    test_second_track_aligned();
    test_overdub_undo_redo();
    test_reverse_oneshot();
    test_all_and_clear();
    test_state_blob();
    test_monitor();
    test_clocked_grid();
    printf("all mark sim tests passed\n");
    return 0;
}
