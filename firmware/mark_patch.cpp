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
 *   Knob 1-4       track 1-4, plus CV In 1-4 summed in hardware:
 *                  level in cvmode 1, record/play trigger in cvmode 0
 *   Gate In 1      all start / stop                 "all_btn"
 *   Gate In 2      undo / redo                      "undo"
 *   Gate Out       track 1 loop boundary            a rack-wide sync pulse
 *   CV Out 1/2     track 1/2 playhead position      0-5V ramp per loop
 *   Encoder turn   menu cursor
 *   Encoder push + turn   edit the selected menu item
 *   Encoder tap    all start / stop
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
 * The Patch has four knobs and four CV inputs, and libDaisy exposes them as
 * FOUR analog controls, not eight (DaisyPatch::CTRL_LAST == 4): each CV jack
 * is summed with its knob before the ADC, so a CV input can only ever be read
 * together with its knob. So knob N and CV N are one channel, and that channel
 * belongs to track N. There are no knob pages: the first cut paged the knobs
 * between track levels and the global params, and in trigger mode that was a
 * bug twice over -- the CV pulse that triggered a track also "moved" its knob,
 * so the track's level chased the parked pot to zero, and on the GLOBAL page
 * the same pulse would have hit master. The global params now live on the
 * encoder menu, which is what the screen is for.
 *
 * cvmode -- which job the four channels do. Runtime, not build-time, because
 * both are legitimate and which one is right depends on the patch.
 *
 *   1  LEVEL (default).  Knob + CV is the track's level, summing as the
 *      hardware intends. Transport comes from the gate inputs, which is the
 *      footswitch workflow anyway. Levels are also on the menu.
 *
 *   0  TRANSPORT.  Knob + CV is a threshold trigger for the track's
 *      record/play button: per-track control from a sequencer. The knob has
 *      to sit near zero or it holds the trigger permanently high, and the
 *      display says so. Levels come from the menu only.
 */
static int  g_cvmode = 1;
static bool g_cv_prev[MARK_TRACKS];

#define CV_TRIG_ON   0.55f   /* schmitt, in normalised ADC units */
#define CV_TRIG_OFF  0.35f
#define CV_PARK_MAX  0.15f   /* a knob above this makes cvmode 0 unusable */

static int g_last_level[MARK_TRACKS];   /* last level dispatched; -32768 = never */

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

static int get_int(const char *key)
{
    char buf[16];
    if(mark_get_param(M, key, buf, sizeof(buf)) < 0) return 0;
    return atoi(buf);
}

static void set_int(const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    mark_set_param(M, key, b);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void track_key(char *out, size_t n, int track, const char *suffix)
{
    /* Clamped so the track number is one digit and the 16-byte key buffers
     * at the call sites provably fit (-Wformat-truncation). */
    snprintf(out, n, "t%d_%s", clampi(track, 0, 8) + 1, suffix);
}

/* ---- encoder menu ------------------------------------------------------- */

/*
 * Turn selects, push-and-turn edits. Values are read back from the engine for
 * display rather than mirrored here, so a MIDI CC or a state restore shows up
 * without bookkeeping. cvmode is the one shim-side item.
 *
 * Four global items, then one level per track. In cvmode 1 a level edited
 * here holds until its knob next moves (last touched wins); in cvmode 0 the
 * menu is the only level control.
 */
#define M_GLOBALS 4
#define M_COUNT   (M_GLOBALS + MARK_TRACKS)
#define M_VISIBLE 5

static const char *const GLOBAL_LABEL[M_GLOBALS] = { "mast", "qnt", "grid", "cvmd" };
static const char *const GLOBAL_KEY[M_GLOBALS]   = { "master", "quantize", "rec_grid", NULL };
static const int         GLOBAL_LO[M_GLOBALS]    = { 0, 0, 0, 0 };
static const int         GLOBAL_HI[M_GLOBALS]    = { 200, 1, 3, 1 };

static int g_menu_sel;
static int g_menu_top;   /* first visible item */

static void menu_label(int item, char *out, size_t n)
{
    if(item < M_GLOBALS) snprintf(out, n, "%s", GLOBAL_LABEL[item]);
    else                 snprintf(out, n, "lvl%d", item - M_GLOBALS + 1);
}

static int menu_get(int item)
{
    if(item < M_GLOBALS)
        return GLOBAL_KEY[item] ? get_int(GLOBAL_KEY[item]) : g_cvmode;
    char key[16];
    track_key(key, sizeof(key), item - M_GLOBALS, "level");
    return get_int(key);
}

static void menu_edit(int inc)
{
    int item = g_menu_sel;
    if(item < M_GLOBALS)
    {
        int v = clampi(menu_get(item) + inc, GLOBAL_LO[item], GLOBAL_HI[item]);
        if(GLOBAL_KEY[item]) set_int(GLOBAL_KEY[item], v);
        else                 g_cvmode = v;
    }
    else
    {
        int  t = item - M_GLOBALS;
        char key[16];
        track_key(key, sizeof(key), t, "level");
        int v = clampi(menu_get(item) + inc, 0, 200);
        set_int(key, v);
        g_last_level[t] = v;   /* the knob re-takes only when it moves */
    }
}

static bool enc_down;
static bool enc_turned;

/* Encoder tap fires all start/stop on RELEASE, not on press -- a press that
 * turns is a menu edit and must not also toggle the transport. That costs a
 * plain tap the time between press and release; Gate In 1 is the tight
 * transport input, and quantized starts land on the grid regardless. */
static void encoder(void)
{
    if(hw.encoder.RisingEdge())
    {
        enc_down   = true;
        enc_turned = false;
    }

    int inc = hw.encoder.Increment();
    if(inc)
    {
        if(enc_down && hw.encoder.Pressed())
        {
            menu_edit(inc);
            enc_turned = true;
        }
        else
        {
            g_menu_sel = clampi(g_menu_sel + inc, 0, M_COUNT - 1);
            if(g_menu_sel < g_menu_top)               g_menu_top = g_menu_sel;
            if(g_menu_sel >= g_menu_top + M_VISIBLE)  g_menu_top = g_menu_sel - M_VISIBLE + 1;
        }
    }

    if(enc_down && !hw.encoder.Pressed())
    {
        enc_down = false;
        if(!enc_turned) mark_set_param(M, "all_btn", "1");
    }
}

/* ---- knobs + CV inputs -------------------------------------------------- */

static void dispatch_channels(void)
{
    for(int k = 0; k < MARK_TRACKS; k++)
    {
        float norm = hw.GetKnobValue((DaisyPatch::Ctrl)k);
        if(norm < 0.0f) norm = 0.0f;
        if(norm > 1.0f) norm = 1.0f;

        if(g_cvmode == 0)
        {
            bool on = g_cv_prev[k] ? (norm > CV_TRIG_OFF) : (norm > CV_TRIG_ON);
            if(on && !g_cv_prev[k])
            {
                char key[16];
                track_key(key, sizeof(key), k, "btn");
                mark_set_param(M, key, "1");
            }
            g_cv_prev[k] = on;
            continue;   /* levels are menu-only in this mode */
        }

        int v = (int)(norm * 200.0f + 0.5f);
        if(v == g_last_level[k]) continue;
        g_last_level[k] = v;
        char key[16];
        track_key(key, sizeof(key), k, "level");
        set_int(key, v);
    }
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

/*
 * 128x64, Font_6x8: 21 columns, rows at y = 0, 16, 26, 36, 46, 56.
 *
 *   RP-S  cv1            <- the four track states, cvmode, park warning
 *   1R 100 |  >mast 200  <- tracks on the left: state, level, playhead
 *   2P  80 /   qnt    1     menu on the right, '>' is the cursor,
 *   3-   0 |   grid   0     scrolls to show lvl1-4
 *   4S 100 -   cvmd   1
 *              lvl1 100
 */
static const char *STATE_CH = "-RPDS";   /* EMPTY REC PLAY DUB STOP */
static const char *SPIN_CH  = "|/-\\";
#define MENU_X 66

static void draw(void)
{
    char line[32], label[8];
    hw.display.Fill(false);

    char st[MARK_TRACKS + 1];
    for(int t = 0; t < MARK_TRACKS; t++)
    {
        int s = get_csv_int("tstates", t);
        st[t] = (s >= 0 && s <= 4) ? STATE_CH[s] : '?';
    }
    st[MARK_TRACKS] = '\0';

    /* cvmode 0 only works with the knobs parked, because each CV jack sums
     * with its knob in hardware. Say so rather than let it misbehave. */
    int parked_high = 0;
    if(g_cvmode == 0)
        for(int k = 0; k < MARK_TRACKS; k++)
            if(hw.GetKnobValue((DaisyPatch::Ctrl)k) > CV_PARK_MAX) parked_high++;

    if(parked_high)
        snprintf(line, sizeof(line), "%s  cv%d  park %d", st, g_cvmode, parked_high);
    else
        snprintf(line, sizeof(line), "%s  cv%d", st, g_cvmode);
    hw.display.SetCursor(0, 0);
    hw.display.WriteString(line, Font_6x8, true);

    for(int t = 0; t < MARK_TRACKS && t < 4; t++)
    {
        char key[16];
        track_key(key, sizeof(key), t, "level");
        int pos = get_csv_int("tpos", t);
        if(pos < 0) pos = 0;
        snprintf(line, sizeof(line), "%d%c %3d %c", t + 1, st[t], get_int(key),
                 st[t] == '-' ? ' ' : SPIN_CH[(pos * 4) / 128]);
        hw.display.SetCursor(0, 16 + t * 10);
        hw.display.WriteString(line, Font_6x8, true);
    }

    for(int row = 0; row < M_VISIBLE; row++)
    {
        int item = g_menu_top + row;
        if(item >= M_COUNT) break;
        menu_label(item, label, sizeof(label));
        snprintf(line, sizeof(line), "%c%-4s %3d",
                 item == g_menu_sel ? '>' : ' ', label, menu_get(item));
        hw.display.SetCursor(MENU_X, 16 + row * 10);
        hw.display.WriteString(line, Font_6x8, true);
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

    for(int t = 0; t < MARK_TRACKS; t++)
    {
        tinp[t]         = tin[t];
        toutp[t]        = tout[t];
        g_last_level[t] = -32768;
    }

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    uint32_t last_draw = System::GetNow();

    for(;;)
    {
        hw.ProcessAllControls();

        encoder();
        dispatch_channels();

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
