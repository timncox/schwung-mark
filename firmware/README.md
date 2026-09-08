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

Builds `BOOT_NONE` at **100,516 B of 131,072 B (76.7%)**, so it flashes with
the STM32 ROM DFU in silicon: hold **BOOT**, tap **RESET**, release BOOT. No
bootloader install, no QSPI write.

## Panel — every connector does something

| Connector | Function |
|---|---|
| Audio In 1–4 | Track 1–4 record source |
| Audio Out 1–4 | Track 1–4 playback |
| CV In 1–4 | See `cvmode` below |
| Gate In 1 | All start / stop |
| Gate In 2 | Undo / redo |
| **Gate Out** | **Track 1 loop boundary** |
| **CV Out 1 / 2** | **Track 1 / 2 playhead, 0–5 V ramp per loop** |
| Encoder turn | Page: `LEVEL` / `GLOBAL` |
| Encoder press | All start / stop |
| Knobs 1–4 | The current page's four params |
| MIDI In | CC to the engine |

The gate output and CV outputs are the reason to run Mark here rather than on
the Move. A looper that emits its own loop-boundary trigger and playhead ramps
drives the rest of the rack at exactly the loop length the player recorded —
which no amount of dividing a master clock will give you.

## `cvmode` — what the four CV inputs do

Runtime, not build-time, because both jobs are legitimate and which is right
depends on the patch. Set it on the `GLOBAL` page, knob 4.

**`cvmode 1` (default) — LEVEL.** Each CV input rides its own track's level,
knob and CV summing as the hardware intends. Transport comes from the gate
inputs, which is the footswitch workflow anyway.

**`cvmode 0` — TRANSPORT.** Each CV input is a threshold trigger for its own
track's record/play button, giving per-track control from a sequencer.

The catch is hardware, and it is why this is a mode rather than the default:
**on the Daisy Patch each CV jack is summed with its knob.** In `cvmode 0` the
knobs must sit near zero or they hold the trigger permanently high. The display
warns (`park N knobs`) when a knob is too high for the mode to work.

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
