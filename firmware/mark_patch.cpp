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
 *   Gate In 1      clock                            see "clock in" below
 *   Gate In 2      all start / stop                 "all_btn"
 *   Gate Out       track 1 loop boundary            a rack-wide sync pulse
 *   CV Out 1/2     track 1/2 playhead position      0-5V ramp per loop
 *   Encoder turn   menu cursor
 *   Encoder push + turn   edit the selected menu item
 *   Encoder tap    all start / stop
 *   Encoder hold   undo / redo
 *   MIDI In        clock, and CC to the engine
 *
 * The gate output and the two CV outputs are the reason to run Mark here
 * rather than on the Move: a looper that emits its own loop-boundary trigger
 * and playhead ramps can drive the rest of the rack instead of sitting in it.
 */
#include "daisy_patch.h"
#include "patch_alloc.h"
#include "module_picker.h"

extern "C" {
#include "vendor/mark_core.h"
#include "vendor/plugin_api_v1.h"
#include "clock_adapter.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace daisy;

static DaisyPatch      hw;
static mark_t         *M;
static host_api_v1_t   HOST;
static clock_adapter_t CLK;

#define BLOCK_SIZE 128

/* 4 tracks + 1 undo buffer, mono int16 at 48 kHz. MARK_MAX_SECONDS is 60, so
 * 5 * 60 * 48000 * 2 B = 28.8 MB, plus a 256 KB delay line per track and the
 * mark_t itself. 32 MB leaves headroom, and mark_create() steps down through
 * its own alloc_seconds ladder if even that does not fit. */
#define POOL_BYTES (32u * 1024u * 1024u)
static uint8_t DSY_SDRAM_BSS g_pool[POOL_BYTES];

/* ---- clock in ----------------------------------------------------------- */

/*
 * Mark quantizes record start and stop to a measure grid, and before any loop
 * exists the grid comes from the tempo. On the Move that tempo came from the
 * host; here the host stub has none, so the engine assumes 120 BPM and the
 * first loop would snap to 2.0 s bars regardless of what was played. A clock
 * fixes that at the source: with one running, every start and stop lands on
 * the rack's bar line -- the RC-505-with-MIDI-sync behaviour Mark was
 * modelled on. (Mark plays at fixed speed: the clock sets the grid at record
 * time; it does not stretch loops if the tempo changes afterwards.)
 *
 * Two sources, one owner at a time:
 *
 *   Gate In 1   through smack-versio's clock adapter, which turns gate edges
 *               into the 24 ppqn MIDI clock the engine already speaks. Ratio
 *               and mode are on the menu as on Smack; INF mode is tap tempo
 *               from a footswitch.
 *   MIDI In     TRS MIDI clock, forwarded as-is.
 *
 * The gate owns the engine while it is locked; otherwise MIDI clock, if any
 * is arriving; otherwise nothing. Changing owner sends the engine 0xFA (new
 * downbeat) or 0xFC (clock gone), so it never quantizes against a clock that
 * has stopped. The adapter free-runs at 120 when unpatched; those ticks are
 * dropped rather than fed to the engine as a phantom clock.
 *
 * Realtime bytes reach the engine only from the audio callback: the adapter
 * emits there, and everything the main loop wants to send is queued and
 * drained there, so tick counting is never interleaved across threads.
 */
enum { SRC_NONE = 0, SRC_GATE, SRC_MIDI };
static volatile int      g_clk_src = SRC_NONE;
static volatile bool     g_gate_edge;          /* main -> audio: a gate 1 rising edge */
static uint32_t          g_midi_tick_last;     /* GetNow() of the last TRS clock tick */
static bool              g_midi_seen;
#define MIDI_CLOCK_TIMEOUT_MS 2000u            /* same idle bound the adapter uses */

#define RT_QUEUE 32
static volatile uint8_t  g_rt_q[RT_QUEUE];
static volatile uint32_t g_rt_head, g_rt_tail;

static void rt_push(uint8_t b)                 /* main thread */
{
    uint32_t h = g_rt_head;
    if(((h + 1) % RT_QUEUE) == g_rt_tail) return;   /* full: drop */
    g_rt_q[h]  = b;
    g_rt_head  = (h + 1) % RT_QUEUE;
}

/*
 * Bar phase from the gate. The adapter's FIRST pulse is the downbeat ("first
 * pulse defines the downbeat", clock_adapter.c), but the gate only takes the
 * engine when the adapter locks, on pulse 2, and the owner switch's 0xFA
 * zeroes the engine's bar count there -- so bar 1 would be pulse 2. Instead,
 * g_gate_pulse counts pulses from the adapter's downbeat, and the first edge
 * tick that falls on a bar line counted from pulse 1 (every 96 / ratio
 * pulses: 1, 5, 9 at "=1") goes to the engine as 0xFA rather than 0xF8. To
 * the engine 0xFA IS tick 0 -- just as the owner switch's 0xFA stands in for
 * the lock edge's own (dropped) tick -- so this re-zeroes it on the true
 * downbeat without adding a tick. Nothing can be waiting on the clock's bar
 * in between: the owner switch only just started it, and the engine's own
 * next bar line (96 ticks after pulse 2) comes after the re-zero.
 *
 * Audio thread only, like every other realtime byte. g_bar_synced drops
 * whenever an owner-switch 0xFA is drained (it lands at whatever phase the
 * lock happened) or the adapter starts a new downbeat.
 */
static int  g_gate_pulse;     /* pulses since the adapter's downbeat */
static bool g_bar_synced;     /* engine's bar count matches g_gate_pulse */

static void rt_drain(void)                     /* audio thread */
{
    while(g_rt_tail != g_rt_head)
    {
        uint8_t b = g_rt_q[g_rt_tail];
        g_rt_tail = (g_rt_tail + 1) % RT_QUEUE;
        if(b == 0xFA) g_bar_synced = false;    /* see "bar phase" above */
        mark_on_midi(M, &b, 1, 3);             /* source 3 = host, as on Move */
    }
}

static void emit_gate_clock(void *ctx, uint8_t byte)   /* audio thread */
{
    if(g_clk_src != SRC_GATE) return;
    /* The adapter's own 0xFA fires on its first edge, before it is locked and
     * before this source owns the engine; the owner switch sends its own, and
     * the bar re-zero below puts the downbeat back on that first edge. */
    if(byte == 0xFA) return;
    if(byte == 0xF8 && !g_bar_synced)
    {
        /* this tick's place counted from pulse 1 (the edge tick is 0) */
        int pos = g_gate_pulse * CLK.ticks_per_pulse + CLK.ticks_this_pulse - 1;
        if(pos % 96 == 0)
        {
            byte         = 0xFA;
            g_bar_synced = true;
        }
    }
    mark_on_midi((mark_t *)ctx, &byte, 1, 3);
}

static void update_clock_source(uint32_t now)  /* main thread */
{
    int want = SRC_NONE;
    if(clk_locked(&CLK))
        want = SRC_GATE;
    else if(g_midi_seen && now - g_midi_tick_last < MIDI_CLOCK_TIMEOUT_MS)
        want = SRC_MIDI;
    if(want == g_clk_src) return;
    g_clk_src = want;
    rt_push(want == SRC_NONE ? 0xFC : 0xFA);
}

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

static int g_last_level[MARK_TRACKS];   /* last KNOB reading dispatched; -32768 = never */

/* Channel k's knob + CV as a 0-200 level, the unit cvmode 1 dispatches. */
static int knob_level(int k)
{
    float norm = hw.GetKnobValue((DaisyPatch::Ctrl)k);
    if(norm < 0.0f) norm = 0.0f;
    if(norm > 1.0f) norm = 1.0f;
    return (int)(norm * 200.0f + 0.5f);
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
 * Turn selects, push-and-turn edits. Engine values are read back from the
 * engine for display rather than mirrored here, so a MIDI CC or a state
 * restore shows up without bookkeeping. cvmode, clock ratio and clock mode
 * are shim-side.
 *
 * Six global items, then one level per track. In cvmode 1 a level edited
 * here holds until its knob next moves (last touched wins); in cvmode 0 the
 * menu is the only level control.
 */
/* "play" (Tim, 2026-10-03): the encoder's performance gestures -- tap = all
 * start/stop, hold = undo/redo -- happen only with the cursor on this first
 * row, where the module starts. On every other row a click selects it for
 * editing ('*'), a turn changes it, a click lets go; a click on "mods" opens
 * the module picker. Push-and-turn never worked on his Patch: the switch
 * opens while the knob turns. */
enum { MI_PLAY = 0, MI_MASTER, MI_QNT, MI_GRID, MI_CVMODE, MI_RATIO, MI_CLKMODE, MI_MODS, M_GLOBALS };
#define M_COUNT   (M_GLOBALS + MARK_TRACKS)
#define M_VISIBLE 5

static const char *const GLOBAL_LABEL[M_GLOBALS] = { "play", "mast", "qnt", "grid", "cvmd", "rat", "clk", "mods" };
static const char *const GLOBAL_KEY[M_GLOBALS]   = { NULL, "master", "quantize", "rec_grid", NULL, NULL, NULL, NULL };
static const int         GLOBAL_HI[M_GLOBALS]    = { 0, 200, 1, 3, 1, 2, 2, 0 };
static bool g_editing;   /* a non-play row is selected: turns edit it */

static int g_ratio_sel = 1, g_mode_sel = 2;
static const char *const RATIO_NAME[3] = { "/2", "=1", "x2" };
static const char *const MODE_NAME[3]  = { "EXT", "INF", "AUTO" };

static int g_menu_sel;
static int g_menu_top;   /* first visible item */

static void apply_clock(void)
{
    clk_set_ratio(&CLK, g_ratio_sel == 0 ? CLK_TICKS_DIV2
                      : g_ratio_sel == 2 ? CLK_TICKS_2X
                                         : CLK_TICKS_1X);
    clk_set_mode(&CLK, g_mode_sel == 0 ? CLK_EXTERNAL
                     : g_mode_sel == 1 ? CLK_INFER
                                       : CLK_AUTO);
}

static void menu_label(int item, char *out, size_t n)
{
    if(item < M_GLOBALS) snprintf(out, n, "%s", GLOBAL_LABEL[item]);
    else                 snprintf(out, n, "lvl%d", item - M_GLOBALS + 1);
}

static int menu_get(int item)
{
    switch(item)
    {
        case MI_CVMODE:  return g_cvmode;
        case MI_RATIO:   return g_ratio_sel;
        case MI_CLKMODE: return g_mode_sel;
        case MI_PLAY:
        case MI_MODS:    return 0;
        default: break;
    }
    if(item < M_GLOBALS) return get_int(GLOBAL_KEY[item]);
    char key[16];
    track_key(key, sizeof(key), item - M_GLOBALS, "level");
    return get_int(key);
}

static void menu_value(int item, char *out, size_t n)
{
    if(item == MI_RATIO)        snprintf(out, n, "%s", RATIO_NAME[g_ratio_sel]);
    else if(item == MI_CLKMODE) snprintf(out, n, "%s", MODE_NAME[g_mode_sel]);
    else if(item == MI_MODS)    snprintf(out, n, "%s", "...");
    else if(item == MI_PLAY)    out[0] = '\0';
    else                        snprintf(out, n, "%d", menu_get(item));
}

static void menu_edit(int inc)
{
    int item = g_menu_sel;
    if(item < M_GLOBALS)
    {
        int v = clampi(menu_get(item) + inc, 0, GLOBAL_HI[item]);
        switch(item)
        {
            case MI_CVMODE:
                g_cvmode = v;
                /* Entering trigger mode: seed the schmitt state from where
                 * the channels already sit, or every knob above the threshold
                 * would read as a rising edge on the next pass and arm its
                 * track. */
                if(v == 0)
                    for(int k = 0; k < MARK_TRACKS; k++)
                        g_cv_prev[k] = hw.GetKnobValue((DaisyPatch::Ctrl)k) > CV_TRIG_ON;
                /* Entering level mode: the same, for levels. The knobs sat
                 * parked through cvmode 0 while the menu set the levels, so
                 * every reading differs from the last one dispatched and
                 * would drop each level to its parked knob at once. Seed
                 * from where they sit; a level follows its knob again only
                 * once that knob actually moves (last touched wins). */
                else
                    for(int k = 0; k < MARK_TRACKS; k++)
                        g_last_level[k] = knob_level(k);
                break;
            case MI_RATIO:   g_ratio_sel = v; apply_clock(); break;
            case MI_CLKMODE: g_mode_sel  = v; apply_clock(); break;
            case MI_PLAY:
            case MI_MODS:    break; /* nothing to turn; a click on mods opens the picker */
            default:         set_int(GLOBAL_KEY[item], v); break;
        }
    }
    else
    {
        /* g_last_level is deliberately NOT updated here: it tracks the last
         * KNOB reading, so the knob re-takes the level only when its own
         * reading changes. Setting it to the menu value would make the very
         * next dispatch see knob != last and overwrite this edit at once. */
        char key[16];
        track_key(key, sizeof(key), item - M_GLOBALS, "level");
        set_int(key, clampi(menu_get(item) + inc, 0, 200));
    }
}

/* Tap = all start/stop, on RELEASE: a press that turns is a menu edit and
 * must not also toggle the transport. That costs a plain tap the time between
 * press and release; Gate In 2 is the tight transport input, and quantized
 * starts land on the grid regardless. Hold = undo/redo, firing at the
 * threshold the way the RC-505's held button does; that press is then spent. */
#define HOLD_UNDO_MS 600u

static bool enc_down;
static bool enc_turned;
static bool enc_held;

static void encoder(void)
{
    if(hw.encoder.RisingEdge())
    {
        enc_down   = true;
        enc_turned = false;
        enc_held   = false;
    }

    /* A press is read from libDaisy's edges, never Pressed(): RisingEdge
     * comes one 1 ms update before Pressed(), and a bounce drops Pressed()
     * for up to 8 ms (found 2026-10-03). A turn during a press makes it not a
     * click / not a tap. */
    int inc = hw.encoder.Increment();
    if(inc)
    {
        if(enc_down) enc_turned = true;
        if(g_editing)
        {
            menu_edit(inc);
        }
        else
        {
            g_menu_sel = clampi(g_menu_sel + inc, 0, M_COUNT - 1);
            if(g_menu_sel < g_menu_top)               g_menu_top = g_menu_sel;
            if(g_menu_sel >= g_menu_top + M_VISIBLE)  g_menu_top = g_menu_sel - M_VISIBLE + 1;
        }
    }

    /* Settings rows: click selects / lets go; on "mods" it opens the picker.
     * No transport or undo fires off the play row. */
    if(g_menu_sel != MI_PLAY)
    {
        if(hw.encoder.FallingEdge())
        {
            bool click = enc_down && !enc_turned;
            enc_down   = false;
            if(click && g_menu_sel == MI_MODS)
                picker::run(hw); /* modal; returns only if the user backs out */
            else if(click)
                g_editing = !g_editing;
        }
        return;
    }

    if(enc_down && !enc_turned && !enc_held && hw.encoder.Pressed()
       && (uint32_t)hw.encoder.TimeHeldMs() > HOLD_UNDO_MS)
    {
        mark_set_param(M, "undo", "1");
        enc_held = true;
    }

    if(enc_down && hw.encoder.FallingEdge())
    {
        enc_down = false;
        if(!enc_turned && !enc_held) mark_set_param(M, "all_btn", "1");
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

        int v = knob_level(k);
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

    /* Clock first, so this block's ticks precede this block's audio, the
     * order the Move host delivers them in. The gate edge is polled in the
     * main loop (fast enough to catch a 1 ms trigger, which block rate is
     * not) and handed over here so the adapter runs on one thread. */
    rt_drain();
    if(g_gate_edge)
    {
        g_gate_edge = false;
        clk_gate_edge(&CLK);
        /* pending_start: the adapter took this edge as a new downbeat (the
         * first one, or the first after an idle gap) */
        if(CLK.pending_start)
        {
            g_gate_pulse = 0;
            g_bar_synced = false;
        }
        else   /* mod 96 keeps pulse * ratio mod 96 exact, and never overflows */
            g_gate_pulse = (g_gate_pulse + 1) % 96;
    }
    clk_advance(&CLK, (int)n, emit_gate_clock, M);

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
 *   RP-S cv1 G120        <- track states, cvmode, clock source (G = gate at
 *   1R 100 |  >mast 200     that BPM, MIDI, free), park warning
 *   2P  80 /   qnt    1  <- tracks on the left: state, level, playhead
 *   3-   0     grid   0     menu on the right, '>' is the cursor,
 *   4S 100 -   cvmd   1     scrolls to reach rat, clk and lvl1-4
 *              rat   =1
 */
static const char *STATE_CH = "-RPDS";   /* EMPTY REC PLAY DUB STOP */
static const char *SPIN_CH  = "|/-\\";
#define MENU_X 66

static void draw(void)
{
    char line[32], label[8], val[8], clk[8];
    hw.display.Fill(false);

    char st[MARK_TRACKS + 1];
    for(int t = 0; t < MARK_TRACKS; t++)
    {
        int s = get_csv_int("tstates", t);
        st[t] = (s >= 0 && s <= 4) ? STATE_CH[s] : '?';
    }
    st[MARK_TRACKS] = '\0';

    if(g_clk_src == SRC_GATE)
        snprintf(clk, sizeof(clk), "G%d", (int)(clk_bpm(&CLK) + 0.5f));
    else
        snprintf(clk, sizeof(clk), "%s", g_clk_src == SRC_MIDI ? "MIDI" : "free");

    /* cvmode 0 only works with the knobs parked, because each CV jack sums
     * with its knob in hardware. Say so rather than let it misbehave. */
    int parked_high = 0;
    if(g_cvmode == 0)
        for(int k = 0; k < MARK_TRACKS; k++)
            if(hw.GetKnobValue((DaisyPatch::Ctrl)k) > CV_PARK_MAX) parked_high++;

    if(parked_high)
        snprintf(line, sizeof(line), "%s cv%d %s park%d", st, g_cvmode, clk, parked_high);
    else
        snprintf(line, sizeof(line), "%s cv%d %s", st, g_cvmode, clk);
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
        menu_value(item, val, sizeof(val));
        snprintf(line, sizeof(line), "%c%-4s %4s",
                 item == g_menu_sel ? (g_editing ? '*' : '>') : ' ', label, val);
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

    clk_init(&CLK, MARK_SR, 120.0f);
    apply_clock();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);
    hw.midi.StartReceive();

    uint32_t last_draw = System::GetNow();

    for(;;)
    {
        hw.ProcessAllControls();
        uint32_t now = System::GetNow();

        encoder();
        dispatch_channels();

        /* Gate 1 is the clock; the edge is handed to the audio thread. Gate 2
         * is global transport, so a footswitch works in either cvmode. */
        if(hw.gate_input[0].Trig()) g_gate_edge = true;
        if(hw.gate_input[1].Trig()) mark_set_param(M, "all_btn", "1");

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
            else if(ev.type == SystemRealTime)
            {
                uint8_t b = (uint8_t)(0xF8 + (uint8_t)ev.srt_type);
                if(ev.srt_type == TimingClock)
                {
                    g_midi_tick_last = now;
                    g_midi_seen      = true;
                }
                /* Forwarded only while MIDI owns the engine; the owner switch
                 * below sends its own 0xFA/0xFC around that. */
                if(g_clk_src == SRC_MIDI
                   && (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC))
                    rt_push(b);
            }
        }

        update_clock_source(now);
        update_cv_outs();

        if(now - last_draw >= 50u)
        {
            draw();
            last_draw = now;
        }
    }
}
