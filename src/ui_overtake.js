/*
 * Mark — RC-505-style 5-track live looper, full-surface overtake UI.
 *
 * Surface map (pad columns 1-5 = tracks 1-5):
 *   Top row    92-96  track REC/PLAY/DUB button (the RC's [>/o]):
 *                     empty=dim, recording=red, playing=green,
 *                     overdubbing=yellow-green, stopped=white,
 *                     pending action blinks
 *              97     ALL START/STOP   98 UNDO/REDO   99 MONITOR
 *   Row 3      84-88  track STOP (tap = stop, hold = clear)
 *              89 QUANTIZE  90 REC ACTION (rec->play / rec->dub)
 *              91 DUB MODE (overdub / replace)
 *   Row 2      76-80  track REVERSE toggles
 *              81 PLAY MODE (multi / single = song sections)
 *              82 SESSION MODE: steps become save slots 1-16
 *                 (tap a slot = load, hold = save)
 *   Bottom row 68-72  track FX on/off; Shift+tap = one-shot toggle
 *   Steps 1-16        playhead chase of the base loop; session slots
 *                     while session mode is on
 *   Knobs 1-5         track levels     Knob 6 master
 *   Knob 7            current track's FX type   Knob 8 its FX amount
 *   Shift+Knobs 1-5   track pans       6 monitor  7 follow  8 BPM override
 *   Jog wheel         master level
 *   Play button       passed through to Move (transport + clock keep working)
 *
 * Talks to the DSP via host_module_set_param/get_param (overtake_dsp shims).
 * Screen reader: everything announced via shared/screen_reader.mjs.
 */

import {
    MoveKnob1, MoveShift, MoveMainKnob,
    Black, White, LightGrey, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/schwung/shared/menu_layout.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';

const TRACKS = 5;

const PAD_TRACK = [92, 93, 94, 95, 96];
const PAD_ALL   = 97;
const PAD_UNDO  = 98;
const PAD_MON   = 99;
const PAD_STOP  = [84, 85, 86, 87, 88];
const PAD_QUANT = 89;
const PAD_RECACT = 90;
const PAD_DUBMD = 91;
const PAD_REV   = [76, 77, 78, 79, 80];
const PAD_PMODE = 81;    /* multi / single play mode */
const PAD_SESS  = 82;    /* session mode: steps = save slots */
const PAD_FX    = [68, 69, 70, 71, 72];   /* FX on/off; Shift = one-shot */
const PADS_DARK = [83, 73, 74, 75];

const STEP_FIRST = 16;
const STEP_COUNT = 16;

/* Track states (mk_tstate_t) */
const ST_EMPTY = 0, ST_REC = 1, ST_PLAY = 2, ST_DUB = 3, ST_STOP = 4;
const ST_NAMES  = ['-', 'REC', 'PLAY', 'DUB', 'STOP'];
const ST_SPEECH = ['empty', 'recording', 'playing', 'overdubbing', 'stopped'];

/* Track FX types (tfx_t in the DSP) */
const FX_NAMES  = ['Off', 'LPF', 'HPF', 'Crush', 'Delay', 'Phasr', 'Ring'];
const FX_SPEECH = ['off', 'low pass filter', 'high pass filter', 'crush',
                   'delay', 'phaser', 'ring mod'];

/* Hold a stop pad this long to clear the track (RC long-press clear);
 * hold a session slot this long to SAVE into it (tap = load) */
const CLEAR_HOLD_MS = 600;
const SAVE_HOLD_MS = 600;

/* Knobs 1-8, page 1 */
const KNOBS = [
    { key: 't1_level', name: 'T1',  min: 0, max: 200, step: 5, speech: 'Track 1 level' },
    { key: 't2_level', name: 'T2',  min: 0, max: 200, step: 5, speech: 'Track 2 level' },
    { key: 't3_level', name: 'T3',  min: 0, max: 200, step: 5, speech: 'Track 3 level' },
    { key: 't4_level', name: 'T4',  min: 0, max: 200, step: 5, speech: 'Track 4 level' },
    { key: 't5_level', name: 'T5',  min: 0, max: 200, step: 5, speech: 'Track 5 level' },
    { key: 'master',   name: 'Mst', min: 0, max: 200, step: 5, speech: 'Master level' },
    null,
    null
];

/* Knob page 2 — held-Shift layer */
const KNOBS2 = [
    { key: 't1_pan', name: 'Pn1', min: 0, max: 100, step: 5, speech: 'Track 1 pan' },
    { key: 't2_pan', name: 'Pn2', min: 0, max: 100, step: 5, speech: 'Track 2 pan' },
    { key: 't3_pan', name: 'Pn3', min: 0, max: 100, step: 5, speech: 'Track 3 pan' },
    { key: 't4_pan', name: 'Pn4', min: 0, max: 100, step: 5, speech: 'Track 4 pan' },
    { key: 't5_pan', name: 'Pn5', min: 0, max: 100, step: 5, speech: 'Track 5 pan' },
    { key: 'monitor', name: 'Mon', opts: ['Mute', 'On'],
      speech: 'Monitoring', speechOpts: ['muted', 'on'] },
    { key: 'follow',  name: 'Flw', opts: ['Free', 'Flw'],
      speech: 'Transport', speechOpts: ['free', 'follow'] },
    { key: 'bpm_override', name: 'BPM', min: 49, max: 200, step: 1,
      speech: 'BPM override', isBpm: true }
];

let knobValues  = [100, 100, 100, 100, 100, 100, 0, 0];
let knob2Values = [50, 50, 50, 50, 50, 1, 0, 0];

let tstates  = [0, 0, 0, 0, 0];
let tpend    = [0, 0, 0, 0, 0];
let tpos     = [0, 0, 0, 0, 0];
let tmeas    = [0, 0, 0, 0, 0];
let trev     = [0, 0, 0, 0, 0];
let tshot    = [0, 0, 0, 0, 0];
let tfx      = [1, 1, 1, 1, 1];
let tfxon    = [0, 0, 0, 0, 0];
let tfxp     = [50, 50, 50, 50, 50];
let undoAvail = 0;      /* 0 none, 1 undo, 2 redo */
let swapBusy  = 0;
let quantize  = 1;
let recAction = 0;
let dubMode   = 0;
let playMode  = 0;      /* 0 multi, 1 single */
let gridBpm   = 120;
let monitorOn = true;
let bpmOverride = 0;

let shiftHeld = false;
let tickCount = 0;
let needsRedraw = true;
let dspReady = false;
let chaseStep = -1;
let anyPending = false;
let curTrack = 0;       /* target of the FX knobs: last track pad touched */

/* Session mode: steps show the 16 save slots */
let sessionMode = false;
let sessSlots = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
let sessHeld = null;    /* { slot, at, fired } while a slot pad is down */
let ioWatch = false;    /* watch session_status until the worker finishes */
let ioWasBusy = '';     /* 'saving' | 'loading' while in flight */

/* stop-pad hold tracking: { at, fired } per track, null when up */
let stopHeld = [null, null, null, null, null];

/* Feedback guard (smack pattern): speakers on + no line-in cable = the
 * internal mic would feed the speakers. Mute the DSP's input monitoring
 * until it's safe or the user overrides via the Monitor pad. */
let guardMuted = false;
let guardOverride = false;

let resumeRepaints = 0;   /* spaced forced repaints after resume: one
                             paintAll(true) is ~50 LED writes in a tick and
                             the shim queue can drop some (smack lesson) */

function gp(key) {
    const v = host_module_get_param(key);
    return (v === null || v === undefined) ? null : String(v);
}

function feedbackRisk() {
    if (typeof host_speaker_active !== 'function') return false;
    if (typeof host_line_in_connected !== 'function') return false;
    return host_speaker_active() && !host_line_in_connected();
}

function setMonitor(on) {
    monitorOn = on;
    host_module_set_param('monitor', on ? '1' : '0');
    paintGlobals(false);
    needsRedraw = true;
}

function reconcileFeedbackGuard() {
    const risk = feedbackRisk();
    if (risk && monitorOn && !guardOverride) {
        guardMuted = true;
        setMonitor(false);
        announce('Feedback risk. Input muted. Monitor pad to override, or plug in headphones.');
    } else if (!risk && guardMuted) {
        guardMuted = false;
        guardOverride = false;
        setMonitor(true);
        announce('Monitoring restored');
    }
}

/* ---- state polling ---- */

function parseCsv(str, into) {
    const v = (str || '').split(',');
    for (let i = 0; i < TRACKS; i++) into[i] = parseInt(v[i], 10) || 0;
}

/* one get_param per tick: "st|pend|pos|undo|swap" */
function pollStatus() {
    const sv = gp('status');
    if (sv === null) return false;
    const parts = sv.split('|');
    const old = tstates.join(',');
    const oldPend = tpend.join(',');
    parseCsv(parts[0], tstates);
    parseCsv(parts[1], tpend);
    parseCsv(parts[2], tpos);
    undoAvail = parseInt(parts[3], 10) || 0;
    swapBusy = parseInt(parts[4], 10) || 0;
    anyPending = tpend.some(p => p !== 0);

    if (tstates.join(',') !== old) {
        parseCsv(gp('tmeas'), tmeas);
        const oldArr = old.split(',').map(Number);
        for (let i = 0; i < TRACKS; i++) {
            if (tstates[i] === oldArr[i]) continue;
            if (tstates[i] === ST_PLAY && oldArr[i] === ST_REC)
                announce(`Track ${i + 1} looping, ${tmeas[i]} measure${tmeas[i] === 1 ? '' : 's'}`);
            else
                announce(`Track ${i + 1} ${ST_SPEECH[tstates[i]]}`);
        }
        paintTracks(false);
        paintGlobals(false);
        needsRedraw = true;
    } else if (tpend.join(',') !== oldPend) {
        paintTracks(false);
        needsRedraw = true;
    }
    return true;
}

function fetchAll() {
    const rs = gp('run_state');
    if (rs === null) return false;       /* DSP not up yet — retry in tick */
    for (let i = 0; i < KNOBS.length; i++) {
        if (!KNOBS[i]) continue;
        const v = gp(KNOBS[i].key);
        if (v !== null) knobValues[i] = parseFloat(v) || 0;
    }
    for (let i = 0; i < KNOBS2.length; i++) {
        if (!KNOBS2[i]) continue;
        const v = gp(KNOBS2[i].key);
        if (v !== null) knob2Values[i] = parseFloat(v) || 0;
    }
    for (let i = 0; i < TRACKS; i++) {
        trev[i]  = parseInt(gp(`t${i + 1}_rev`) || '0', 10);
        tshot[i] = parseInt(gp(`t${i + 1}_shot`) || '0', 10);
        tfx[i]   = parseInt(gp(`t${i + 1}_fx`) || '1', 10);
        tfxon[i] = parseInt(gp(`t${i + 1}_fx_on`) || '0', 10);
        tfxp[i]  = parseInt(gp(`t${i + 1}_fxp`) || '50', 10);
    }
    parseCsv(gp('tmeas'), tmeas);
    quantize  = parseInt(gp('quantize') || '1', 10);
    recAction = parseInt(gp('rec_action') || '0', 10);
    dubMode   = parseInt(gp('dub_mode') || '0', 10);
    playMode  = parseInt(gp('play_mode') || '0', 10);
    gridBpm   = parseInt(gp('grid_bpm') || '120', 10);
    monitorOn = (gp('monitor') || '1') !== '0';
    bpmOverride = parseFloat(gp('bpm_override')) || 0;
    if (sessionMode) fetchSlots();
    pollStatus();
    return true;
}

function fetchSlots() {
    const v = (gp('session_slots') || '').split(',');
    for (let i = 0; i < 16; i++) sessSlots[i] = parseInt(v[i], 10) || 0;
}

/* ---- knobs ---- */

function knobDisplay(bank, i) {
    const k = bank === 2 ? KNOBS2[i] : KNOBS[i];
    const vals = bank === 2 ? knob2Values : knobValues;
    if (!k) return '';
    if (k.isBpm) return vals[i] > 0 ? `${Math.round(vals[i])}` : 'Off';
    if (k.opts) {
        const idx = Math.max(0, Math.min(k.opts.length - 1, Math.round(vals[i])));
        return k.opts[idx];
    }
    return `${Math.round(vals[i])}`;
}

function adjustKnob(bank, i, delta) {
    const k = bank === 2 ? KNOBS2[i] : KNOBS[i];
    const vals = bank === 2 ? knob2Values : knobValues;
    if (!k) return;
    if (k.isBpm) {
        /* 49 and below = Off (project tempo); 50-200 = override */
        const cur = vals[i] > 0 ? vals[i] : 49;
        const v = Math.max(49, Math.min(200, Math.round(cur) + delta * k.step));
        const out = v < 50 ? 0 : v;
        vals[i] = out;
        bpmOverride = out;
        host_module_set_param(k.key, `${out}`);
        announceParameter(k.speech, out > 0 ? `${out}` : 'off, project tempo');
        needsRedraw = true;
        return;
    }
    const max = k.opts ? k.opts.length - 1 : k.max;
    const min = k.opts ? 0 : k.min;
    const step = k.opts ? 1 : k.step;
    const v = Math.max(min, Math.min(max, vals[i] + delta * step));
    if (v === vals[i]) return;
    vals[i] = v;
    host_module_set_param(k.key, `${Math.round(v)}`);
    if (k.speechOpts) announceParameter(k.speech, k.speechOpts[Math.round(v)]);
    else announceParameter(k.speech, `${Math.round(v)}`);
    if (k.key === 'monitor') {
        monitorOn = Math.round(v) !== 0;
        guardMuted = false;
        paintGlobals(false);
    }
    needsRedraw = true;
}

/* Knobs 7/8: FX type + amount for the current track */
function adjustFxType(delta) {
    const v = Math.max(0, Math.min(FX_NAMES.length - 1, tfx[curTrack] + (delta > 0 ? 1 : -1)));
    if (v === tfx[curTrack]) return;
    tfx[curTrack] = v;
    host_module_set_param(`t${curTrack + 1}_fx`, `${v}`);
    announceParameter(`Track ${curTrack + 1} effect`, FX_SPEECH[v]);
    needsRedraw = true;
}

function adjustFxParam(delta) {
    const v = Math.max(0, Math.min(100, tfxp[curTrack] + delta * 5));
    if (v === tfxp[curTrack]) return;
    tfxp[curTrack] = v;
    host_module_set_param(`t${curTrack + 1}_fxp`, `${v}`);
    announceParameter(`Track ${curTrack + 1} effect amount`, `${v}`);
    needsRedraw = true;
}

/* ---- LEDs ---- */

function trackColor(i) {
    if (tpend[i]) {   /* scheduled action blinks */
        const on = (tickCount % 8) < 4;
        if (!on) return Black;
        return tpend[i] === 1 ? BrightRed : Green;
    }
    switch (tstates[i]) {
    case ST_REC:  return BrightRed;
    case ST_PLAY: return Green;
    case ST_DUB:  return YellowGreen;
    case ST_STOP: return White;
    default:      return 0x10;
    }
}

function paintTracks(force) {
    for (let i = 0; i < TRACKS; i++) {
        setLED(PAD_TRACK[i], trackColor(i), force);
        let stopColor = Black;
        if (tstates[i] === ST_PLAY || tstates[i] === ST_DUB || tstates[i] === ST_REC)
            stopColor = LightGrey;
        else if (tstates[i] === ST_STOP)
            stopColor = White;
        setLED(PAD_STOP[i], stopColor, force);
        setLED(PAD_REV[i], trev[i] ? Blue : 0x10, force);
        /* bottom row: FX on/off; Shift reveals the one-shot layer */
        setLED(PAD_FX[i],
               shiftHeld ? (tshot[i] ? Purple : 0x10)
                         : (tfxon[i] && tfx[i] > 0 ? OrangeRed : 0x10),
               force);
    }
}

function paintGlobals(force) {
    const anyPlay = tstates.some(s => s === ST_PLAY || s === ST_DUB);
    const anyContent = tstates.some(s => s !== ST_EMPTY);
    setLED(PAD_ALL, anyPlay ? Green : (anyContent ? White : 0x10), force);
    setLED(PAD_UNDO, swapBusy ? BrightGreen
                              : (undoAvail === 1 ? OrangeRed
                              : (undoAvail === 2 ? Cyan : Black)), force);
    setLED(PAD_MON, monitorOn ? Green : BrightRed, force);
    setLED(PAD_QUANT, quantize ? Cyan : 0x10, force);
    setLED(PAD_RECACT, recAction ? OrangeRed : 0x10, force);
    setLED(PAD_DUBMD, dubMode ? BrightRed : 0x10, force);
    setLED(PAD_PMODE, playMode ? Cyan : 0x10, force);
    setLED(PAD_SESS, sessionMode ? White : 0x10, force);
    for (let i = 0; i < PADS_DARK.length; i++) setLED(PADS_DARK[i], Black, force);
}

function paintSteps(force) {
    if (sessionMode) {
        /* the 16 save slots: green = has a session, dim = empty */
        for (let i = 0; i < STEP_COUNT; i++) {
            let c = sessSlots[i] ? Green : 0x10;
            if (sessHeld && sessHeld.slot === i) c = White;
            setLED(STEP_FIRST + i, c, force);
        }
        chaseStep = -2;
        return;
    }
    /* chase the base loop: lowest-numbered playing track */
    let ref = -1;
    for (let i = 0; i < TRACKS; i++)
        if (tstates[i] === ST_PLAY || tstates[i] === ST_DUB) { ref = i; break; }
    const step = ref >= 0 ? Math.min(15, Math.floor(tpos[ref] / 8)) : -1;
    if (step === chaseStep && !force) return;
    chaseStep = step;
    for (let i = 0; i < STEP_COUNT; i++)
        setLED(STEP_FIRST + i, i === step ? White : (i < step ? 0x10 : Black), force);
}

function paintAll(force) {
    paintTracks(force);
    paintGlobals(force);
    paintSteps(force);
}

/* ---- Screen ---- */

function drawUI() {
    clear_screen();
    let title = `MARK  @${bpmOverride > 0 ? Math.round(bpmOverride) : gridBpm}`;
    if (swapBusy) title += ' UNDO...';
    drawHeader(title);

    /* five track columns: state / measures / level (pan when shifted) */
    for (let i = 0; i < TRACKS; i++) {
        const x = 2 + i * 25;
        print(x, 13, `${i + 1}${trev[i] ? '<' : ''}${tshot[i] ? '!' : ''}`, 1);
        print(x, 22, tpend[i] ? '...' : ST_NAMES[tstates[i]], 1);
        print(x, 31, tstates[i] === ST_EMPTY ? '' : `${tmeas[i]}m`, 1);
        print(x, 40, shiftHeld ? knobDisplay(2, i) : knobDisplay(1, i), 1);
    }

    /* footer: ~20 chars budget, one hint set at a time (smack lesson) */
    let fLeft, fRight;
    if (sessionMode) {
        fLeft = 'tap load · hold save';
        fRight = '';
    } else if (shiftHeld) {
        fLeft = 'Pans · Mon · BPM';
        fRight = '';
    } else {
        fLeft = `T${curTrack + 1} ${FX_NAMES[tfx[curTrack]]}` +
                (tfxon[curTrack] && tfx[curTrack] > 0 ? ` ${tfxp[curTrack]}` : ' off');
        fRight = `${playMode ? 'SGL' : 'MLT'} ${quantize ? 'Q' : ''}`;
    }
    drawFooter({ left: fLeft, right: fRight });
    needsRedraw = false;
}

/* ---- Lifecycle ---- */

globalThis.init = function() {
    dspReady = fetchAll();
    paintAll(true);
    needsRedraw = true;
    if (dspReady) {
        announceView('Mark, 5 track looper');
        reconcileFeedbackGuard();
    }
};

globalThis.onResume = function() {
    dspReady = fetchAll();
    paintAll(true);
    resumeRepaints = 3;
    needsRedraw = true;
    announceView('Mark');
    reconcileFeedbackGuard();
};

globalThis.tick = function() {
    tickCount++;

    if (!dspReady) {
        dspReady = fetchAll();
        if (dspReady) {
            paintAll(true);
            needsRedraw = true;
            announceView('Mark, 5 track looper');
            reconcileFeedbackGuard();
        } else if (needsRedraw) {
            drawUI();
        }
        return;
    }

    /* jack state can change mid-session — re-check about twice a second */
    if (tickCount % 15 === 0) reconcileFeedbackGuard();

    /* spaced post-resume repaints heal any LED writes the queue dropped */
    if (resumeRepaints > 0 && tickCount % 8 === 0) {
        paintAll(true);
        needsRedraw = true;
        resumeRepaints--;
    }

    /* stop-pad holds: long-press clears the track */
    for (let i = 0; i < TRACKS; i++) {
        if (stopHeld[i] && !stopHeld[i].fired &&
            Date.now() - stopHeld[i].at >= CLEAR_HOLD_MS) {
            stopHeld[i].fired = true;
            host_module_set_param(`t${i + 1}_clear`, '1');
            announce(`Track ${i + 1} cleared`);
            refreshSoon();
        }
    }

    /* session-slot hold: long-press SAVES into the slot (tap = load) */
    if (sessHeld && !sessHeld.fired &&
        Date.now() - sessHeld.at >= SAVE_HOLD_MS) {
        sessHeld.fired = true;
        host_module_set_param('save_session', `${sessHeld.slot + 1}`);
        announce(`Saving slot ${sessHeld.slot + 1}`);
        ioWatch = true;
        ioWasBusy = 'saving';
    }

    /* watch the session worker until it settles, then report */
    if (ioWatch && tickCount % 6 === 0) {
        const s = gp('session_status') || 'idle';
        if (s !== 'saving' && s !== 'loading') {
            ioWatch = false;
            if (s === 'error') announce('Session error');
            else announce(ioWasBusy === 'saving' ? 'Saved' : 'Loaded');
            ioWasBusy = '';
            fetchSlots();
            paintSteps(true);
            refreshSoon();
        }
    }

    /* per-tick status poll drives blink, chase and async transitions */
    pollStatus();
    if (anyPending && tickCount % 4 === 0) paintTracks(false);
    paintSteps(false);

    /* periodic full refresh: knob edits, toggles, measures — including
     * changes arriving from the web editor */
    if (tickCount % 12 === 0) {
        const oldKnobs = knobValues.join(',') + knob2Values.join(',');
        const oldFlags = `${quantize}${recAction}${dubMode}${monitorOn}${playMode}` +
                         trev.join('') + tshot.join('') +
                         tfx.join('') + tfxon.join('') + tfxp.join(',');
        fetchAll();
        if (knobValues.join(',') + knob2Values.join(',') !== oldKnobs)
            needsRedraw = true;
        if (`${quantize}${recAction}${dubMode}${monitorOn}${playMode}` +
            trev.join('') + tshot.join('') +
            tfx.join('') + tfxon.join('') + tfxp.join(',') !== oldFlags) {
            paintTracks(false);
            paintGlobals(false);
            needsRedraw = true;
        }
    }

    if (needsRedraw) drawUI();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            const was = shiftHeld;
            shiftHeld = d2 >= 64;
            if (was !== shiftHeld) {
                paintTracks(false);   /* bottom row swaps FX <-> one-shot */
                needsRedraw = true;
            }
            return;
        }
        /* jog wheel: master level */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) adjustKnob(1, 5, delta);
            return;
        }
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
            const k = d1 - MoveKnob1;
            if (!shiftHeld && k === 6) { adjustFxType(delta); return; }
            if (!shiftHeld && k === 7) { adjustFxParam(delta); return; }
            adjustKnob(shiftHeld ? 2 : 1, k, delta);
            return;
        }
        return;
    }

    /* pad releases: end hold windows; a short session-slot tap = LOAD */
    if (status === 0x80 || (status === 0x90 && d2 === 0)) {
        for (let i = 0; i < TRACKS; i++) {
            if (stopHeld[i] && d1 === PAD_STOP[i]) {
                stopHeld[i] = null;
                return;
            }
        }
        if (sessHeld && d1 === STEP_FIRST + sessHeld.slot) {
            if (!sessHeld.fired) {
                if (sessSlots[sessHeld.slot]) {
                    host_module_set_param('load_session', `${sessHeld.slot + 1}`);
                    announce(`Loading slot ${sessHeld.slot + 1}`);
                    ioWatch = true;
                    ioWasBusy = 'loading';
                } else {
                    announce(`Slot ${sessHeld.slot + 1} empty. Hold to save.`);
                }
            }
            sessHeld = null;
            paintSteps(true);
            return;
        }
        return;
    }

    if (status === 0x90 && d2 > 0) {
        /* session mode: step pads are the save slots */
        if (sessionMode && d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            sessHeld = { slot: d1 - STEP_FIRST, at: Date.now(), fired: false };
            paintSteps(true);
            return;
        }
        for (let i = 0; i < TRACKS; i++) {
            if (d1 === PAD_TRACK[i]) {
                curTrack = i;
                host_module_set_param(`t${i + 1}_btn`, '1');
                refreshSoon();
                return;
            }
            if (d1 === PAD_STOP[i]) {
                /* stop fires on press (RC behavior); keep holding to clear */
                curTrack = i;
                host_module_set_param(`t${i + 1}_stop`, '1');
                if (tstates[i] !== ST_EMPTY) announce(`Track ${i + 1} stop`);
                stopHeld[i] = { at: Date.now(), fired: false };
                refreshSoon();
                return;
            }
            if (d1 === PAD_REV[i]) {
                curTrack = i;
                trev[i] = trev[i] ? 0 : 1;
                host_module_set_param(`t${i + 1}_rev`, `${trev[i]}`);
                announce(`Track ${i + 1} reverse ${trev[i] ? 'on' : 'off'}`);
                paintTracks(false);
                needsRedraw = true;
                return;
            }
            if (d1 === PAD_FX[i]) {
                curTrack = i;
                if (shiftHeld) {   /* Shift layer: one-shot toggle */
                    tshot[i] = tshot[i] ? 0 : 1;
                    host_module_set_param(`t${i + 1}_shot`, `${tshot[i]}`);
                    announce(`Track ${i + 1} one-shot ${tshot[i] ? 'on' : 'off'}`);
                } else {
                    tfxon[i] = tfxon[i] ? 0 : 1;
                    host_module_set_param(`t${i + 1}_fx_on`, `${tfxon[i]}`);
                    announce(`Track ${i + 1} ${FX_SPEECH[tfx[i]]} ${tfxon[i] ? 'on' : 'off'}`);
                }
                paintTracks(false);
                needsRedraw = true;
                return;
            }
        }
        if (d1 === PAD_PMODE) {
            playMode = playMode ? 0 : 1;
            host_module_set_param('play_mode', `${playMode}`);
            announce(playMode ? 'Single mode: tracks are song sections'
                              : 'Multi mode');
            paintGlobals(false);
            needsRedraw = true;
            return;
        }
        if (d1 === PAD_SESS) {
            sessionMode = !sessionMode;
            if (sessionMode) {
                fetchSlots();
                announce('Sessions. Tap a step to load, hold to save.');
            } else {
                sessHeld = null;
                announce('Session mode off');
            }
            paintGlobals(false);
            paintSteps(true);
            needsRedraw = true;
            return;
        }
        if (d1 === PAD_ALL) {
            host_module_set_param('all_btn', '1');
            announce('All start stop');
            refreshSoon();
            return;
        }
        if (d1 === PAD_UNDO) {
            if (undoAvail === 0) { announce('Nothing to undo'); return; }
            host_module_set_param('undo', '1');
            announce(undoAvail === 2 ? 'Redo' : 'Undo');
            refreshSoon();
            return;
        }
        if (d1 === PAD_MON) {
            if (monitorOn) {
                guardMuted = false;
                guardOverride = false;
                setMonitor(false);
                announce('Input muted');
            } else {
                guardMuted = false;
                if (feedbackRisk()) {
                    guardOverride = true;
                    announce('Monitoring on. Feedback risk!');
                } else {
                    announce('Monitoring on');
                }
                setMonitor(true);
            }
            return;
        }
        if (d1 === PAD_QUANT) {
            quantize = quantize ? 0 : 1;
            host_module_set_param('quantize', `${quantize}`);
            announce(quantize ? 'Quantize measure' : 'Quantize off');
            paintGlobals(false);
            needsRedraw = true;
            return;
        }
        if (d1 === PAD_RECACT) {
            recAction = recAction ? 0 : 1;
            host_module_set_param('rec_action', `${recAction}`);
            announce(recAction ? 'Record then overdub' : 'Record then play');
            paintGlobals(false);
            needsRedraw = true;
            return;
        }
        if (d1 === PAD_DUBMD) {
            dubMode = dubMode ? 0 : 1;
            host_module_set_param('dub_mode', `${dubMode}`);
            announce(dubMode ? 'Replace mode' : 'Overdub mode');
            paintGlobals(false);
            needsRedraw = true;
            return;
        }
    }
};

globalThis.onUnload = function() {
    /* Host unloads the slot-0 DSP and restores LEDs; nothing to release. */
};

/* pull fresh state on the next periodic refresh instead of immediately —
 * quantized actions land later anyway */
function refreshSoon() {
    tickCount = -1; /* forces the %12 refresh on next tick */
}
