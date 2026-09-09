# Mark — Daisy Patch firmware

Mark's engine ported to the Electrosmith **Daisy Patch**. This directory is
the Daisy shim; it is the counterpart of `src/mark_fx.c`, which is the Move
shim. The engine in `vendor/` is vendored from `src/`, not forked.

**This is not the Move instrument moved sideways.** The Move gives Mark 32
pads and one stereo bus. The Patch gives it four independent audio jacks in
and four out, four CV inputs, two CV outputs and a gate output — so the port
is built around `mark_process_multi()`: **four mono tracks, each with its own
input jack and its own output jack.**

## Build

```sh
make -C firmware                 # -> firmware/build/mark_patch.bin
make -C firmware program-dfu     # flash over USB
```

Needs libDaisy built at `~/tim-os/daisy-sdk/libDaisy` (override `LIBDAISY_DIR`).
DaisySP is not used.

Builds `BOOT_SRAM` — it runs under the Daisy bootloader, because that is what
lets the firmware be switched from the panel (see **Switching modules**
below). The bootloader is installed once from ST ROM DFU (hold **BOOT**, tap
**RESET**, release BOOT, then `make -C firmware program-boot`); after that,
`make -C firmware program-dfu` has to hit the bootloader's ~2 s DFU window on
power-up, or, simpler, the `.bin` goes on the card.

## Panel — every connector does something

| Connector | Function |
|---|---|
| Audio In 1–4 | Track 1–4 record source |
| Audio Out 1–4 | Track 1–4 playback |
| **Knob 1–4 + CV In 1–4** | Track 1–4: level in `cvmode 1`, record/play trigger in `cvmode 0`. Knob and jack are one channel — see below |
| **Gate In 1** | **Clock** — see below |
| Gate In 2 | All start / stop |
| **Gate Out** | **Track 1 loop boundary** |
| **CV Out 1 / 2** | **Track 1 / 2 playhead, 0–5 V ramp per loop** |
| Encoder turn | Menu cursor: `mast` / `qnt` / `grid` / `cvmd` / `rat` / `clk` / `mods` / `lvl1`–`lvl4` |
| **Encoder push + turn** | Edit the selected menu item; on `mods`, open the module picker |
| Encoder tap | All start / stop — fires on release, so a push-and-turn is never a transport toggle; Gate In 2 is the tight one |
| Encoder hold > 0.6 s | Undo / redo |
| MIDI In | Clock, and CC to the engine |

The gate output and CV outputs are the reason to run Mark here rather than on
the Move. A looper that emits its own loop-boundary trigger and playhead ramps
drives the rest of the rack at exactly the loop length the player recorded —
which no amount of dividing a master clock will give you.

## Screen

128×64, `Font_6x8`. Tracks on the left — number, state (`-` empty, `R` rec,
`P` play, `D` dub, `S` stop), level, and a playhead spinner; the encoder menu
on the right with a `>` cursor, scrolling to reach `rat`, `clk` and
`lvl1`–`lvl4`. The header is the four states at a glance, the `cvmode`, the
clock source (`G120` = gate at that tempo, `MIDI`, `free`), and the park
warning.

```
RP-S cv1 G120
1R 100 |  >mast 200
2P  80 /   qnt    1
3-   0     grid   0
4S 100 -   cvmd   1
           rat   =1
```

## Clock in

Mark quantizes record start and stop to a measure grid, and before any loop
exists the grid comes from the tempo. On the Move that tempo came from the
host. Here the host stub has none, so the engine would assume 120 BPM and,
with quantize on and the grid at a whole bar (both defaults), the first loop
would snap to a multiple of 2.0 s regardless of what was played. Later loops
quantize to the first, so only the first is affected — but it sets the length
of everything else.

A clock fixes that at the source. With one running, every start and stop
lands on the rack's bar line: the RC-505-with-MIDI-sync behaviour Mark was
modelled on. Mark plays at fixed speed, so the clock sets the grid at record
time; it does not stretch loops if the tempo changes afterwards.

Two sources, one owner at a time:

| Source | How |
|---|---|
| **Gate In 1** | smack-versio's clock adapter turns gate edges into the 24 ppqn MIDI clock the engine already speaks. `rat` is what a pulse is worth: `/2` half note, `=1` quarter, `x2` eighth. `clk` is `EXT` (pulses are a clock), `INF` (tap tempo: pulse spacing sets the tempo, footswitchable) or `AUTO` (a steady train counts as `EXT`, anything else as `INF`) |
| **MIDI In** | TRS MIDI clock, forwarded as-is |

The gate owns the engine while it is locked; otherwise MIDI clock if any is
arriving; otherwise nothing, and the header says which. Changing owner sends
the engine a start (new downbeat) or a stop (clock gone), so it never
quantizes against a clock that has stopped. The adapter free-runs at 120 BPM
when unpatched; those ticks are dropped rather than fed to the engine as a
phantom clock.

With nothing patched the first loop is free-length and anchors the grid for
the rest — the engine rounds a record stop only against a real rhythm
reference (a synced track, a session grid, a running clock, a host tempo or
a tempo override), and on bare metal with none of those there is nothing
honest to round to.

## `cvmode` — what the four CV inputs do

Runtime, not build-time, because both jobs are legitimate and which is right
depends on the patch. Set it on the menu (`cvmd`).

First the hardware fact everything else follows from: libDaisy's `DaisyPatch`
exposes four analog controls, not eight — **each CV jack is summed with its
knob before the ADC**, so knob N and CV N are one channel, and that channel
belongs to track N. There are no knob pages. The first cut paged the knobs
between track levels and the global params, and in trigger mode that was a bug
twice over: the CV pulse that triggered a track also "moved" its knob, so the
track's level chased the parked pot to zero; and on the GLOBAL page the same
pulse would have hit master. The global params now live on the encoder menu.

**`cvmode 1` (default) — LEVEL.** Knob + CV is the track's level, summing as
the hardware intends. Transport comes from the gate inputs, which is the
footswitch workflow anyway. Levels are also on the menu; a menu edit holds
until the knob next moves.

**`cvmode 0` — TRANSPORT.** Knob + CV is a threshold trigger for the track's
record/play button, giving per-track control from a sequencer. The knobs must
sit near zero or they hold the trigger permanently high — the header warns
(`park N`) when one is too high — and levels come from the menu only.

## Switching modules from the panel

All three Patch ports share `module_picker.cpp`. The SD card is a module
library: put `belt.bin`, `smack.bin` and `mark.bin` in a `modules` folder on a
FAT32 card, leave the card in the slot, and switch from the screen:

1. Push-and-turn on the `mods` menu item. The list appears: `back`, then every
   `.bin` in `/modules`.
2. Turn to choose, press to load. The file is written into the Daisy
   bootloader's QSPI app slot (the same place a USB flash writes), verified
   byte for byte, and the module resets into the bootloader, which boots it.
   A few seconds; the card never leaves the slot.

What it needs, once: the Daisy bootloader in internal flash — board in ST ROM
DFU (hold **BOOT**, tap **RESET**), then `make -C firmware program-boot`. From
then on every app is a `BOOT_SRAM` build (all three are).

Keep `.bin` files **out of the card's root**: the bootloader itself flashes
the first root `.bin` it finds at every power-up, a cruder mechanism that would
fight the picker.

If a load fails the screen says why (`no card`, `no folder`, `flash build` for
an image built for internal flash, `verify failed`) and the running module
carries on. A failed write does leave the QSPI slot invalid until the next
successful load, so a power cycle in that state lands in the bootloader
waiting for USB — nothing is lost, it just needs a `make program-dfu`.

A switch is a reset: whatever is playing stops, and Mark's loops go with it.

## Build configuration

Set in the Makefile; the engine is one source shared with Move.

| Define | Value | Why |
|---|---|---|
| `MARK_TRACKS` | 4 | One track per audio jack pair |
| `MARK_CH` | 1 | Each track is fed by one **mono** jack, so stereo storage would hold the same signal twice. 28.8 MB instead of 57.6 MB for 4 tracks + undo at 60 s |
| `MARK_SR` | 48000 | libDaisy's `SaiHandle` offers no 44.1 kHz |
| `MARK_HOSTED_FX` | 0 | No `dlopen`/`dirent` on bare metal — tracks use built-in FX |
| `MARK_SESSIONS` | 0 | No filesystem *yet* — see below |

Memory: 32 MB SDRAM pool for 28.8 MB of buffers plus 1 MB of delay lines.
`mark_create()` steps down its own `alloc_seconds` ladder if that ever fails.

## Consequences of one jack per track

Both deliberate, both documented in `mark_core.h`:

- **Pan does nothing.** A mono output has nowhere to pan to. The `pan` param
  still round-trips through state; it just doesn't affect audio.
- **Monitor passes each track's own input**, not a shared one.

Level, master gain, one-shot, reverse, undo and the built-in FX all behave
exactly as on Move.

## Known gaps

- **No sessions.** The Patch *has* an SD card and libDaisy has FatFS, so
  `MARK_SESSIONS=1` backed by the card is the obvious next feature. That's why
  it's a flag and not a deletion.
- **No hosted audio_fx plugins.** Bare metal has no `dlopen`; tracks fall
  through to the built-in FX, which is a state the engine already handles.
- **CV outs cover tracks 1–2 only** — the Patch has exactly two.
- **Nothing has been heard on hardware.** Clean build, 20/20 native tests
  across five build configurations, ASan clean; that is all that is known.
