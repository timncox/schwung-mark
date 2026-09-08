/*
 * Mark — Electrosmith Daisy Patch port.
 *
 * Mark is an RC-505-style live looper. The engine (vendor/mark_core.c) is the
 * same C the Move build uses; this file is the Daisy shim, the counterpart of
 * src/mark_fx.c on Move.
 *
 * This is not the Move instrument moved sideways. The Move gives Mark 32 pads
 * and one stereo bus; the Patch gives it four independent audio jacks in and
 * four out, four CV inputs, two CV outputs and a gate output. So the port is
 * built around mark_process_multi(): FOUR MONO TRACKS, each with its own
 * input jack and its own output jack.
 *
 * Build config (see Makefile): MARK_TRACKS=4, MARK_CH=1, MARK_SR=48000,
 * MARK_HOSTED_FX=0, MARK_SESSIONS=0.
 *
 * Where each connector goes, and why:
 *
 *   Audio In 1-4   track 1-4 record source          mark_process_multi
 *   Audio Out 1-4  track 1-4 playback               mark_process_multi
 *   CV In 1-4      cvmode-dependent, see below
 *   Gate In 1      all start / stop                 "all_btn"
 *   Gate In 2      undo / redo                      "undo"
 *   Gate Out       track 1 loop boundary            a rack-wide sync pulse
 *   CV Out 1/2     track 1/2 playhead position      0-5V ramp per loop
 *   Encoder turn   page
 *   Encoder press  all start / stop
 *   Knobs 1-4      the current page's four params
 *   MIDI In        CC to the engine
 *
 * The gate output and the two CV outputs are the reason to run Mark here
 * rather than on the Move: a looper that emits its own loop-boundary trigger
 * and playhead ramps can drive the rest of the rack instead of sitting in it.
 */
#include "daisy_patch.h"
#include "patch_alloc.h"

extern "C" {
#include "vendor/mark_core.h"
#include "vendor/plugin_api_v1.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace daisy;

static DaisyPatch    hw;
static mark_t       *M;
static host_api_v1_t HOST;

#define BLOCK_SIZE 128

/* 4 tracks + 1 undo buffer, mono int16 at 48 kHz. MARK_MAX_SECONDS is 60, so
 * 5 * 60 * 48000 * 2 B = 28.8 MB, plus a 256 KB delay line per track and the
 * mark_t itself. 32 MB leaves headroom, and mark_create() steps down through
 * its own alloc_seconds ladder if even that does not fit. */
#define POOL_BYTES (32u * 1024u * 1024u)
static uint8_t DSY_SDRAM_BSS g_pool[POOL_BYTES];

/* ---- control surface ---------------------------------------------------- */

/*
 * cvmode — which job the four CV inputs do. Runtime, not build-time, because
 * both are legitimate and which one is right depends on the patch.
 *
 *   0  TRANSPORT.  Each CV input is a threshold trigger for its own track's
 *      record/play button. Per-track control from a sequencer. The catch is
 *      hardware: on the Patch each CV jack is SUMMED WITH ITS KNOB, so the
 *      knob has to sit near zero or it holds the trigger permanently high.
 *      The display warns when a knob is parked too high to be safe.
 *
 *   1  LEVEL (default).  Each CV input rides its own track's level, knob and
 *      CV summing as the hardware intends. Transport comes from the gate
 *      inputs instead, which is the footswitch workflow anyway.
 */
static int  g_cvmode = 1;
static bool g_cv_prev[4];

#define CV_TRIG_ON   0.55f   /* schmitt, in normalised ADC units */
#define CV_TRIG_OFF  0.35f
#define CV_PARK_MAX  0.15f   /* a knob above this makes cvmode 0 unusable */

enum { PG_LEVEL = 0, PG_GLOBAL, PG_COUNT };
static const char *const PAGE_NAME[PG_COUNT] = { "LEVEL", "GLOBAL" };
static int g_page;

/* Page 1 params, in knob order. */
struct GParam { const char *key; const char *label; int lo, hi; };
static const GParam GPARAM[4] = {
    { "master",   "mast", 0, 200 },
    { "quantize", "qnt",  0, 1   },
    { "rec_grid", "grid", 0, 3   },
    { NULL,       "cvmd", 0, 1   },   /* cvmode is shim-side, not an engine param */
};

/* Knob pickup: absolute pots across pages, same reasoning as the Belt and
 * Smack ports. A knob is inert until it crosses the value it takes over. */
static bool  g_live[4];
static float g_knob_at[4];
static int   g_last[4];
#define PICKUP_SLOP 0.02f

static void page_reset(void)
{
    for(int k = 0; k < 4; k++)
    {
        g_live[k]    = false;
        g_knob_at[k] = hw.GetKnobValue((DaisyPatch::Ctrl)k);
        g_last[k]    = -32768;
    }
}

/* ---- engine helpers ----------------------------------------------------- */

static int get_csv_int(const char *key, int idx)
{
    char buf[64];
    if(mark_get_param(M, key, buf, sizeof(buf)) < 0) return -1;
    const char *p = buf;
    for(int i = 0; i < idx; i++)
    {
        p = strchr(p, ',');
        if(!p) return -1;
        p++;
    }
    return atoi(p);
}

static void set_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    mark_set_param(M, key, b);
}

static void track_key(char *out, size_t n, int track, const char *suffix)
{
    snprintf(out, n, "t%d_%s", track + 1, suffix);
}

/* ---- audio -------------------------------------------------------------- */

static int16_t        tin[MARK_TRACKS][BLOCK_SIZE];
static int16_t        tout[MARK_TRACKS][BLOCK_SIZE];
static const int16_t *tinp[MARK_TRACKS];
static int16_t       *toutp[MARK_TRACKS];

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    const size_t n = size < BLOCK_SIZE ? size : BLOCK_SIZE;

    for(int t = 0; t < MARK_TRACKS; t++)
        for(size_t i = 0; i < n; i++)
        {
            float v = in[t][i];
            if(v > 1.0f) v = 1.0f;
            else if(v < -1.0f) v = -1.0f;
            tin[t][i] = (int16_t)(v * 32767.0f);
        }

    mark_process_multi(M, tinp, toutp, (int)n);

    for(int t = 0; t < MARK_TRACKS; t++)
        for(size_t i = 0; i < n; i++)
            out[t][i] = (float)tout[t][i] / 32767.0f;
}

/* ---- CV / gate outputs -------------------------------------------------- */

/*
 * CV Out 1 and 2 carry tracks 1 and 2's playhead as a 0-5V ramp, one ramp per
 * loop. The DAC is 12-bit and the Patch's CV output stage spans 0-5V, so full
 * scale is 4095.
 *
 * The gate output fires on track 1's loop boundary, detected as the playhead
 * wrapping backwards. That makes Mark a clock source for the rest of the rack
 * at exactly the loop length the player recorded, which no amount of dividing
 * a master clock will give you.
 */
#define GATE_MS 5u

static uint32_t g_gate_until;
static int      g_pos_prev[2];

static void update_cv_outs(void)
{
    for(int t = 0; t < 2; t++)
    {
        int pos = get_csv_int("tpos", t);      /* 0-127 across the loop */
        if(pos < 0) pos = 0;

        uint16_t v = (uint16_t)((pos * 4095) / 127);
        hw.seed.dac.WriteValue(t == 0 ? DacHandle::Channel::ONE
                                      : DacHandle::Channel::TWO, v);

        if(t == 0 && pos < g_pos_prev[0])      /* wrapped: loop top */
            g_gate_until = System::GetNow() + GATE_MS;
        g_pos_prev[t] = pos;
    }
    hw.gate_output.Write(System::GetNow() < g_gate_until);
}

/* ---- display ------------------------------------------------------------ */

static const char *STATE_CH = "-RPDS";   /* EMPTY REC PLAY DUB STOP */

static void draw(void)
{
    char line[32];
    hw.display.Fill(false);

    /* Header: page, the four track states at a glance, cvmode. */
    char st[8];
    for(int t = 0; t < MARK_TRACKS; t++)
    {
        int s = get_csv_int("tstates", t);
        st[t] = (s >= 0 && s <= 4) ? STATE_CH[s] : '?';
    }
    st[MARK_TRACKS] = '\0';
    snprintf(line, sizeof(line), "%-6s %s  cv%d", PAGE_NAME[g_page], st, g_cvmode);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    if(g_page == PG_LEVEL)
    {
        for(int t = 0; t < MARK_TRACKS; t++)
        {
            char key[16], val[16];
            track_key(key, sizeof(key), t, "level");
            if(mark_get_param(M, key, val, sizeof(val)) < 0) strcpy(val, "?");
            snprintf(line, sizeof(line), "t%d %-4s%s", t + 1, val,
                     g_live[t] ? "" : " *");
            hw.display.SetCursor(0, 16 + t * 10);
            hw.display.WriteString(line, Font_6x8, true);
        }
    }
    else
    {
        for(int k = 0; k < 4; k++)
        {
            char val[16];
            if(GPARAM[k].key)
            {
                if(mark_get_param(M, GPARAM[k].key, val, sizeof(val)) < 0)
                    strcpy(val, "?");
            }
            else
            {
                snprintf(val, sizeof(val), "%d", g_cvmode);
            }
            snprintf(line, sizeof(line), "%-4s %-4s%s", GPARAM[k].label, val,
                     g_live[k] ? "" : " *");
            hw.display.SetCursor(0, 16 + k * 10);
            hw.display.WriteString(line, Font_6x8, true);
        }
    }

    /* cvmode 0 only works with the knobs parked, because each CV jack sums
     * with its knob in hardware. Say so rather than let it misbehave. */
    if(g_cvmode == 0)
    {
        int bad = 0;
        for(int k = 0; k < 4; k++)
            if(hw.GetKnobValue((DaisyPatch::Ctrl)k) > CV_PARK_MAX) bad++;
        if(bad)
        {
            snprintf(line, sizeof(line), "park %d knob%s", bad,
                     bad > 1 ? "s" : "");
            hw.display.SetCursor(64, 0);
            hw.display.WriteString(line, Font_6x8, true);
        }
    }

    hw.display.Update();
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    patch_alloc_init(g_pool, POOL_BYTES);

    HOST.api_version      = 1;
    HOST.sample_rate      = MARK_SR;
    HOST.frames_per_block = BLOCK_SIZE;

    M = mark_create(&HOST);

    if(!M || patch_alloc_failed())
    {
        hw.display.Fill(false);
        hw.display.SetCursor(0, 0);
        hw.display.WriteString("MARK: alloc fail", Font_6x8, true);
        char l[32];
        snprintf(l, sizeof(l), "%u/%u KB",
                 (unsigned)(patch_alloc_used() / 1024u),
                 (unsigned)(patch_alloc_capacity() / 1024u));
        hw.display.SetCursor(0, 16);
        hw.display.WriteString(l, Font_6x8, true);
        hw.display.Update();
        for(;;) {}
    }

    for(int t = 0; t < MARK_TRACKS; t++) { tinp[t] = tin[t]; toutp[t] = tout[t]; }

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    page_reset();
    uint32_t last_draw = System::GetNow();

    for(;;)
    {
        hw.ProcessAllControls();

        int inc = hw.encoder.Increment();
        if(inc)
        {
            g_page += inc;
            while(g_page < 0)        g_page += PG_COUNT;
            while(g_page >= PG_COUNT) g_page -= PG_COUNT;
            page_reset();
        }
        if(hw.encoder.RisingEdge()) mark_set_param(M, "all_btn", "1");

        /* Knobs, with pickup. */
        for(int k = 0; k < 4; k++)
        {
            float norm = hw.GetKnobValue((DaisyPatch::Ctrl)k);
            int   lo, hi;
            if(g_page == PG_LEVEL) { lo = 0; hi = 200; }
            else                   { lo = GPARAM[k].lo; hi = GPARAM[k].hi; }

            int v = lo + (int)(norm * (float)(hi - lo) + 0.5f);
            if(v < lo) v = lo;
            if(v > hi) v = hi;

            if(!g_live[k])
            {
                if(g_last[k] == -32768)
                {
                    if(fabsf(norm - g_knob_at[k]) > PICKUP_SLOP) g_live[k] = true;
                }
                else
                {
                    float at = (float)(g_last[k] - lo) / (float)(hi - lo);
                    if((g_knob_at[k] <= at && norm >= at)
                       || (g_knob_at[k] >= at && norm <= at))
                        g_live[k] = true;
                }
                g_knob_at[k] = norm;
                if(!g_live[k]) continue;
            }
            g_knob_at[k] = norm;
            if(v == g_last[k]) continue;
            g_last[k] = v;

            if(g_page == PG_LEVEL)
            {
                char key[16];
                track_key(key, sizeof(key), k, "level");
                set_int(key, v);
            }
            else if(GPARAM[k].key)
            {
                set_int(GPARAM[k].key, v);
            }
            else
            {
                g_cvmode = v;
            }
        }

        /* CV inputs, per cvmode. NOTE these read the same ADCs as the knobs —
         * the Patch sums jack and pot in hardware — which is exactly why
         * cvmode 0 needs the knobs parked. */
        for(int k = 0; k < 4 && k < MARK_TRACKS; k++)
        {
            float cv = hw.GetKnobValue((DaisyPatch::Ctrl)k);
            if(g_cvmode == 0)
            {
                bool on = g_cv_prev[k] ? (cv > CV_TRIG_OFF) : (cv > CV_TRIG_ON);
                if(on && !g_cv_prev[k])
                {
                    char key[16];
                    track_key(key, sizeof(key), k, "btn");
                    mark_set_param(M, key, "1");
                }
                g_cv_prev[k] = on;
            }
            /* cvmode 1: the knob loop above already sent it as level. */
        }

        /* Gates: global transport, so a footswitch works in either cvmode. */
        if(hw.gate_input[0].Trig()) mark_set_param(M, "all_btn", "1");
        if(hw.gate_input[1].Trig()) mark_set_param(M, "undo", "1");

        hw.midi.Listen();
        while(hw.midi.HasEvents())
        {
            MidiEvent ev = hw.midi.PopEvent();
            if(ev.type == ControlChange)
            {
                uint8_t msg[3] = { (uint8_t)(0xB0 | (ev.channel & 0x0F)),
                                   (uint8_t)ev.data[0],
                                   (uint8_t)ev.data[1] };
                mark_on_midi(M, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            }
        }

        update_cv_outs();

        uint32_t now = System::GetNow();
        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
