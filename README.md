# Mark

**RC-505-style 5-track live looper for the Ableton Move**, built on
[schwung](https://github.com/charlesvestal/schwung).

Five independent stereo loop tracks, each cycling **record → play →
overdub** with its own stop/clear, level, pan, reverse and one-shot —
the Boss RC-505 workflow on the Move's pad grid. Loop lengths snap to
whole measures; the first loop (or Move's MIDI clock) defines the grid
and every later track starts and ends in sync. Single-level **undo/redo**
of the last recording or overdub. Move keeps playing underneath, so its
own tracks are your rhythm section.

The name: RC-505 **mkII** → Mark. (Its undo feature is literally called
MARK BACK.)

## Install

In schwung-manager, choose **Install Custom Module** and give it this
repository URL (`https://github.com/timncox/schwung-mark`), or upload
`mark-module.tar.gz` from the latest release.

Launch from the overtake menu (**Shift+Vol+Jog-Click**). Back suspends
the UI (audio keeps running); Shift+Vol+Jog-Click exits.

## Surface

Rows counted from the top, as you look at the device:

| | Col 1-5 (tracks) | Col 6 | Col 7 | Col 8 |
|---|---|---|---|---|
| **Row 1 (top)** | REC / PLAY / DUB | All start/stop | Undo/redo | Monitor |
| **Row 2** | Stop (hold = clear) | Quantize | Rec→Play / Rec→Dub | Overdub / Replace |
| **Row 3** | Reverse | Play mode (multi/single) | Sessions | — |
| **Row 4 (bottom)** | FX on/off (Shift = one-shot) | — | — | — |

Track button colors: dim = empty, **red** = recording, **green** =
playing, **yellow** = overdubbing, **white** = stopped, blinking =
waiting for the next measure.

Knobs 1–5 = track levels, knob 6 / jog wheel = master, knob 7 = FX type
and knob 8 = FX amount for the last track touched. Hold **Shift**:
knobs 1–5 = pans, 6 = monitor, 7 = transport follow, 8 = BPM override.
Steps 1–16 chase the base loop's playhead — or show the 16 **session
slots** while session mode is on (tap = load, hold = save; sessions
store loop audio + settings on the Move and survive reinstalls).

**Track FX**: one insert per track — low-pass, high-pass, crush,
tempo-synced delay, phaser, ring mod. **Single mode** makes the five
tracks exclusive song sections. The **web mixer** (move.local → Remote
UI → Tool tab, manager > v0.11.4) has faders, pans, FX, modes, and the
session grid.

## Sync

Set Move's **MIDI Clock to Out** (Settings → MIDI Sync) to lock loops to
Move's transport. Without clock, Mark free-runs at the project tempo and
the first recorded loop anchors the grid.

## Notes

- Records whatever input Move has selected (mic, line-in, USB-C).
- The feedback guard mutes input monitoring when the speaker is live
  with no line-in cable; the Monitor pad overrides.
- Reversed tracks can't overdub (RC-505 rule).
- Presets save mixer settings; sessions save the audio too.
- Per-track capacity 60 s (falls back gracefully on low memory).

## Development

```
make test    # native build + simulation tests (no hardware)
make arm     # Docker cross-compile for the Move + tarball
scripts/deploy.sh   # scp straight onto move.local + rescan
```

MIT. Engine patterns borrowed from [schwung-smack](https://github.com/timncox/schwung-smack).
