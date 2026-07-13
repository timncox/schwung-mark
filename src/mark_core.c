/*
 * Mark core engine. See mark_core.h for the model.
 *
 * v1 simplifications (documented follow-ups, not accidents):
 *   - Playback is speed 1.0 only: no tempo-sync time-stretch or varispeed.
 *     Tracks recorded at one tempo drift against a changed clock tempo.
 *   - Record/pending actions land on a block boundary (<= 2.9 ms early/late)
 *     when driven by the clocked measure flag; base-track boundaries are
 *     frame-accurate.
 *   - Overdubbing a reversed track is refused (the RC-505 does the same).
 *   - Loop audio is not saved with presets; `state` carries settings only.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mark_core.h"

/* Allocation fallback ladder: first capacity that calloc grants wins. */
static const int alloc_seconds[] = { MARK_MAX_SECONDS, 45, 30, 20, 15 };
#define ALLOC_STEPS 5

/* Incremental undo-swap budget per 128-frame block. 32768 frames is ~0.5 MB
 * of traffic — comfortably inside the render budget, and a full 60 s swap
 * completes in ~81 blocks (~0.24 s) with the track muted. */
#define SWAP_FRAMES_PER_BLOCK 32768

/* Per-track insert FX (the RC's Track FX, reduced to one slot per track).
 * Codes are stable — the pad UI and web editor index into them. */
typedef enum {
    TFX_OFF = 0,
    TFX_LPF,        /* param = cutoff, exponential ~60 Hz .. ~11 kHz */
    TFX_HPF,        /* param = cutoff, exponential ~40 Hz .. ~10 kHz */
    TFX_CRUSH,      /* param = sample-hold depth + bit quantize */
    TFX_DELAY,      /* tempo-synced 8th echo, param = feedback/mix */
    TFX_PHASER,     /* 4-stage allpass sweep, param = LFO rate */
    TFX_RINGMOD,    /* sine ring mod, param = carrier freq */
    TFX_COUNT
} tfx_t;

#define MARK_DLY_LEN 8192      /* per-track delay line, frames */
#define MARK_SESSION_SLOTS 16

/* RBJ biquad (smack pattern) for the LPF/HPF track FX */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } mk_bq_t;

static inline float bq_run(mk_bq_t *q, float x) {
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static void bq_set(mk_bq_t *q, float freq, int highpass, float Q) {
    float w0 = 6.2831853f * freq / (float)MARK_SR;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float inv = 1.0f / (1.0f + alpha);
    if (highpass) {
        q->b0 = (1.0f + cw) * 0.5f * inv;
        q->b1 = -(1.0f + cw) * inv;
        q->b2 = q->b0;
    } else {
        q->b0 = (1.0f - cw) * 0.5f * inv;
        q->b1 = (1.0f - cw) * inv;
        q->b2 = q->b0;
    }
    q->a1 = -2.0f * cw * inv;
    q->a2 = (1.0f - alpha) * inv;
}

typedef struct {
    int16_t *buf;          /* track_frames * 2, interleaved */
    uint32_t len;          /* current loop frames, 0 = empty */
    uint32_t full_len;     /* frames as finalized — trim can't exceed this */
    uint32_t rec_len;      /* frames recorded so far while MK_REC */
    uint32_t rec_target;   /* 0 = none; keep recording until this length */
    int      rec_end;      /* state to enter when recording finalizes */
    double   pos;          /* 0 .. len */
    int      st;           /* mk_tstate_t */
    int      pending;      /* 0 none, 1 = start record, 2 = start play */
    int      level;        /* 0..200, 100 = unity (RC play level) */
    int      pan;          /* 0..100, 50 = center */
    int      rev, shot;
    float    g_cur;        /* smoothed level gain */
    float    pg[2];        /* balance gains L/R */

    /* Track FX runtime */
    int      fx;           /* tfx_t code, kept while switched off */
    int      fx_on;
    int      fxp;          /* 0..100 */
    int      fx_dirty;     /* recompute filter coefficients in render */
    mk_bq_t  bqL, bqR;
    int      crush_cnt;
    float    crush_l, crush_r;
    float    *dly;         /* MARK_DLY_LEN * 2 floats, interleaved */
    uint32_t dly_w;
    float    ph_lfo;       /* phaser LFO phase 0..1 */
    float    ph_x[2][4], ph_y[2][4];
    float    rm_phase;
} mk_track_t;

struct mark {
    const host_api_v1_t *host;
    uint32_t track_frames;       /* per-track capacity actually allocated */

    mk_track_t t[MARK_TRACKS];
    int16_t *undo_buf;           /* track_frames * 2 */

    /* Undo: single level, last record/overdub gesture wins.
     * kind 0 = overdub (undo_buf holds the pre-dub audio for the covered
     * region; undo = incremental swap, so redo is the same swap again).
     * kind 1 = first recording (undo = clear, redo = restore length). */
    int      undo_track;         /* -1 = none */
    int      undo_kind;
    int      undo_redo;          /* 0 = next undo press undoes, 1 = redoes */
    int      undo_capturing;     /* copy-before-write active (dub gesture) */
    uint32_t undo_start;         /* loop frame where the dub began */
    uint32_t undo_count;         /* frames captured, capped at len */
    uint32_t undo_saved_len;     /* kind 1: length to restore on redo */
    int      swap_track;         /* -1 = no swap job running */
    uint32_t swap_pos;

    /* Clock (smack pattern). Global frame counter across all blocks. */
    uint64_t global_frames;
    double   frames_per_tick;    /* smoothed; 918.75 = 120 BPM */
    uint64_t last_tick_global;
    uint32_t tick_total;
    int      clock_running;
    int      clock_seen;
    int      measure_flag;       /* set on tick_total % 96 == 0, consumed
                                    by the pending scheduler */

    /* Grid: frames per measure captured when the first track finalized.
     * Used for quantization while free-running; a running clock always
     * wins. Reset when every track is cleared. */
    double   grid_unit;

    /* Params */
    int   quantize;              /* 0 off, 1 on (default) */
    int   rec_grid;              /* quantize unit: 0 measure (default),
                                    1 beat, 2 eighth, 3 sixteenth — finer
                                    grids allow polymetric loops (15/16) */
    int   rec_action;            /* 0 rec->play (default), 1 rec->dub */
    int   dub_mode;              /* 0 overdub (default), 1 replace */
    int   play_mode;             /* 0 multi (default); 1 single: starting a
                                    track stops the others (song sections) */
    int   master;                /* 0..200 */
    float master_g;
    int   monitor;               /* 0 = mute live input at the output */
    int   hw_input;              /* set by the gen wrapper */
    int   follow;                /* 1 = Move transport stop pauses loops */
    int   transport_paused;
    float bpm_override;          /* free-run tempo; 0 = project tempo */

    /* Remote-UI sync: bumped on any state-visible edit; gates the browser
     * editor's full-state refetch (schwung-manager polls rui_poll). */
    uint32_t edit_rev;

    /* Sessions: loop audio + settings saved per slot (tN.wav + a flat
     * session.json) under session_dir. File I/O runs on a detached worker
     * thread; io_busy gates every state-mutating action meanwhile. */
    char session_dir[240];
    volatile int io_busy;        /* 0 idle, 1 saving, 2 loading */
    volatile int io_error;       /* last I/O op failed */
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int16_t clip16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

/* snprintf returns the WOULD-HAVE-WRITTEN length — on truncation a raw
 * `n += snprintf(...)` pushes n past buf_len and later appends write out
 * of bounds. Every state append goes through this clamp (smack lesson). */
static int nclamp(int n, int buf_len) {
    return (n < 0) ? 0 : (n >= buf_len ? buf_len - 1 : n);
}

/* Trigger params fire on any ACTIVE value: UI pads send "1" (every press
 * fires) and hierarchy trigger enums send the literal "trigger"; "0" and
 * "idle" are the no-ops autosave restores send. */
static int trig_active(const char *val) {
    return atoi(val) != 0 || strcmp(val, "trigger") == 0;
}

static int json_int(const char *js, const char *key, int def) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atoi(p + strlen(pat)) : def;
}

static void update_track_gains(mk_track_t *t) {
    /* balance law: center passes both channels at unity */
    t->pg[0] = t->pan <= 50 ? 1.0f : (float)(100 - t->pan) / 50.0f;
    t->pg[1] = t->pan >= 50 ? 1.0f : (float)t->pan / 50.0f;
}

/* ------------------------------------------------------------------ */
/*  Clock                                                              */
/* ------------------------------------------------------------------ */

/* A RUNNING clock always wins. A stopped-but-seen clock keeps its
 * remembered tempo unless the user set a bpm_override (smack lesson:
 * clock_seen is sticky and would silently eat the override forever). */
static int clock_governs(const mark_t *m) {
    return m->clock_seen && (m->clock_running || m->bpm_override <= 0.0f);
}

static double frames_per_tick_now(const mark_t *m) {
    if (clock_governs(m)) return m->frames_per_tick;
    float bpm = 120.0f;
    if (m->bpm_override > 0.0f) {
        bpm = m->bpm_override;
    } else if (m->host && m->host->get_bpm) {
        float b = m->host->get_bpm();
        if (b >= 20.0f && b <= 999.0f) bpm = b;
    }
    return (double)MARK_SR * 60.0 / ((double)bpm * 24.0);
}

static double frames_per_measure(const mark_t *m) {
    return frames_per_tick_now(m) * 96.0;   /* 4/4, 24 ppqn */
}

/* Quantization grid in frames: running clock > session grid unit > tempo. */
static double effective_grid(const mark_t *m) {
    if (m->clock_seen && m->clock_running) return frames_per_measure(m);
    if (m->grid_unit > 0.0) return m->grid_unit;
    return frames_per_measure(m);
}

/* rec_grid divisor: units per measure (4/4) */
static int grid_div(const mark_t *m) {
    static const int div[4] = { 1, 4, 8, 16 };
    return div[m->rec_grid & 3];
}

/* Record-length rounding unit, from the live grid. */
static double live_unit(const mark_t *m) {
    return effective_grid(m) / (double)grid_div(m);
}

/* Trim unit, locked to the session grid so edits are audio-exact. */
static double locked_unit(const mark_t *m) {
    double g = m->grid_unit > 0.0 ? m->grid_unit : frames_per_measure(m);
    return g / (double)grid_div(m);
}

/* Lowest-numbered track currently playing or overdubbing — its position
 * IS the live grid every pending action aligns to. */
static mk_track_t *base_playing(mark_t *m) {
    for (int i = 0; i < MARK_TRACKS; i++)
        if (m->t[i].len > 0 && (m->t[i].st == MK_PLAY || m->t[i].st == MK_DUB))
            return &m->t[i];
    return NULL;
}

static int any_content(const mark_t *m) {
    for (int i = 0; i < MARK_TRACKS; i++)
        if (m->t[i].len > 0 || m->t[i].st == MK_REC) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Create / destroy                                                   */
/* ------------------------------------------------------------------ */

mark_t *mark_create(const host_api_v1_t *host) {
    mark_t *m = calloc(1, sizeof(mark_t));
    if (!m) return NULL;

    for (int step = 0; step < ALLOC_STEPS; step++) {
        uint32_t frames = (uint32_t)alloc_seconds[step] * MARK_SR;
        int16_t *bufs[MARK_TRACKS + 1];
        int ok = 1;
        for (int i = 0; i <= MARK_TRACKS; i++) {
            bufs[i] = calloc((size_t)frames * 2, sizeof(int16_t));
            if (!bufs[i]) {
                for (int j = 0; j < i; j++) free(bufs[j]);
                ok = 0;
                break;
            }
        }
        if (ok) {
            m->track_frames = frames;
            for (int i = 0; i < MARK_TRACKS; i++) m->t[i].buf = bufs[i];
            m->undo_buf = bufs[MARK_TRACKS];
            break;
        }
    }
    if (!m->track_frames) { free(m); return NULL; }

    m->frames_per_tick = 918.75;   /* 120 BPM */
    m->undo_track = -1;
    m->swap_track = -1;
    m->quantize = 1;
    m->master = 100;
    m->master_g = 1.0f;
    m->monitor = 1;
    for (int i = 0; i < MARK_TRACKS; i++) {
        m->t[i].level = 100;
        m->t[i].pan = 50;
        m->t[i].g_cur = 1.0f;
        m->t[i].fx = TFX_LPF;      /* a sensible default once switched on */
        m->t[i].fxp = 50;
        m->t[i].fx_dirty = 1;
        m->t[i].dly = calloc((size_t)MARK_DLY_LEN * 2, sizeof(float));
        update_track_gains(&m->t[i]);
        if (!m->t[i].dly) {
            for (int j = 0; j <= i; j++) free(m->t[j].dly);
            for (int j = 0; j < MARK_TRACKS; j++) free(m->t[j].buf);
            free(m->undo_buf);
            free(m);
            return NULL;
        }
    }
    m->host = host;
    return m;
}

void mark_destroy(mark_t *m) {
    if (!m) return;
    /* a detached I/O worker may still hold this instance — wait it out
     * (bounded; a stuck filesystem beats a use-after-free) */
    for (int spin = 0; m->io_busy && spin < 20000; spin++) usleep(1000);
    for (int i = 0; i < MARK_TRACKS; i++) { free(m->t[i].buf); free(m->t[i].dly); }
    free(m->undo_buf);
    free(m);
}

/* ------------------------------------------------------------------ */
/*  MIDI (clock + transport)                                           */
/* ------------------------------------------------------------------ */

void mark_on_midi(mark_t *m, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!m || len < 1) return;
    switch (msg[0]) {
    case 0xFA: /* start */
    case 0xFB: /* continue: treated as downbeat too (pushnpull convention) */
        m->tick_total = 0;
        m->clock_running = 1;
        m->measure_flag = 1;
        if (m->transport_paused) {   /* resume every loop from its top */
            m->transport_paused = 0;
            for (int i = 0; i < MARK_TRACKS; i++)
                if (m->t[i].st == MK_PLAY || m->t[i].st == MK_DUB)
                    m->t[i].pos = 0.0;
        }
        break;
    case 0xFC:
        m->clock_running = 0;
        if (m->follow && base_playing(m)) m->transport_paused = 1;
        break;
    case 0xF8:
        if (m->clock_seen && m->global_frames > m->last_tick_global) {
            double d = (double)(m->global_frames - m->last_tick_global);
            if (d > 100.0 && d < 20000.0)
                m->frames_per_tick = 0.9 * m->frames_per_tick + 0.1 * d;
        }
        m->last_tick_global = m->global_frames;
        m->clock_seen = 1;
        m->tick_total++;
        if (m->tick_total % 96 == 0) m->measure_flag = 1;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/*  Track actions                                                      */
/* ------------------------------------------------------------------ */

static void reset_undo(mark_t *m) {
    m->undo_track = -1;
    m->undo_capturing = 0;
    m->undo_redo = 0;
    m->undo_count = 0;
    if (m->swap_track >= 0) m->swap_track = -1;
}

static void begin_dub_gesture(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    m->undo_track = ti;
    m->undo_kind = 0;
    m->undo_redo = 0;
    m->undo_capturing = 1;
    m->undo_start = (uint32_t)t->pos;
    m->undo_count = 0;
    m->swap_track = -1;
}

/* SINGLE play mode: when a track starts playing, every other playing
 * track stops — the RC's PLAY MODE SINGLE, i.e. song sections A-E. */
static void solo_stop_others(mark_t *m, int ti) {
    if (!m->play_mode) return;
    for (int i = 0; i < MARK_TRACKS; i++) {
        if (i == ti) continue;
        mk_track_t *o = &m->t[i];
        if (o->st == MK_PLAY || o->st == MK_DUB) {
            if (o->st == MK_DUB) m->undo_capturing = 0;
            o->st = MK_STOP;
            o->pos = 0.0;
        }
    }
}

static void apply_start_record(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    t->st = MK_REC;
    t->rec_len = 0;
    t->rec_target = 0;
    t->rec_end = MK_PLAY;
    t->pending = 0;
    m->edit_rev++;
}

static void apply_start_play(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    solo_stop_others(m, ti);
    t->st = MK_PLAY;
    t->pos = 0.0;
    t->pending = 0;
    m->edit_rev++;
}

static void finalize_record(mark_t *m, int ti, uint32_t final_len, int end_state) {
    mk_track_t *t = &m->t[ti];
    t->len = final_len;
    t->rec_target = 0;
    if (final_len == 0) {
        t->st = MK_EMPTY;
        return;
    }
    t->full_len = final_len;
    /* first finalized loop anchors the session grid for free-run sync.
     * Derive the MEASURE from the rounding unit actually used, so a
     * 15/16 polymetric first loop still anchors a true measure grid. */
    if (m->grid_unit <= 0.0) {
        double u = frames_per_measure(m) / (double)grid_div(m);
        double k = floor(((double)final_len / u) + 0.5);
        if (k < 1.0) k = 1.0;
        m->grid_unit = ((double)final_len / k) * (double)grid_div(m);
    }
    /* undo of a first recording = clear it (redo restores) */
    m->undo_track = ti;
    m->undo_kind = 1;
    m->undo_redo = 0;
    m->undo_capturing = 0;
    m->undo_saved_len = final_len;
    m->swap_track = -1;

    t->pos = 0.0;
    t->st = end_state;
    if (end_state == MK_DUB) {
        if (t->rev) t->st = MK_PLAY;    /* no overdub on reversed tracks */
        else begin_dub_gesture(m, ti);
    }
    if (t->st == MK_PLAY || t->st == MK_DUB) solo_stop_others(m, ti);
    m->edit_rev++;
}

/* Button-stop of an in-progress recording: quantize the length. If the
 * grid-rounded target is longer than what's recorded, keep recording
 * until the target (the RC records to the end of the measure); shorter
 * targets truncate immediately. */
static void request_finish(mark_t *m, int ti, int end_state) {
    mk_track_t *t = &m->t[ti];
    uint32_t target = t->rec_len;
    if (m->quantize) {
        double g = live_unit(m);
        if (g >= 32.0) {
            double k = floor(((double)t->rec_len / g) + 0.5);
            if (k < 1.0) k = 1.0;
            double f = k * g;
            while (f > (double)m->track_frames && k > 1.0) { k -= 1.0; f = k * g; }
            target = (uint32_t)(f + 0.5);   /* round: units can be x.5 frames */
        }
    }
    if (target > m->track_frames) target = m->track_frames;
    if (target == 0) {   /* stopped instantly: nothing worth keeping */
        t->st = MK_EMPTY;
        t->rec_len = 0;
        return;
    }
    if (target <= t->rec_len) {
        finalize_record(m, ti, target, end_state);
    } else {
        t->rec_target = target;
        t->rec_end = end_state;
    }
}

/* Schedule (or immediately apply) a quantized action. Immediate when
 * quantize is off, or when nothing is playing and the clock is stopped —
 * the RC's rule: quantize corrects timing only against a running rhythm,
 * a synced track, or MIDI sync. */
static void schedule(mark_t *m, int ti, int action /* 1 rec, 2 play */) {
    int quantized = m->quantize && (base_playing(m) != NULL || m->clock_running);
    if (!quantized) {
        if (action == 1) apply_start_record(m, ti);
        else apply_start_play(m, ti);
    } else {
        m->t[ti].pending = action;
    }
}

/* The RC's [>/o] button cycle for one track. */
static void track_button(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    if (m->swap_track == ti || m->io_busy) return;   /* swap or file I/O */
    if (t->pending) { t->pending = 0; m->edit_rev++; return; }
    switch (t->st) {
    case MK_EMPTY:
        schedule(m, ti, 1);
        break;
    case MK_REC:
        request_finish(m, ti, m->rec_action ? MK_DUB : MK_PLAY);
        break;
    case MK_PLAY:
        if (t->rev) break;                    /* no overdub on reversed tracks */
        t->st = MK_DUB;
        begin_dub_gesture(m, ti);
        m->edit_rev++;
        break;
    case MK_DUB:
        t->st = MK_PLAY;
        m->undo_capturing = 0;
        m->edit_rev++;
        break;
    case MK_STOP:
        schedule(m, ti, 2);
        break;
    default: break;
    }
}

static void track_stop(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    if (m->swap_track == ti || m->io_busy) return;
    if (t->pending) { t->pending = 0; m->edit_rev++; return; }
    switch (t->st) {
    case MK_REC:
        request_finish(m, ti, MK_STOP);
        break;
    case MK_PLAY:
    case MK_DUB:
        if (t->st == MK_DUB) m->undo_capturing = 0;
        t->st = MK_STOP;
        t->pos = 0.0;
        m->edit_rev++;
        break;
    default: break;
    }
}

/* Post-record loop trim ("effectively shortening the loop"): grow or
 * shrink len by one rec_grid unit, locked to the session grid. Trims
 * never exceed the finalized length; re-lengthening restores audio. */
static void track_trim(mark_t *m, int ti, int delta) {
    mk_track_t *t = &m->t[ti];
    if (m->io_busy || m->swap_track == ti) return;
    if (t->len == 0 || t->full_len == 0 || t->st == MK_REC) return;
    double u = locked_unit(m);
    if (u < 32.0) return;
    int cur = (int)floor((double)t->len / u + 0.5);
    int full = (int)floor((double)t->full_len / u + 0.5);
    if (cur < 1) cur = 1;
    if (full < 1) full = 1;
    int nu = clampi(cur + delta, 1, full);
    if (nu == cur) return;
    uint32_t nl = (uint32_t)((double)nu * u + 0.5);
    if (nl > t->full_len) nl = t->full_len;
    if (nl < 64) nl = 64;
    if (t->st == MK_DUB) {   /* dub write head would cross the new edge */
        t->st = MK_PLAY;
        m->undo_capturing = 0;
    }
    if (m->undo_track == ti) reset_undo(m);   /* indexes tied to old len */
    t->len = nl;
    if (t->pos >= (double)nl) t->pos = fmod(t->pos, (double)nl);
    m->edit_rev++;
}

/* Absolute length set in SIXTEENTHS of a measure — the step buttons'
 * unit (step 5 = a 5/16 loop), independent of rec_grid. */
static void track_setlen16(mark_t *m, int ti, int n16) {
    mk_track_t *t = &m->t[ti];
    if (m->io_busy || m->swap_track == ti) return;
    if (t->len == 0 || t->full_len == 0 || t->st == MK_REC) return;
    if (n16 < 1 || n16 > 16) return;
    double g = m->grid_unit > 0.0 ? m->grid_unit : frames_per_measure(m);
    double u = g / 16.0;
    if (u < 32.0) return;
    uint32_t nl = (uint32_t)((double)n16 * u + 0.5);
    if (nl > t->full_len) nl = t->full_len;
    if (nl < 64) nl = 64;
    if (nl == t->len) return;
    if (t->st == MK_DUB) {
        t->st = MK_PLAY;
        m->undo_capturing = 0;
    }
    if (m->undo_track == ti) reset_undo(m);
    t->len = nl;
    if (t->pos >= (double)nl) t->pos = fmod(t->pos, (double)nl);
    m->edit_rev++;
}

static void track_clear(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    if (m->io_busy) return;
    if (m->swap_track == ti) m->swap_track = -1;
    t->st = MK_EMPTY;
    t->len = 0;
    t->full_len = 0;
    t->rec_len = 0;
    t->rec_target = 0;
    t->pending = 0;
    t->pos = 0.0;
    if (m->undo_track == ti) reset_undo(m);
    if (!any_content(m)) {
        m->grid_unit = 0.0;
        m->transport_paused = 0;
    }
    m->edit_rev++;
}

/* One button: if anything plays, stop everything (and cancel pendings);
 * otherwise start every non-empty track together from the top — or just
 * the first one in SINGLE mode. */
static void all_button(mark_t *m) {
    if (m->io_busy) return;
    if (base_playing(m)) {
        for (int i = 0; i < MARK_TRACKS; i++) {
            mk_track_t *t = &m->t[i];
            t->pending = 0;
            if (t->st == MK_REC) request_finish(m, i, MK_STOP);
            else if (t->st == MK_PLAY || t->st == MK_DUB) {
                if (t->st == MK_DUB) m->undo_capturing = 0;
                t->st = MK_STOP;
                t->pos = 0.0;
            }
        }
        m->edit_rev++;
    } else {
        for (int i = 0; i < MARK_TRACKS; i++) {
            if (m->t[i].len > 0 && m->t[i].st == MK_STOP) {
                apply_start_play(m, i);
                if (m->play_mode) break;      /* single: sections are exclusive */
            }
        }
    }
}

static void undo_press(mark_t *m) {
    if (m->undo_track < 0 || m->swap_track >= 0 || m->io_busy) return;
    mk_track_t *t = &m->t[m->undo_track];
    if (m->undo_kind == 1) {
        if (!m->undo_redo) {          /* undo the first recording: clear */
            m->undo_saved_len = t->len;
            t->len = 0;
            t->st = MK_EMPTY;
            t->pos = 0.0;
            m->undo_redo = 1;
            m->edit_rev++;
            if (!any_content(m)) m->grid_unit = 0.0;
        } else {                      /* redo: bring it back, stopped */
            t->len = m->undo_saved_len;
            t->st = MK_STOP;
            t->pos = 0.0;
            m->undo_redo = 0;
            m->edit_rev++;
            if (m->grid_unit <= 0.0 && t->len) {
                double g = frames_per_measure(m);
                double k = floor(((double)t->len / g) + 0.5);
                if (k < 1.0) k = 1.0;
                m->grid_unit = (double)t->len / k;
            }
        }
        return;
    }
    /* kind 0: overdub — swap the captured region back in, incrementally.
     * Ignored while the same track is still overdubbing (leave dub first). */
    if (t->st == MK_DUB || t->len == 0 || m->undo_count == 0) return;
    m->undo_capturing = 0;
    m->swap_track = m->undo_track;
    m->swap_pos = 0;
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

/* Incremental undo swap: exchange track audio with the undo buffer over
 * the captured region, a bounded slice per block. The track is skipped
 * (muted) while the job runs; on completion undo becomes redo. */
static void swap_step(mark_t *m) {
    if (m->swap_track < 0) return;
    mk_track_t *t = &m->t[m->swap_track];
    uint32_t n = SWAP_FRAMES_PER_BLOCK;
    if (m->swap_pos + n > m->undo_count) n = m->undo_count - m->swap_pos;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = (m->undo_start + m->swap_pos + k) % t->len;
        int16_t tl = t->buf[idx * 2], tr = t->buf[idx * 2 + 1];
        t->buf[idx * 2]     = m->undo_buf[idx * 2];
        t->buf[idx * 2 + 1] = m->undo_buf[idx * 2 + 1];
        m->undo_buf[idx * 2]     = tl;
        m->undo_buf[idx * 2 + 1] = tr;
    }
    m->swap_pos += n;
    if (m->swap_pos >= m->undo_count) {
        m->swap_track = -1;
        m->undo_redo = !m->undo_redo;
        m->edit_rev++;
    }
}

/* ------------------------------------------------------------------ */
/*  Sessions (save/load loop audio + settings, worker thread)          */
/* ------------------------------------------------------------------ */

static int wav_write(const char *path, const int16_t *data, uint32_t frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data_bytes = frames * 4;
    uint32_t riff = 36 + data_bytes;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16;   memcpy(h + 16, &fmt_len, 4);
    uint16_t pcm = 1;        memcpy(h + 20, &pcm, 2);
    uint16_t ch = 2;         memcpy(h + 22, &ch, 2);
    uint32_t sr = MARK_SR;   memcpy(h + 24, &sr, 4);
    uint32_t br = MARK_SR * 4; memcpy(h + 28, &br, 4);
    uint16_t ba = 4;         memcpy(h + 32, &ba, 2);
    uint16_t bits = 16;      memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    int ok = fwrite(h, 1, 44, f) == 44 &&
             fwrite(data, 4, frames, f) == frames;
    fclose(f);
    return ok ? 0 : -1;
}

/* Minimal reader for the files wav_write produces (44.1k/16-bit/stereo);
 * walks chunks to find "data". Returns frames read or -1. */
static long wav_read(const char *path, int16_t *data, uint32_t max_frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f);
        return -1;
    }
    long frames = -1;
    uint8_t ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t sz;
        memcpy(&sz, ch + 4, 4);
        if (!memcmp(ch, "data", 4)) {
            uint32_t want = sz / 4;
            if (want > max_frames) want = max_frames;
            frames = (long)fread(data, 4, want, f);
            break;
        }
        if (fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR) != 0) break;
    }
    fclose(f);
    return frames;
}

typedef struct { mark_t *m; int slot; int op; /* 1 save, 2 load */ } mk_job_t;

static void session_path(const mark_t *m, int slot, const char *file,
                         char *buf, size_t len) {
    snprintf(buf, len, "%s/slot%02d%s%s", m->session_dir, slot,
             file ? "/" : "", file ? file : "");
}

static void *io_worker(void *arg) {
    mk_job_t *job = (mk_job_t *)arg;
    mark_t *m = job->m;
    char path[320];
    int err = 0;

    if (job->op == 1) {   /* ---- save ---- */
        mkdir(m->session_dir, 0755);
        session_path(m, job->slot, NULL, path, sizeof(path));
        mkdir(path, 0755);
        session_path(m, job->slot, "session.json", path, sizeof(path));
        FILE *f = fopen(path, "w");
        if (!f) {
            err = 1;
        } else {
            fprintf(f, "{\"v\":1,\"gu\":%ld,\"q\":%d,\"gd\":%d,\"ra\":%d,"
                       "\"dm\":%d,\"pm\":%d,\"mst\":%d,\"flw\":%d",
                    (long)(m->grid_unit + 0.5), m->quantize, m->rec_grid,
                    m->rec_action, m->dub_mode, m->play_mode, m->master,
                    m->follow);
            for (int i = 0; i < MARK_TRACKS; i++) {
                const mk_track_t *t = &m->t[i];
                fprintf(f, ",\"l%d\":%u,\"F%d\":%u,\"v%d\":%d,\"p%d\":%d,"
                           "\"r%d\":%d,\"s%d\":%d,\"f%d\":%d,\"o%d\":%d,"
                           "\"g%d\":%d",
                        i + 1, t->len, i + 1, t->full_len, i + 1, t->level,
                        i + 1, t->pan, i + 1, t->rev, i + 1, t->shot,
                        i + 1, t->fx, i + 1, t->fx_on, i + 1, t->fxp);
            }
            fprintf(f, "}");
            if (fclose(f) != 0) err = 1;
            for (int i = 0; i < MARK_TRACKS && !err; i++) {
                char wname[16];
                snprintf(wname, sizeof(wname), "t%d.wav", i + 1);
                session_path(m, job->slot, wname, path, sizeof(path));
                if (m->t[i].len > 0) {
                    /* write the FULL finalized loop — a trim is metadata,
                     * so re-lengthening still works after a reload */
                    uint32_t wf = m->t[i].full_len > m->t[i].len
                                    ? m->t[i].full_len : m->t[i].len;
                    if (wav_write(path, m->t[i].buf, wf) != 0) err = 1;
                } else {
                    unlink(path);   /* drop a stale wav from an older save */
                }
            }
        }
    } else {              /* ---- load ---- */
        char js[2048];
        session_path(m, job->slot, "session.json", path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) {
            err = 1;
        } else {
            size_t n = fread(js, 1, sizeof(js) - 1, f);
            js[n] = 0;
            fclose(f);
            long gu = json_int(js, "gu", 0);
            m->quantize   = json_int(js, "q", 1) ? 1 : 0;
            m->rec_grid   = clampi(json_int(js, "gd", 0), 0, 3);
            m->rec_action = json_int(js, "ra", 0) ? 1 : 0;
            m->dub_mode   = json_int(js, "dm", 0) ? 1 : 0;
            m->play_mode  = json_int(js, "pm", 0) ? 1 : 0;
            m->master     = clampi(json_int(js, "mst", 100), 0, 200);
            m->master_g   = (float)m->master / 100.0f;
            m->follow     = json_int(js, "flw", 0) ? 1 : 0;
            for (int i = 0; i < MARK_TRACKS; i++) {
                mk_track_t *t = &m->t[i];
                char k[8];
                snprintf(k, sizeof(k), "v%d", i + 1);
                t->level = clampi(json_int(js, k, 100), 0, 200);
                snprintf(k, sizeof(k), "p%d", i + 1);
                t->pan = clampi(json_int(js, k, 50), 0, 100);
                update_track_gains(t);
                snprintf(k, sizeof(k), "r%d", i + 1);
                t->rev = json_int(js, k, 0) ? 1 : 0;
                snprintf(k, sizeof(k), "s%d", i + 1);
                t->shot = json_int(js, k, 0) ? 1 : 0;
                snprintf(k, sizeof(k), "f%d", i + 1);
                t->fx = clampi(json_int(js, k, TFX_LPF), 0, TFX_COUNT - 1);
                snprintf(k, sizeof(k), "o%d", i + 1);
                t->fx_on = json_int(js, k, 0) ? 1 : 0;
                snprintf(k, sizeof(k), "g%d", i + 1);
                t->fxp = clampi(json_int(js, k, 50), 0, 100);
                t->fx_dirty = 1;

                snprintf(k, sizeof(k), "l%d", i + 1);
                long lval = json_int(js, k, 0);
                snprintf(k, sizeof(k), "F%d", i + 1);
                long want = json_int(js, k, lval);   /* full loop in the wav */
                if (want < lval) want = lval;
                if (want > (long)m->track_frames) want = (long)m->track_frames;
                if (want > 0) {
                    char wname[16];
                    snprintf(wname, sizeof(wname), "t%d.wav", i + 1);
                    session_path(m, job->slot, wname, path, sizeof(path));
                    long got = wav_read(path, t->buf, (uint32_t)want);
                    if (got > 0) {
                        t->full_len = (uint32_t)got;
                        t->len = (lval > 0 && lval <= got) ? (uint32_t)lval
                                                           : (uint32_t)got;
                        t->pos = 0.0;
                        t->st = MK_STOP;   /* set last: render gates on it */
                    }
                }
            }
            m->grid_unit = gu > 0 ? (double)gu : 0.0;
        }
    }

    m->io_error = err;
    m->edit_rev++;
    m->io_busy = 0;       /* release last */
    free(job);
    return NULL;
}

static void session_start(mark_t *m, int slot, int op) {
    if (m->io_busy || slot < 1 || slot > MARK_SESSION_SLOTS) return;
    if (!m->session_dir[0]) { m->io_error = 1; return; }
    if (op == 2) {
        /* silence + detach every track before the worker touches buffers */
        for (int i = 0; i < MARK_TRACKS; i++) {
            mk_track_t *t = &m->t[i];
            t->st = MK_EMPTY;
            t->len = 0;
            t->pos = 0.0;
            t->rec_len = 0;
            t->rec_target = 0;
            t->pending = 0;
        }
        reset_undo(m);
        m->grid_unit = 0.0;
    } else {
        /* saving while overdubbing/recording would race the writer — leave
         * dub, and finalize a recording at what's already down (no
         * extend-to-measure: that would keep writing during the save) */
        for (int i = 0; i < MARK_TRACKS; i++) {
            mk_track_t *t = &m->t[i];
            if (t->st == MK_DUB) {
                t->st = MK_PLAY;
                m->undo_capturing = 0;
            } else if (t->st == MK_REC) {
                uint32_t final_len = t->rec_len;
                double g = effective_grid(m);
                if (m->quantize && g >= 256.0 && (double)final_len >= g)
                    final_len = (uint32_t)(floor((double)final_len / g) * g);
                finalize_record(m, i, final_len, MK_PLAY);
            }
        }
    }
    mk_job_t *job = malloc(sizeof(mk_job_t));
    if (!job) { m->io_error = 1; return; }
    job->m = m;
    job->slot = slot;
    job->op = op;
    m->io_error = 0;
    m->io_busy = op;
    m->edit_rev++;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, io_worker, job) != 0) {
        m->io_busy = 0;
        m->io_error = 1;
        free(job);
    }
    pthread_attr_destroy(&at);
}

/* Track FX: one insert per track, applied post-read / pre-fader. Filter
 * coefficients recompute lazily on the render thread when fx_dirty. */
static void track_fx_prepare(mk_track_t *t) {
    if (!t->fx_dirty) return;
    t->fx_dirty = 0;
    switch (t->fx) {
    case TFX_LPF:
        bq_set(&t->bqL, 60.0f * powf(2.0f, (float)t->fxp * 0.075f), 0, 0.9f);
        t->bqR = t->bqL;
        t->bqL.z1 = t->bqL.z2 = t->bqR.z1 = t->bqR.z2 = 0.0f;
        break;
    case TFX_HPF:
        bq_set(&t->bqL, 40.0f * powf(2.0f, (float)t->fxp * 0.08f), 1, 0.9f);
        t->bqR = t->bqL;
        t->bqL.z1 = t->bqL.z2 = t->bqR.z1 = t->bqR.z2 = 0.0f;
        break;
    default: break;
    }
}

static void track_fx_run(mark_t *m, mk_track_t *t, float *l, float *r) {
    switch (t->fx) {
    case TFX_LPF:
    case TFX_HPF:
        *l = bq_run(&t->bqL, *l);
        *r = bq_run(&t->bqR, *r);
        break;
    case TFX_CRUSH: {
        /* sample-hold rate reduce + coarser quantize as param rises */
        int hold = 1 + (t->fxp * 30) / 100;              /* 1..31 */
        float quant = (float)(1 << (2 + t->fxp / 25));   /* 4..64 */
        if (--t->crush_cnt <= 0) {
            t->crush_cnt = hold;
            t->crush_l = floorf(*l / quant) * quant;
            t->crush_r = floorf(*r / quant) * quant;
        }
        *l = t->crush_l;
        *r = t->crush_r;
        break;
    }
    case TFX_DELAY: {
        /* tempo-synced 8th-note echo; param drives feedback + mix */
        uint32_t d = (uint32_t)(frames_per_measure(m) / 8.0);
        if (d >= MARK_DLY_LEN) d = MARK_DLY_LEN - 1;
        if (d < 64) d = 64;
        float fb = 0.15f + 0.55f * (float)t->fxp / 100.0f;
        float mix = 0.25f + 0.50f * (float)t->fxp / 100.0f;
        uint32_t rdi = (t->dly_w + MARK_DLY_LEN - d) % MARK_DLY_LEN;
        float dl = t->dly[rdi * 2], dr = t->dly[rdi * 2 + 1];
        t->dly[t->dly_w * 2]     = *l + dl * fb;
        t->dly[t->dly_w * 2 + 1] = *r + dr * fb;
        t->dly_w = (t->dly_w + 1) % MARK_DLY_LEN;
        *l += dl * mix;
        *r += dr * mix;
        break;
    }
    case TFX_PHASER: {
        /* 4-stage allpass, LFO rate from param (~0.05 .. ~2.5 Hz) */
        float rate = 0.05f + 2.45f * (float)t->fxp / 100.0f;
        t->ph_lfo += rate / (float)MARK_SR;
        if (t->ph_lfo >= 1.0f) t->ph_lfo -= 1.0f;
        float sweep = 0.55f + 0.4f * sinf(6.2831853f * t->ph_lfo);
        float in2[2] = { *l, *r };
        for (int c = 0; c < 2; c++) {
            float x = in2[c];
            for (int st = 0; st < 4; st++) {
                float y = -sweep * x + t->ph_x[c][st] + sweep * t->ph_y[c][st];
                t->ph_x[c][st] = x;
                t->ph_y[c][st] = y;
                x = y;
            }
            in2[c] = 0.5f * (in2[c] + x);
        }
        *l = in2[0];
        *r = in2[1];
        break;
    }
    case TFX_RINGMOD: {
        /* carrier ~30 Hz .. ~1 kHz, exponential */
        float freq = 30.0f * powf(2.0f, (float)t->fxp / 20.0f);
        t->rm_phase += freq / (float)MARK_SR;
        if (t->rm_phase >= 1.0f) t->rm_phase -= 1.0f;
        float c = sinf(6.2831853f * t->rm_phase);
        *l *= c;
        *r *= c;
        break;
    }
    default: break;
    }
}

void mark_process(mark_t *m, const int16_t *in, int16_t *out, int frames) {
    if (!m) return;

    swap_step(m);
    for (int i = 0; i < MARK_TRACKS; i++)
        if (m->t[i].fx_on) track_fx_prepare(&m->t[i]);

    int paused = m->transport_paused;
    int use_measure_flag = m->measure_flag;
    m->measure_flag = 0;

    for (int n = 0; n < frames; n++) {
        float inl = (float)in[n * 2], inr = (float)in[n * 2 + 1];

        /* --- pending scheduler: fire on the grid --- */
        int boundary = 0;
        mk_track_t *base = base_playing(m);
        if (base && !paused) {
            double g = m->grid_unit > 0.0 ? m->grid_unit : frames_per_measure(m);
            if (g >= 1.0 && fmod(base->pos, g) < 1.0) boundary = 1;
        } else if (use_measure_flag && n == 0) {
            boundary = 1;
        }
        if (boundary) {
            for (int i = 0; i < MARK_TRACKS; i++) {
                if (m->t[i].pending == 1) apply_start_record(m, i);
                else if (m->t[i].pending == 2) apply_start_play(m, i);
            }
        }

        /* --- render/record each track --- */
        float outl = 0.0f, outr = 0.0f;
        for (int i = 0; i < MARK_TRACKS; i++) {
            mk_track_t *t = &m->t[i];

            if (t->st == MK_REC) {
                if (t->rec_len < m->track_frames) {
                    t->buf[t->rec_len * 2]     = in[n * 2];
                    t->buf[t->rec_len * 2 + 1] = in[n * 2 + 1];
                    t->rec_len++;
                }
                if (t->rec_target && t->rec_len >= t->rec_target) {
                    finalize_record(m, i, t->rec_target, t->rec_end);
                } else if (t->rec_len >= m->track_frames) {
                    /* buffer full: keep whole grid multiples if we have any */
                    uint32_t final_len = m->track_frames;
                    double g = live_unit(m);
                    if (m->quantize && g >= 32.0 && (double)final_len >= g)
                        final_len = (uint32_t)(floor((double)final_len / g) * g);
                    finalize_record(m, i, final_len, m->rec_action ? MK_DUB : MK_PLAY);
                }
                continue;
            }

            if ((t->st != MK_PLAY && t->st != MK_DUB) || t->len == 0 || paused)
                continue;
            if (m->swap_track == i)   /* undo swap in flight: track muted */
                continue;

            uint32_t ip = (uint32_t)t->pos;
            if (ip >= t->len) ip = t->len - 1;
            uint32_t rp = t->rev ? (t->len - 1 - ip) : ip;
            float l = (float)t->buf[rp * 2];
            float r = (float)t->buf[rp * 2 + 1];

            if (t->st == MK_DUB) {
                /* capture-before-write; writes run sequentially from the
                 * gesture start, so the captured region stays contiguous */
                if (m->undo_capturing && m->undo_track == i &&
                    m->undo_count < t->len) {
                    m->undo_buf[ip * 2]     = t->buf[ip * 2];
                    m->undo_buf[ip * 2 + 1] = t->buf[ip * 2 + 1];
                    m->undo_count++;
                }
                if (m->dub_mode) {   /* replace */
                    t->buf[ip * 2]     = in[n * 2];
                    t->buf[ip * 2 + 1] = in[n * 2 + 1];
                } else {             /* overdub: layer on top */
                    t->buf[ip * 2]     = clip16((float)t->buf[ip * 2] + inl);
                    t->buf[ip * 2 + 1] = clip16((float)t->buf[ip * 2 + 1] + inr);
                }
            }

            if (t->fx_on && t->fx != TFX_OFF) track_fx_run(m, t, &l, &r);

            float gt = (float)t->level / 100.0f;
            t->g_cur += (gt - t->g_cur) * 0.002f;
            outl += l * t->g_cur * t->pg[0];
            outr += r * t->g_cur * t->pg[1];

            t->pos += 1.0;
            if (t->pos >= (double)t->len) {
                t->pos = 0.0;
                if (t->shot) {       /* one-shot: play once, then stop */
                    if (t->st == MK_DUB) m->undo_capturing = 0;
                    t->st = MK_STOP;
                }
            }
        }

        float dry = m->monitor ? 1.0f : 0.0f;
        out[n * 2]     = clip16(outl * m->master_g + inl * dry);
        out[n * 2 + 1] = clip16(outr * m->master_g + inr * dry);
    }
    m->global_frames += (uint64_t)frames;
}

/* ------------------------------------------------------------------ */
/*  Params                                                             */
/* ------------------------------------------------------------------ */

void mark_set_param(mark_t *m, const char *key, const char *val) {
    if (!m || !key || !val) return;

    /* per-track keys: t<1-5>_... */
    if (key[0] == 't' && key[1] >= '1' && key[1] <= '5' && key[2] == '_') {
        int ti = key[1] - '1';
        const char *k = key + 3;
        mk_track_t *t = &m->t[ti];
        if (!strcmp(k, "btn"))   { if (trig_active(val)) track_button(m, ti); return; }
        if (!strcmp(k, "stop"))  { if (trig_active(val)) track_stop(m, ti);   return; }
        if (!strcmp(k, "clear")) { if (trig_active(val)) track_clear(m, ti);  return; }
        if (!strcmp(k, "trim"))  { track_trim(m, ti, atoi(val)); return; }
        if (!strcmp(k, "len16")) { track_setlen16(m, ti, atoi(val)); return; }
        if (!strcmp(k, "level")) { t->level = clampi(atoi(val), 0, 200); m->edit_rev++; return; }
        if (!strcmp(k, "pan"))   { t->pan = clampi(atoi(val), 0, 100); update_track_gains(t); m->edit_rev++; return; }
        if (!strcmp(k, "rev")) {
            t->rev = atoi(val) ? 1 : 0;
            if (t->rev && t->st == MK_DUB) {   /* RC rule: no dub while reversed */
                t->st = MK_PLAY;
                m->undo_capturing = 0;
            }
            m->edit_rev++;
            return;
        }
        if (!strcmp(k, "shot"))  { t->shot = atoi(val) ? 1 : 0; m->edit_rev++; return; }
        if (!strcmp(k, "fx")) {
            t->fx = clampi(atoi(val), 0, TFX_COUNT - 1);
            t->fx_dirty = 1;
            m->edit_rev++;
            return;
        }
        if (!strcmp(k, "fx_on")) { t->fx_on = atoi(val) ? 1 : 0; t->fx_dirty = 1; m->edit_rev++; return; }
        if (!strcmp(k, "fxp"))   { t->fxp = clampi(atoi(val), 0, 100); t->fx_dirty = 1; m->edit_rev++; return; }
        return;
    }

    if (!strcmp(key, "all_btn"))  { if (trig_active(val)) all_button(m); return; }
    if (!strcmp(key, "undo"))     { if (trig_active(val)) undo_press(m); return; }
    if (!strcmp(key, "quantize")) { m->quantize = atoi(val) ? 1 : 0; m->edit_rev++; return; }
    if (!strcmp(key, "rec_grid")) { m->rec_grid = clampi(atoi(val), 0, 3); m->edit_rev++; return; }
    if (!strcmp(key, "rec_action")) { m->rec_action = atoi(val) ? 1 : 0; m->edit_rev++; return; }
    if (!strcmp(key, "dub_mode")) { m->dub_mode = atoi(val) ? 1 : 0; m->edit_rev++; return; }
    if (!strcmp(key, "play_mode")) { m->play_mode = atoi(val) ? 1 : 0; m->edit_rev++; return; }
    if (!strcmp(key, "master"))   { m->master = clampi(atoi(val), 0, 200);
                                    m->master_g = (float)m->master / 100.0f;
                                    m->edit_rev++; return; }
    if (!strcmp(key, "monitor"))  { m->monitor = atoi(val) ? 1 : 0; m->edit_rev++; return; }
    if (!strcmp(key, "hw_input")) { m->hw_input = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "follow")) {
        m->follow = atoi(val) ? 1 : 0;
        if (!m->follow) m->transport_paused = 0;
        m->edit_rev++;
        return;
    }
    if (!strcmp(key, "bpm_override")) {
        float b = (float)atof(val);
        m->bpm_override = (b >= 20.0f && b <= 999.0f) ? b : 0.0f;
        m->edit_rev++;
        return;
    }
    if (!strcmp(key, "session_dir")) {
        snprintf(m->session_dir, sizeof(m->session_dir), "%s", val);
        return;
    }
    if (!strcmp(key, "save_session")) {
        int slot = atoi(val);
        if (slot >= 1) session_start(m, slot, 1);
        return;
    }
    if (!strcmp(key, "load_session")) {
        int slot = atoi(val);
        if (slot >= 1) session_start(m, slot, 2);
        return;
    }
    if (!strcmp(key, "delete_session")) {
        /* deletion is a handful of unlinks — safe synchronously on the UI
         * thread, but never while a save/load worker owns the directory */
        int slot = atoi(val);
        if (slot < 1 || slot > MARK_SESSION_SLOTS || m->io_busy ||
            !m->session_dir[0])
            return;
        char path[320];
        session_path(m, slot, "session.json", path, sizeof(path));
        unlink(path);
        for (int i = 0; i < MARK_TRACKS; i++) {
            char wname[16];
            snprintf(wname, sizeof(wname), "t%d.wav", i + 1);
            session_path(m, slot, wname, path, sizeof(path));
            unlink(path);
        }
        session_path(m, slot, NULL, path, sizeof(path));
        rmdir(path);
        m->edit_rev++;
        return;
    }

    if (!strcmp(key, "state")) {
        /* settings-only preset blob; loop audio is never saved. Display
         * fields the getter emits (run/ts/ms/un/sess/bpm/sec) are ignored
         * here on purpose. */
        m->quantize   = json_int(val, "q",  m->quantize) ? 1 : 0;
        m->rec_grid   = clampi(json_int(val, "gd", m->rec_grid), 0, 3);
        m->rec_action = json_int(val, "ra", m->rec_action) ? 1 : 0;
        m->dub_mode   = json_int(val, "dm", m->dub_mode) ? 1 : 0;
        m->play_mode  = json_int(val, "pm", m->play_mode) ? 1 : 0;
        m->master     = clampi(json_int(val, "mst", m->master), 0, 200);
        m->master_g   = (float)m->master / 100.0f;
        m->follow     = json_int(val, "flw", m->follow) ? 1 : 0;
        const char *arr;
        struct { const char *key; int which; } fields[] = {
            { "\"lv\":\"", 0 }, { "\"pn\":\"", 1 },
            { "\"rv\":\"", 2 }, { "\"sh\":\"", 3 },
            { "\"fx\":\"", 4 }, { "\"fo\":\"", 5 }, { "\"fp\":\"", 6 },
        };
        for (int f = 0; f < 7; f++) {
            arr = strstr(val, fields[f].key);
            if (!arr) continue;
            arr += strlen(fields[f].key);
            for (int i = 0; i < MARK_TRACKS && *arr && *arr != '"'; i++) {
                char *end;
                long v = strtol(arr, &end, 10);
                if (end == arr) break;
                mk_track_t *t = &m->t[i];
                switch (fields[f].which) {
                case 0: t->level = clampi((int)v, 0, 200); break;
                case 1: t->pan = clampi((int)v, 0, 100); update_track_gains(t); break;
                case 2: t->rev = v ? 1 : 0; break;
                case 3: t->shot = v ? 1 : 0; break;
                case 4: t->fx = clampi((int)v, 0, TFX_COUNT - 1); t->fx_dirty = 1; break;
                case 5: t->fx_on = v ? 1 : 0; t->fx_dirty = 1; break;
                case 6: t->fxp = clampi((int)v, 0, 100); t->fx_dirty = 1; break;
                }
                arr = (*end == ',') ? end + 1 : end;
            }
        }
        m->edit_rev++;
        return;
    }
}

/* csv of one per-track int, appended into a state blob or returned raw */
static int track_csv(const mark_t *m, char *buf, int buf_len,
                     int (*get)(const mk_track_t *)) {
    int n = 0;
    for (int i = 0; i < MARK_TRACKS && n < buf_len - 8; i++)
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                i ? "," : "", get(&m->t[i])), buf_len);
    return n;
}

static int g_level(const mk_track_t *t) { return t->level; }
static int g_pan(const mk_track_t *t)   { return t->pan; }
static int g_rev(const mk_track_t *t)   { return t->rev; }
static int g_shot(const mk_track_t *t)  { return t->shot; }
static int g_state(const mk_track_t *t) { return t->st; }
static int g_pend(const mk_track_t *t)  { return t->pending; }
static int g_fx(const mk_track_t *t)    { return t->fx; }
static int g_fxon(const mk_track_t *t)  { return t->fx_on; }
static int g_fxp(const mk_track_t *t)   { return t->fxp; }

int mark_get_param(mark_t *m, const char *key, char *buf, int buf_len) {
    if (!m || !key || !buf || buf_len < 2) return -1;

    if (key[0] == 't' && key[1] >= '1' && key[1] <= '5' && key[2] == '_') {
        int ti = key[1] - '1';
        const char *k = key + 3;
        const mk_track_t *t = &m->t[ti];
        if (!strcmp(k, "level")) return snprintf(buf, (size_t)buf_len, "%d", t->level);
        if (!strcmp(k, "pan"))   return snprintf(buf, (size_t)buf_len, "%d", t->pan);
        if (!strcmp(k, "rev"))   return snprintf(buf, (size_t)buf_len, "%d", t->rev);
        if (!strcmp(k, "shot"))  return snprintf(buf, (size_t)buf_len, "%d", t->shot);
        if (!strcmp(k, "len"))   return snprintf(buf, (size_t)buf_len, "%u", t->len);
        if (!strcmp(k, "state")) return snprintf(buf, (size_t)buf_len, "%d", t->st);
        if (!strcmp(k, "fx"))    return snprintf(buf, (size_t)buf_len, "%d", t->fx);
        if (!strcmp(k, "fx_on")) return snprintf(buf, (size_t)buf_len, "%d", t->fx_on);
        if (!strcmp(k, "fxp"))   return snprintf(buf, (size_t)buf_len, "%d", t->fxp);
        /* trigger params read back inactive */
        if (!strcmp(k, "btn") || !strcmp(k, "stop") || !strcmp(k, "clear"))
            return snprintf(buf, (size_t)buf_len, "0");
        return -1;
    }

    if (!strcmp(key, "tstates")) return track_csv(m, buf, buf_len, g_state);
    if (!strcmp(key, "tpend"))   return track_csv(m, buf, buf_len, g_pend);
    if (!strcmp(key, "tpos")) {
        int n = 0;
        for (int i = 0; i < MARK_TRACKS && n < buf_len - 8; i++) {
            int v = 0;
            if (m->t[i].len > 0)
                v = (int)(m->t[i].pos * 128.0 / (double)m->t[i].len);
            if (v > 127) v = 127;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", v), buf_len);
        }
        return n;
    }
    if (!strcmp(key, "tmeas")) {
        double g = m->grid_unit > 0.0 ? m->grid_unit : frames_per_measure(m);
        int n = 0;
        for (int i = 0; i < MARK_TRACKS && n < buf_len - 8; i++) {
            int meas = 0;
            if (m->t[i].len > 0 && g >= 1.0)
                meas = (int)floor(((double)m->t[i].len / g) + 0.5);
            if (m->t[i].len > 0 && meas < 1) meas = 1;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", meas), buf_len);
        }
        return n;
    }

    /* one poll per UI tick: states|pending|pos|undo|swap */
    if (!strcmp(key, "status")) {
        int n = 0;
        n = nclamp(n + track_csv(m, buf + n, buf_len - n, g_state), buf_len);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "|"), buf_len);
        n = nclamp(n + track_csv(m, buf + n, buf_len - n, g_pend), buf_len);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "|"), buf_len);
        for (int i = 0; i < MARK_TRACKS; i++) {
            int v = 0;
            if (m->t[i].len > 0)
                v = (int)(m->t[i].pos * 128.0 / (double)m->t[i].len);
            if (v > 127) v = 127;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", v), buf_len);
        }
        int undo_avail = 0;
        if (m->undo_track >= 0 && m->swap_track < 0)
            undo_avail = m->undo_redo ? 2 : 1;
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "|%d|%d|",
                                undo_avail, m->swap_track >= 0 ? 1 : 0), buf_len);
        if (n < buf_len - 16) {
            int w = mark_get_param(m, "tunits", buf + n, buf_len - n);
            if (w > 0) n = nclamp(n + w, buf_len);
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "|"), buf_len);
        if (n < buf_len - 16) {
            int w = mark_get_param(m, "tlen16", buf + n, buf_len - n);
            if (w > 0) n = nclamp(n + w, buf_len);
        }
        return n;
    }

    if (!strcmp(key, "run_state")) {
        int rs = 0;
        for (int i = 0; i < MARK_TRACKS; i++) {
            if (m->t[i].st == MK_REC || m->t[i].st == MK_DUB) { rs = 2; break; }
            if (m->t[i].st == MK_PLAY) rs = 3;
            else if (m->t[i].pending && rs == 0) rs = 1;
        }
        return snprintf(buf, (size_t)buf_len, "%d", rs);
    }
    if (!strcmp(key, "quantize"))   return snprintf(buf, (size_t)buf_len, "%d", m->quantize);
    if (!strcmp(key, "rec_grid"))   return snprintf(buf, (size_t)buf_len, "%d", m->rec_grid);
    if (!strcmp(key, "tunits")) {
        /* per-track length in current rec_grid units (0 = empty) */
        double u = locked_unit(m);
        int n = 0;
        for (int i = 0; i < MARK_TRACKS && n < buf_len - 8; i++) {
            int v = 0;
            if (m->t[i].len > 0 && u >= 32.0) {
                v = (int)floor((double)m->t[i].len / u + 0.5);
                if (v < 1) v = 1;
            }
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", v), buf_len);
        }
        return n;
    }
    if (!strcmp(key, "tlen16")) {
        /* per-track length in 16ths of a measure — the step buttons' unit */
        double g = m->grid_unit > 0.0 ? m->grid_unit : frames_per_measure(m);
        double u = g / 16.0;
        int n = 0;
        for (int i = 0; i < MARK_TRACKS && n < buf_len - 8; i++) {
            int v = 0;
            if (m->t[i].len > 0 && u >= 32.0) {
                v = (int)floor((double)m->t[i].len / u + 0.5);
                if (v < 1) v = 1;
                if (v > 999) v = 999;
            }
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", v), buf_len);
        }
        return n;
    }
    if (!strcmp(key, "rec_action")) return snprintf(buf, (size_t)buf_len, "%d", m->rec_action);
    if (!strcmp(key, "dub_mode"))   return snprintf(buf, (size_t)buf_len, "%d", m->dub_mode);
    if (!strcmp(key, "play_mode"))  return snprintf(buf, (size_t)buf_len, "%d", m->play_mode);
    if (!strcmp(key, "edit_rev"))   return snprintf(buf, (size_t)buf_len, "%u", m->edit_rev);
    if (!strcmp(key, "session_status")) {
        const char *s = m->io_busy == 1 ? "saving"
                      : m->io_busy == 2 ? "loading"
                      : m->io_error ? "error" : "idle";
        return snprintf(buf, (size_t)buf_len, "%s", s);
    }
    if (!strcmp(key, "session_slots")) {
        /* csv of 0/1 per slot; stat() runs on the UI thread, not audio */
        int n = 0;
        for (int i = 1; i <= MARK_SESSION_SLOTS && n < buf_len - 4; i++) {
            int have = 0;
            if (m->session_dir[0]) {
                char path[320];
                struct stat st;
                session_path(m, i, "session.json", path, sizeof(path));
                have = stat(path, &st) == 0;
            }
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i > 1 ? "," : "", have), buf_len);
        }
        return n;
    }
    if (!strcmp(key, "rui_poll")) {
        /* schwung-manager Remote-UI poll digest "rev:on:tick:bpm"
         * (remote_ui.go parseRuiPoll): rev gates the heavy full-state
         * refetch, tick drives the browser playhead between edits. */
        mk_track_t *base = base_playing(m);
        int on = (base && !m->transport_paused) ? 1 : 0;
        int tick = -1;
        if (on && base->len > 0) {
            tick = (int)(base->pos * 128.0 / (double)base->len);
            if (tick > 127) tick = 127;
        }
        double fpt = frames_per_tick_now(m);
        int bpm = fpt > 0.0
            ? (int)((double)MARK_SR * 60.0 / (fpt * 24.0) + 0.5) : 0;
        return snprintf(buf, (size_t)buf_len, "%u:%d:%d:%d",
                        m->edit_rev, on, tick, bpm);
    }
    if (!strcmp(key, "master"))     return snprintf(buf, (size_t)buf_len, "%d", m->master);
    if (!strcmp(key, "monitor"))    return snprintf(buf, (size_t)buf_len, "%d", m->monitor);
    if (!strcmp(key, "hw_input"))   return snprintf(buf, (size_t)buf_len, "%d", m->hw_input);
    if (!strcmp(key, "follow"))     return snprintf(buf, (size_t)buf_len, "%d", m->follow);
    if (!strcmp(key, "bpm_override"))
        return snprintf(buf, (size_t)buf_len, "%d", (int)(m->bpm_override + 0.5f));
    if (!strcmp(key, "grid_bpm")) {
        double fpt = frames_per_tick_now(m);
        return snprintf(buf, (size_t)buf_len, "%d",
                        (int)((double)MARK_SR * 60.0 / (fpt * 24.0) + 0.5));
    }
    if (!strcmp(key, "track_seconds"))
        return snprintf(buf, (size_t)buf_len, "%u", m->track_frames / MARK_SR);
    if (!strcmp(key, "undo_avail")) {
        int v = 0;
        if (m->undo_track >= 0 && m->swap_track < 0) v = m->undo_redo ? 2 : 1;
        return snprintf(buf, (size_t)buf_len, "%d", v);
    }
    if (!strcmp(key, "all_btn") || !strcmp(key, "undo"))
        return snprintf(buf, (size_t)buf_len, "0");

    if (!strcmp(key, "state")) {
        /* settings (restored by the setter) + read-only display fields for
         * the browser editor (run/ts/ms/un/io/sess/bpm/sec — the setter
         * ignores them). The manager flattens this JSON into M.* keys. */
        int n = 0;
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                "{\"q\":%d,\"gd\":%d,\"ra\":%d,\"dm\":%d,\"pm\":%d,"
                                "\"mst\":%d,\"flw\":%d",
                                m->quantize, m->rec_grid, m->rec_action,
                                m->dub_mode, m->play_mode, m->master,
                                m->follow), buf_len);
        struct { const char *key; int (*get)(const mk_track_t *); } arrs[] = {
            { "lv", g_level }, { "pn", g_pan }, { "rv", g_rev }, { "sh", g_shot },
            { "fx", g_fx }, { "fo", g_fxon }, { "fp", g_fxp }, { "ts", g_state },
        };
        for (int f = 0; f < 8; f++) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%s\":\"", arrs[f].key), buf_len);
            n = nclamp(n + track_csv(m, buf + n, buf_len - n, arrs[f].get), buf_len);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), buf_len);
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), ",\"ms\":\""), buf_len);
        if (n < buf_len - 24) {
            int w = mark_get_param(m, "tmeas", buf + n, buf_len - n);
            if (w > 0) n = nclamp(n + w, buf_len);
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\",\"tu\":\""), buf_len);
        if (n < buf_len - 24) {
            int w = mark_get_param(m, "tunits", buf + n, buf_len - n);
            if (w > 0) n = nclamp(n + w, buf_len);
        }
        int run = 0;
        for (int i = 0; i < MARK_TRACKS; i++) {
            if (m->t[i].st == MK_REC || m->t[i].st == MK_DUB) { run = 2; break; }
            if (m->t[i].st == MK_PLAY) run = 3;
        }
        int undo_avail = 0;
        if (m->undo_track >= 0 && m->swap_track < 0)
            undo_avail = m->undo_redo ? 2 : 1;
        double fpt = frames_per_tick_now(m);
        int bpm = fpt > 0.0
            ? (int)((double)MARK_SR * 60.0 / (fpt * 24.0) + 0.5) : 0;
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                "\",\"run\":%d,\"un\":%d,\"io\":%d,\"mon\":%d,"
                                "\"bpm\":%d,\"sec\":%u,\"sess\":\"",
                                run, undo_avail, m->io_busy, m->monitor,
                                bpm, m->track_frames / MARK_SR), buf_len);
        if (n < buf_len - 40) {
            int w = mark_get_param(m, "session_slots", buf + n, buf_len - n);
            if (w > 0) n = nclamp(n + w, buf_len);
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\"}"), buf_len);
        return n;
    }

    return -1;
}
