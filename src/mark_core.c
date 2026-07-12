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

#include "mark_core.h"

/* Allocation fallback ladder: first capacity that calloc grants wins. */
static const int alloc_seconds[] = { MARK_MAX_SECONDS, 45, 30, 20, 15 };
#define ALLOC_STEPS 5

/* Incremental undo-swap budget per 128-frame block. 32768 frames is ~0.5 MB
 * of traffic — comfortably inside the render budget, and a full 60 s swap
 * completes in ~81 blocks (~0.24 s) with the track muted. */
#define SWAP_FRAMES_PER_BLOCK 32768

typedef struct {
    int16_t *buf;          /* track_frames * 2, interleaved */
    uint32_t len;          /* finalized loop frames, 0 = empty */
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
    int   quantize;              /* 0 off, 1 measure (default) */
    int   rec_action;            /* 0 rec->play (default), 1 rec->dub */
    int   dub_mode;              /* 0 overdub (default), 1 replace */
    int   master;                /* 0..200 */
    float master_g;
    int   monitor;               /* 0 = mute live input at the output */
    int   hw_input;              /* set by the gen wrapper */
    int   follow;                /* 1 = Move transport stop pauses loops */
    int   transport_paused;
    float bpm_override;          /* free-run tempo; 0 = project tempo */
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
        update_track_gains(&m->t[i]);
    }
    m->host = host;
    return m;
}

void mark_destroy(mark_t *m) {
    if (!m) return;
    for (int i = 0; i < MARK_TRACKS; i++) free(m->t[i].buf);
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

static void apply_start_record(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    t->st = MK_REC;
    t->rec_len = 0;
    t->rec_target = 0;
    t->rec_end = MK_PLAY;
    t->pending = 0;
}

static void apply_start_play(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    (void)m;
    t->st = MK_PLAY;
    t->pos = 0.0;
    t->pending = 0;
}

static void finalize_record(mark_t *m, int ti, uint32_t final_len, int end_state) {
    mk_track_t *t = &m->t[ti];
    t->len = final_len;
    t->rec_target = 0;
    if (final_len == 0) {
        t->st = MK_EMPTY;
        return;
    }
    /* first finalized loop anchors the session grid for free-run sync */
    if (m->grid_unit <= 0.0) {
        double g = frames_per_measure(m);
        double k = floor(((double)final_len / g) + 0.5);
        if (k < 1.0) k = 1.0;
        m->grid_unit = (double)final_len / k;
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
}

/* Button-stop of an in-progress recording: quantize the length. If the
 * grid-rounded target is longer than what's recorded, keep recording
 * until the target (the RC records to the end of the measure); shorter
 * targets truncate immediately. */
static void request_finish(mark_t *m, int ti, int end_state) {
    mk_track_t *t = &m->t[ti];
    uint32_t target = t->rec_len;
    if (m->quantize) {
        double g = effective_grid(m);
        if (g >= 256.0) {
            double k = floor(((double)t->rec_len / g) + 0.5);
            if (k < 1.0) k = 1.0;
            double f = k * g;
            while (f > (double)m->track_frames && k > 1.0) { k -= 1.0; f = k * g; }
            target = (uint32_t)f;
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
    if (m->swap_track == ti) return;          /* undo swap in flight */
    if (t->pending) { t->pending = 0; return; }  /* cancel a scheduled action */
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
        break;
    case MK_DUB:
        t->st = MK_PLAY;
        m->undo_capturing = 0;
        break;
    case MK_STOP:
        schedule(m, ti, 2);
        break;
    default: break;
    }
}

static void track_stop(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    if (m->swap_track == ti) return;
    if (t->pending) { t->pending = 0; return; }
    switch (t->st) {
    case MK_REC:
        request_finish(m, ti, MK_STOP);
        break;
    case MK_PLAY:
    case MK_DUB:
        if (t->st == MK_DUB) m->undo_capturing = 0;
        t->st = MK_STOP;
        t->pos = 0.0;
        break;
    default: break;
    }
}

static void track_clear(mark_t *m, int ti) {
    mk_track_t *t = &m->t[ti];
    if (m->swap_track == ti) m->swap_track = -1;
    t->st = MK_EMPTY;
    t->len = 0;
    t->rec_len = 0;
    t->rec_target = 0;
    t->pending = 0;
    t->pos = 0.0;
    if (m->undo_track == ti) reset_undo(m);
    if (!any_content(m)) {
        m->grid_unit = 0.0;
        m->transport_paused = 0;
    }
}

/* One button: if anything plays, stop everything (and cancel pendings);
 * otherwise start every non-empty track together from the top. */
static void all_button(mark_t *m) {
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
    } else {
        for (int i = 0; i < MARK_TRACKS; i++)
            if (m->t[i].len > 0 && m->t[i].st == MK_STOP)
                apply_start_play(m, i);
    }
}

static void undo_press(mark_t *m) {
    if (m->undo_track < 0 || m->swap_track >= 0) return;
    mk_track_t *t = &m->t[m->undo_track];
    if (m->undo_kind == 1) {
        if (!m->undo_redo) {          /* undo the first recording: clear */
            m->undo_saved_len = t->len;
            t->len = 0;
            t->st = MK_EMPTY;
            t->pos = 0.0;
            m->undo_redo = 1;
            if (!any_content(m)) m->grid_unit = 0.0;
        } else {                      /* redo: bring it back, stopped */
            t->len = m->undo_saved_len;
            t->st = MK_STOP;
            t->pos = 0.0;
            m->undo_redo = 0;
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
    }
}

void mark_process(mark_t *m, const int16_t *in, int16_t *out, int frames) {
    if (!m) return;

    swap_step(m);

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
                    double g = effective_grid(m);
                    if (m->quantize && g >= 256.0 && (double)final_len >= g)
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
        if (!strcmp(k, "level")) { t->level = clampi(atoi(val), 0, 200); return; }
        if (!strcmp(k, "pan"))   { t->pan = clampi(atoi(val), 0, 100); update_track_gains(t); return; }
        if (!strcmp(k, "rev")) {
            t->rev = atoi(val) ? 1 : 0;
            if (t->rev && t->st == MK_DUB) {   /* RC rule: no dub while reversed */
                t->st = MK_PLAY;
                m->undo_capturing = 0;
            }
            return;
        }
        if (!strcmp(k, "shot"))  { t->shot = atoi(val) ? 1 : 0; return; }
        return;
    }

    if (!strcmp(key, "all_btn"))  { if (trig_active(val)) all_button(m); return; }
    if (!strcmp(key, "undo"))     { if (trig_active(val)) undo_press(m); return; }
    if (!strcmp(key, "quantize")) { m->quantize = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "rec_action")) { m->rec_action = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "dub_mode")) { m->dub_mode = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "master"))   { m->master = clampi(atoi(val), 0, 200);
                                    m->master_g = (float)m->master / 100.0f; return; }
    if (!strcmp(key, "monitor"))  { m->monitor = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "hw_input")) { m->hw_input = atoi(val) ? 1 : 0; return; }
    if (!strcmp(key, "follow")) {
        m->follow = atoi(val) ? 1 : 0;
        if (!m->follow) m->transport_paused = 0;
        return;
    }
    if (!strcmp(key, "bpm_override")) {
        float b = (float)atof(val);
        m->bpm_override = (b >= 20.0f && b <= 999.0f) ? b : 0.0f;
        return;
    }

    if (!strcmp(key, "state")) {
        /* settings-only preset blob; loop audio is never saved */
        m->quantize   = json_int(val, "q",  m->quantize) ? 1 : 0;
        m->rec_action = json_int(val, "ra", m->rec_action) ? 1 : 0;
        m->dub_mode   = json_int(val, "dm", m->dub_mode) ? 1 : 0;
        m->master     = clampi(json_int(val, "mst", m->master), 0, 200);
        m->master_g   = (float)m->master / 100.0f;
        m->follow     = json_int(val, "flw", m->follow) ? 1 : 0;
        const char *arr;
        struct { const char *key; int which; } fields[] = {
            { "\"lv\":\"", 0 }, { "\"pn\":\"", 1 },
            { "\"rv\":\"", 2 }, { "\"sh\":\"", 3 },
        };
        for (int f = 0; f < 4; f++) {
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
                }
                arr = (*end == ',') ? end + 1 : end;
            }
        }
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
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "|%d|%d",
                                undo_avail, m->swap_track >= 0 ? 1 : 0), buf_len);
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
    if (!strcmp(key, "rec_action")) return snprintf(buf, (size_t)buf_len, "%d", m->rec_action);
    if (!strcmp(key, "dub_mode"))   return snprintf(buf, (size_t)buf_len, "%d", m->dub_mode);
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
        int n = 0;
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                "{\"q\":%d,\"ra\":%d,\"dm\":%d,\"mst\":%d,\"flw\":%d",
                                m->quantize, m->rec_action, m->dub_mode,
                                m->master, m->follow), buf_len);
        struct { const char *key; int (*get)(const mk_track_t *); } arrs[] = {
            { "lv", g_level }, { "pn", g_pan }, { "rv", g_rev }, { "sh", g_shot },
        };
        for (int f = 0; f < 4; f++) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%s\":\"", arrs[f].key), buf_len);
            n = nclamp(n + track_csv(m, buf + n, buf_len - n, arrs[f].get), buf_len);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), buf_len);
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "}"), buf_len);
        return n;
    }

    return -1;
}
