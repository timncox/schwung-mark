/*
 * Mark — RC-505-style 5-track live looper for the Ableton Move (schwung).
 *
 * Five independent stereo loop tracks, each cycling record -> play ->
 * overdub with its own stop/clear, level, pan, reverse and one-shot.
 * Tracks quantize to a shared measure grid (MIDI clock when running,
 * otherwise the first-recorded track defines the grid), like the RC-505's
 * LOOP SYNC ON + QUANTIZE MEASURE defaults. Single-level undo/redo of the
 * last record/overdub gesture (the RC's UNDO/REDO), implemented as an
 * incremental buffer swap so the render path never blocks.
 *
 * Timing model mirrors smack: 4/4, MIDI clock 24 ppqn (96 ticks/measure)
 * via on_midi, free-run fallback from host get_bpm(). Render path is
 * non-allocating; all buffers are allocated in mark_create() (with a
 * shrinking-capacity fallback if the device can't give us the full size).
 */
#ifndef MARK_CORE_H
#define MARK_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

/* All four are overridable at build time so one engine serves several panels.
 * Move is the default and must stay bit-identical: 44100 / 5 tracks / stereo.
 * The Daisy Patch build passes -DMARK_SR=48000 -DMARK_TRACKS=4 -DMARK_CH=1. */
#ifndef MARK_SR
#define MARK_SR      44100
#endif

#ifndef MARK_TRACKS
#define MARK_TRACKS  5
#endif

/*
 * Channels held in TRACK STORAGE (t->buf and undo_buf). NOT the width of the
 * audio path — that is always stereo, so pan, the per-track delay and the FX
 * chain are unaffected by this.
 *
 * It is a separate knob because storage is where all the memory is. The delay
 * lines are 256 KB per track and fx_block is 2 KB total; the track buffers are
 * 11.5 MB each at 60 s / 48 kHz. On a panel whose tracks are fed by one mono
 * jack each, storing two identical channels doubles the only allocation that
 * matters, so MARK_CH=1 halves the module's footprint and buys twice the loop
 * length for the same SDRAM.
 *
 * Every access goes through the tbuf_* accessors below, which at MARK_CH==2
 * compile to exactly the expressions they replaced.
 */
#ifndef MARK_CH
#define MARK_CH      2
#endif

/* Per-track capacity target, seconds. At MARK_CH=2, 60 s stereo int16 is
 * ~10.6 MB per track (44.1k) and 5 tracks + 1 undo buffer is ~63 MB.
 * At MARK_CH=1 and 4 tracks that same 60 s costs ~23 MB at 48 kHz.
 * mark_create() falls back to smaller capacities if allocation fails
 * (see mark_alloc_seconds). */
#ifndef MARK_MAX_SECONDS
#define MARK_MAX_SECONDS 60
#endif

/* Track states, exposed via the `tstates` getter in this order. */
typedef enum {
    MK_EMPTY = 0,
    MK_REC   = 1,
    MK_PLAY  = 2,
    MK_DUB   = 3,
    MK_STOP  = 4
} mk_tstate_t;

typedef struct mark mark_t;

mark_t *mark_create(const host_api_v1_t *host);
/* Production constructor: module_dir lets Mark discover sibling
 * modules/audio_fx entries. mark_create() remains the no-catalog test API. */
mark_t *mark_create_in_dir(const host_api_v1_t *host, const char *module_dir);
void    mark_destroy(mark_t *m);

/* Process one block, stereo interleaved int16. in and out may alias. */
void mark_process(mark_t *m, const int16_t *in, int16_t *out, int frames);

void mark_on_midi(mark_t *m, const uint8_t *msg, int len, int source);
void mark_set_param(mark_t *m, const char *key, const char *val);
int  mark_get_param(mark_t *m, const char *key, char *buf, int buf_len);

#endif /* MARK_CORE_H */
