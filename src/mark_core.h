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

#define MARK_SR      44100
#define MARK_TRACKS  5
/* Per-track capacity target, seconds. 60 s stereo int16 = ~10.6 MB per
 * track; 5 tracks + 1 undo buffer = ~63 MB. mark_create() falls back to
 * smaller capacities if allocation fails (see mark_alloc_seconds). */
#define MARK_MAX_SECONDS 60

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
