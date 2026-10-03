#include "clock_adapter.h"

#define MIDI_TICK  0xF8
#define MIDI_START 0xFA

/* Longer than this between pulses and we treat the next one as a fresh start
 * (re-emit 0xFA so smack resets bar phase) rather than a very slow tempo. */
#define CLK_IDLE_SECONDS 2.0

/* Tempo search range, matching smack's own detect_bpm bounds. */
#define CLK_BPM_MIN 60.0
#define CLK_BPM_MAX 180.0

static double fpt_from_bpm(int sr, double bpm)
{
    if (bpm <= 0.0) return 0.0;
    return (double)sr * 60.0 / (bpm * 24.0); /* 24 ppqn */
}

static double bpm_from_fpt(int sr, double fpt)
{
    if (fpt <= 0.0) return 0.0;
    return (double)sr * 60.0 / (fpt * 24.0);
}

void clk_init(clock_adapter_t *c, int sample_rate, float free_bpm)
{
    int i;
    c->sample_rate     = sample_rate;
    c->mode            = CLK_AUTO;
    c->ticks_per_pulse = CLK_TICKS_1X;
    c->frames_per_tick = fpt_from_bpm(sample_rate, free_bpm > 0 ? free_bpm : 120.0);
    c->accum           = 0.0;
    c->frame           = 0;
    c->last_edge       = 0;
    c->have_edge       = 0;
    for (i = 0; i < 4; i++) c->iv[i] = 0.0;
    c->iv_n            = 0;
    c->free_bpm        = free_bpm > 0 ? free_bpm : 120.0f;
    c->locked          = 0;
    c->pending_start   = 0;
    c->tick_now        = 0;
    c->ticks_this_pulse = 0;
}

void clk_set_mode(clock_adapter_t *c, clk_mode_t m) { c->mode = m; }

void clk_set_ratio(clock_adapter_t *c, int ticks_per_pulse)
{
    if (ticks_per_pulse == CLK_TICKS_DIV2 || ticks_per_pulse == CLK_TICKS_1X
        || ticks_per_pulse == CLK_TICKS_2X)
        c->ticks_per_pulse = ticks_per_pulse;
}

void clk_set_free_bpm(clock_adapter_t *c, float bpm)
{
    if (bpm < 20.0f)  bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    c->free_bpm = bpm;
    if (!c->locked) c->frames_per_tick = fpt_from_bpm(c->sample_rate, bpm);
}

/* Are the last few intervals close enough to be a real clock? Used by AUTO to
 * tell "someone is patching a clock" from "someone is hitting a trigger". */
static int intervals_are_steady(const clock_adapter_t *c)
{
    double lo, hi;
    int    i;
    if (c->iv_n < 3) return 0;
    lo = hi = c->iv[0];
    for (i = 1; i < 3; i++) {
        if (c->iv[i] < lo) lo = c->iv[i];
        if (c->iv[i] > hi) hi = c->iv[i];
    }
    if (lo <= 0.0) return 0;
    return (hi / lo) < 1.12; /* within 12% */
}

/* Fold an arbitrary interval into the musical range by octaves. This is how
 * WTF! copes with players who tap irregular rhythms rather than clocks. */
static double fold_to_range(int sr, double interval_frames)
{
    double bpm;
    int    guard = 0;
    if (interval_frames <= 0.0) return 0.0;
    bpm = (double)sr * 60.0 / interval_frames;
    while (bpm > CLK_BPM_MAX && guard++ < 8) bpm *= 0.5;
    while (bpm < CLK_BPM_MIN && guard++ < 8) bpm *= 2.0;
    return bpm;
}

void clk_gate_edge(clock_adapter_t *c)
{
    double interval, idle_frames;
    int    i;

    idle_frames = CLK_IDLE_SECONDS * (double)c->sample_rate;

    if (!c->have_edge) {
        c->have_edge      = 1;
        c->last_edge      = c->frame;
        c->pending_start  = 1; /* first pulse defines the downbeat */
        c->iv_n           = 0;
        return;
    }

    interval = (double)(c->frame - c->last_edge);
    c->last_edge = c->frame;

    if (interval <= 0.0) return;

    if (interval > idle_frames) {
        /* Treated as a fresh start, not a very slow tempo. */
        c->pending_start = 1;
        c->iv_n          = 0;
        return;
    }

    /* Ring of recent intervals, newest first. */
    for (i = 3; i > 0; i--) c->iv[i] = c->iv[i - 1];
    c->iv[0] = interval;
    if (c->iv_n < 4) c->iv_n++;

    {
        clk_mode_t eff = c->mode;
        if (eff == CLK_AUTO)
            eff = intervals_are_steady(c) ? CLK_EXTERNAL : CLK_INFER;

        if (eff == CLK_EXTERNAL) {
            c->frames_per_tick = interval / (double)c->ticks_per_pulse;
        } else {
            double bpm = fold_to_range(c->sample_rate, interval);
            if (bpm > 0.0) c->frames_per_tick = fpt_from_bpm(c->sample_rate, bpm);
        }
        c->locked = 1;
    }

    /* Re-lock tick phase to the pulse so error cannot accumulate.
     *
     * The pulse IS a tick boundary, so it emits one tick and resets the
     * accumulator. That tick REPLACES the accumulated one that was about to
     * fire at the same instant -- it is not an extra. Getting this wrong adds
     * one tick per pulse, which reads downstream as a tempo ~4% fast and
     * silently shortens every captured bar. */
    c->accum            = 0.0;
    c->tick_now         = 1;
    c->ticks_this_pulse = 1; /* the edge tick counts toward the budget */
}

void clk_advance(clock_adapter_t *c, int frames, clk_emit_fn emit, void *ctx)
{
    if (frames <= 0) return;

    if (c->frames_per_tick <= 0.0)
        c->frames_per_tick = fpt_from_bpm(c->sample_rate, c->free_bpm);

    /* Fall back to free-run if the clock source goes away. */
    if (c->locked && c->have_edge) {
        double since = (double)(c->frame - c->last_edge);
        if (since > CLK_IDLE_SECONDS * (double)c->sample_rate) {
            c->locked = 0;
            c->frames_per_tick = fpt_from_bpm(c->sample_rate, c->free_bpm);
        }
    }

    if (c->pending_start) {
        c->pending_start = 0;
        if (emit) emit(ctx, MIDI_START);
    }

    if (c->tick_now) {
        c->tick_now = 0;
        if (emit) emit(ctx, MIDI_TICK);
    }

    c->accum += (double)frames;
    while (c->accum >= c->frames_per_tick) {
        /* While locked to a pulse train, a pulse is worth EXACTLY
         * ticks_per_pulse ticks. Without this cap the accumulator emits its
         * full quota and then the next edge adds one more, so the engine sees
         * 25 ticks per quarter instead of 24 -- a tempo ~4% fast that shows
         * up as every captured bar coming out short. Free-run is uncapped. */
        if (c->locked && c->ticks_this_pulse >= c->ticks_per_pulse) {
            c->accum = c->frames_per_tick; /* hold, ready for the next pulse */
            break;
        }
        c->accum -= c->frames_per_tick;
        c->ticks_this_pulse++;
        if (emit) emit(ctx, MIDI_TICK);
    }

    c->frame += (uint64_t)frames;
}

float clk_bpm(const clock_adapter_t *c)
{
    return (float)bpm_from_fpt(c->sample_rate, c->frames_per_tick);
}

int clk_locked(const clock_adapter_t *c) { return c->locked; }
