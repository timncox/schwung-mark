---
status: active
last_touched: 2026-07-12
---

# Mark

RC-505mkII-style **5-track live looper** for the Ableton Move, as a
schwung overtake module. Modeled on the Boss RC-505's workflow: per-track
record → play → overdub cycle, stop/clear, level/pan/reverse/one-shot,
measure-quantized loop sync, single-level undo/redo, All Start/Stop.
Named for the RC-505 **mkII** (whose undo is called MARK BACK).

Schwung = charlesvestal/schwung, the Shadow-UI sidecar framework for the
Move (LD_PRELOAD shim + QuickJS shadow UI + native ARM DSP modules).
Distribution repo: github.com/timncox/schwung-mark.

## Layout

- `src/mark_core.c` — engine: 5 loop tracks (record/overdub/undo-swap),
  MIDI-clock grid, pending-action scheduler, params. Non-allocating
  render path; all buffers allocated in `mark_create()` with a shrinking
  fallback ladder (60→45→30→20→15 s/track) if the device is tight on RAM.
  60 s/track = ~63 MB total including the undo buffer.
- `src/mark_gen.c` — plugin_api_v2 wrapper reading the hardware mailbox
  input; answers `module_id` = "mark" (manager tool discovery).
- `src/ui_overtake.js` — full-surface overtake UI (pads/knobs/screen/
  screen-reader). Strict-mode module: EVERY assigned identifier must be
  declared (smack lesson — the old host swallowed ReferenceErrors,
  schwung main treats them as fatal and exits the tool).
- `modules/overtake/mark/module.json` — the only build (overtake).

## Key conventions (inherited from smack, verified on hardware there)

- Trigger params fire on any ACTIVE value ("1"/"trigger"); "0"/"idle"
  are no-ops (autosave restore sends those).
- Clock: 24 ppqn via on_midi; 0xFA/0xFB reset the measure phase;
  `clock_governs` — running clock > bpm_override > stopped-clock memory
  > get_bpm(). Move must have MIDI Clock = Out for phase lock.
- `nclamp()` every state-JSON snprintf append (OOB lesson, v0.8.2).
- UI polls one `status` param per tick ("states|pending|pos|undo|swap");
  full fetch every 12 ticks; `resumeRepaints = 3` spaced forced repaints
  after resume (LED queue drops bursts).
- Feedback guard mirrored in the UI (host's slot guard can't see
  overtake modules): speakers active + no line-in = auto-mute monitor.
- Deploys only load at FULL exit (Shift+Vol+Jog from inside) + relaunch;
  Back+re-enter resumes the in-memory session with old code.

## Engine model

- Grid: running MIDI clock wins; otherwise the first finalized loop
  anchors `grid_unit` (frames/measure). Pending actions fire when the
  base track (lowest-numbered playing) crosses a grid multiple —
  frame-accurate; clocked-only fallback uses the measure flag (block
  granularity).
- Record stop rounds to the NEAREST whole measure: shorter truncates
  immediately, longer keeps recording to the target (RC behavior).
- Undo: kind 1 = first recording (undo clears, redo restores length);
  kind 0 = overdub (undo_buf holds pre-dub audio for the covered region,
  captured copy-before-write; undo = incremental swap ~32k frames/block
  with the track muted ~0.24 s max, so undo IS redo).
- Reversed tracks refuse overdub (RC rule). One-shot stops at loop end.
- v1 limitations: speed 1.0 only (no time-stretch/varispeed); presets
  save settings, not audio; no per-track FX; no web editor yet.

## Build / test / deploy

- `make test` — native compile + `test/host_sim.c` (8 sim tests: record
  quantize, grid alignment, overdub/undo/redo, reverse/one-shot,
  all/clear, state blob + buffer safety, monitor, clocked grid).
- `make arm` — Docker cross-compile (debian:bookworm +
  gcc-aarch64-linux-gnu, same `smack-build` image), tars INSIDE the
  container (macOS AppleDouble trap).
- `scripts/deploy.sh` — scp to ableton@move.local + rescan.

## Release process

Bump `modules/overtake/mark/module.json` version + root `release.json`,
`make arm`, commit/push, `gh release create vX` with
`build/mark-module.tar.gz`. Installer resolves release.json →
download_url. Not in the schwung catalog yet (add after hardware test).

## Not yet verified on hardware

Everything — first hardware pass pending: pad map / LED colors, blink
pacing, undo-swap mute audibility, measure alignment against Move's
clock, allocation ladder on-device, help.json rendering, screen-reader
output.
