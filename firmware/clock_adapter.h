/*
 * clock_adapter — turn a Eurorack gate jack into the 24 ppqn MIDI clock that
 * smack_core already speaks.
 *
 * The whole point: smack_core's timing model (retro-capture alignment, the
 * 96-tick regression window, drift compensation) is the part that took
 * longest to get right on the Move. Rather than port it to analog clocking,
 * we synthesize MIDI clock bytes and feed them to smack_on_midi() unchanged.
 *
 * DELIBERATELY has no libDaisy include, so it builds and runs in the native
 * test harness. See test/test_clock_adapter.c — this is how M2 gets built
 * and verified without flashing anything (DESIGN.md §13).
 */
#ifndef CLOCK_ADAPTER_H
#define CLOCK_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the gate jack means. Maps to the SW_1 three-position switch. */
typedef enum {
    CLK_EXTERNAL = 0, /* pulses are a clock; ratio applies */
    CLK_INFER,        /* pulses are capture triggers; tempo inferred from them */
    CLK_AUTO          /* steady pulse train -> EXTERNAL, else INFER */
} clk_mode_t;

/* Ticks emitted per input pulse. Maps to the SW_0 three-position switch.
 * =1 means one pulse per quarter note (24 ppqn). */
#define CLK_TICKS_DIV2 48 /* pulse = half note   ("/2") */
#define CLK_TICKS_1X   24 /* pulse = quarter note ("=1") */
#define CLK_TICKS_2X   12 /* pulse = eighth note  ("x2") */

/* Emitted as a callback so the caller decides what to do with the byte —
 * the firmware forwards to smack_on_midi(), the test records it. */
typedef void (*clk_emit_fn)(void *ctx, uint8_t midi_byte);

typedef struct {
    int      sample_rate;
    clk_mode_t mode;
    int      ticks_per_pulse;

    double   frames_per_tick;  /* current estimate; 0 = no tempo yet */
    double   accum;            /* frames since the last emitted tick */
    uint64_t frame;            /* running frame counter */
    uint64_t last_edge;        /* frame of the previous rising edge */
    int      have_edge;

    double   iv[4];            /* recent pulse intervals, for steadiness */
    int      iv_n;

    float    free_bpm;         /* fallback when nothing is patched */
    int      locked;           /* 1 = tempo came from the gate jack */
    int      pending_start;    /* emit 0xFA before the next tick */
    int      tick_now;         /* a pulse landed: emit its tick, once */
    int      ticks_this_pulse; /* hard cap: never exceed ticks_per_pulse */
} clock_adapter_t;

void  clk_init(clock_adapter_t *c, int sample_rate, float free_bpm);
void  clk_set_mode(clock_adapter_t *c, clk_mode_t m);
void  clk_set_ratio(clock_adapter_t *c, int ticks_per_pulse);
void  clk_set_free_bpm(clock_adapter_t *c, float bpm);

/* Call on every rising edge of the gate jack. */
void  clk_gate_edge(clock_adapter_t *c);

/* Advance the clock by one audio block, emitting 0xFA/0xF8 as they fall due.
 * Call once per block BEFORE smack_process(), mirroring host_sim's
 * send_due_ticks(). */
void  clk_advance(clock_adapter_t *c, int frames, clk_emit_fn emit, void *ctx);

/* Current tempo in BPM (inferred, external, or free-run). */
float clk_bpm(const clock_adapter_t *c);

/* 1 when tempo is being driven by the gate jack rather than free_bpm. */
int   clk_locked(const clock_adapter_t *c);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_ADAPTER_H */
