/*
 * Mark — overtake UI harness.
 *
 * Built to measure, not just to check values. host_module_get_param is a
 * BLOCKING round-trip to the shim, serviced once per SPI frame (~23 ms) and
 * abandoned after 100 ms; the channel serves roughly 44 reads a second in
 * total. A UI that asks for more than that starves itself — reads start
 * returning null, and any code that folds null into a literal default then
 * writes that default back to the DSP on the next knob turn.
 *
 * That exact chain shipped three hardware bugs in the sibling module Work
 * while its 445-check engine suite stayed green, because none of them were
 * engine bugs. So this harness counts round-trips and models a channel that
 * can FAIL.
 *
 * Run via `make test`.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_overtake.js'), 'utf8');

const MoveKnob1 = 71, MoveShift = 49, MoveMainKnob = 14;
const MoveLeft = 62, MoveRight = 63;

const constants = {
    MoveKnob1, MoveShift, MoveMainKnob, MoveLeft, MoveRight,
    Black: 0, White: 120, LightGrey: 118, BrightRed: 1, Blue: 125,
    Green: 126, BrightGreen: 8, Cyan: 14, Purple: 22, YellowGreen: 30,
    OrangeRed: 2
};

const TRACKS = 5;

/* A plausible engine state. `status` is pipe-delimited with CSV fields. */
const params = new Map([
    ['status', [
        new Array(TRACKS).fill(0).join(','),      /* track states   */
        new Array(TRACKS).fill(0).join(','),      /* pending        */
        new Array(TRACKS).fill(0).join(','),      /* positions      */
        '0', '0',                                 /* undo, swapBusy */
        new Array(TRACKS).fill(1).join(','),      /* units          */
        new Array(TRACKS).fill(16).join(',')      /* length in 16ths*/
    ].join('|')],
    ['run_state', '0'], ['session_slots', ''], ['session_status', 'idle'],
    ['master', '100'], ['tmeas', '1'], ['quantize', '1'], ['rec_grid', '0'],
    ['rec_action', '0'], ['dub_mode', '0'], ['play_mode', '0'],
    ['grid_bpm', '120'], ['monitor', '1'], ['bpm_override', '0'],
    ['follow', '1']
]);
for (let t = 1; t <= TRACKS; t++) {
    params.set(`t${t}_level`, '100');
    params.set(`t${t}_pan`, '50');
    params.set(`t${t}_rev`, '0');
    params.set(`t${t}_shot`, '0');
    params.set(`t${t}_fx`, '0');
    params.set(`t${t}_fx_on`, '0');
    params.set(`t${t}_fxp`, '0');
    params.set(`t${t}_fx_module`, '');
    params.set(`t${t}_fx_status`, '0');
}

let roundTrips = 0;
let readFailures = 0;
const announcements = [];
const writes = [];

const context = vm.createContext({
    console, Math, Number, JSON, String, Array, parseInt, parseFloat, isFinite, Date,
    clear_screen() {}, print() {}, fill_rect() {}, draw_rect() {},
    text_width(t) { return String(t).length * 6; },
    move_midi_internal_send() {},
    host_speaker_active() { return false; },
    host_line_in_connected() { return true; },
    host_get_module_metadata() { return null; },
    host_module_get_param(key) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
        return params.get(key) ?? '0';
    },
    /* BULK_GET, transcribed from shim_handle_param_bulk / bulk_next in
     * schwung's src/schwung_shim.c — NOT from the code under test. Request is
     * "<count>\n" then count records of "<len>\n<key>"; the response is the
     * same shape carrying values in the same order. The shim rejects more than
     * 64 items outright. */
    host_module_get_params(blob) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
        const text = String(blob);
        let at = 0;
        const readLen = () => {
            let n = 0, any = false;
            while (at < text.length && text[at] >= '0' && text[at] <= '9') {
                n = n * 10 + (text.charCodeAt(at) - 48); at++; any = true;
            }
            if (!any || text[at] !== '\n') return -1;
            at++;
            return n;
        };
        const count = readLen();
        if (count < 0 || count > 64) return null;
        const keys = [];
        for (let i = 0; i < count; i++) {
            const len = readLen();
            if (len < 0) return null;
            keys.push(text.slice(at, at + len));
            at += len;
        }
        let out = `${keys.length}\n`;
        for (const k of keys) {
            const value = params.get(k) ?? '0';
            out += `${value.length}\n${value}`;
        }
        return out;
    },
    host_module_set_param(key, value) {
        writes.push({ key, value: String(value) });
        params.set(key, String(value));
    }
});

function synthetic(exports) {
    return new vm.SyntheticModule(Object.keys(exports), function initialize() {
        for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, { context });
}

const modules = new Map([
    ['/data/UserData/schwung/shared/constants.mjs', synthetic(constants)],
    ['/data/UserData/schwung/shared/input_filter.mjs', synthetic({
        /* Transcribed from schwung's src/shared/input_filter.mjs. decodeDelta
         * returns the ACCUMULATED tick count, which is the whole reason the
         * knob maths needs a cap — do not simplify it here. */
        decodeDelta(v) {
            if (v === 0) return 0;
            if (v >= 1 && v <= 63) return v;
            if (v >= 65 && v <= 127) return -(128 - v);
            return 0;
        },
        decodeAcceleratedDelta(v) {
            if (v === 0) return 0;
            if (v >= 1 && v <= 63) return v;
            if (v >= 65 && v <= 127) return -(128 - v);
            return 0;
        },
        setLED() {}
    })],
    ['/data/UserData/schwung/shared/menu_layout.mjs', synthetic({
        drawMenuHeader() {}, drawMenuFooter() {}
    })],
    ['/data/UserData/schwung/shared/screen_reader.mjs', synthetic({
        announce(t) { announcements.push(String(t)); },
        announceParameter(l, v) { announcements.push(`${l} ${v}`); },
        announceView(t) { announcements.push(String(t)); }
    })]
]);

const module_ = new vm.SourceTextModule(source, { context, identifier: 'ui_overtake.js' });
await module_.link((specifier) => {
    const found = modules.get(specifier);
    assert(found, `unexpected import: ${specifier}`);
    return found;
});
await module_.evaluate();

const ui = context;
const cc = (num, value) => ui.onMidiMessageInternal([0xb0, num, value]);
const settle = (n = 20) => { for (let i = 0; i < n; i++) ui.tick(); };

/* ------------------------------------------------------------------ tests */

ui.init();
settle(60);                                    /* let LED init and first fetch finish */

/* The headline measurement. */
roundTrips = 0;
settle(44);                                    /* one second of ticks */
assert(roundTrips <= 40,
    `steady state costs ${roundTrips} blocking round-trips per second against a ` +
    'channel that serves about 44 — at that rate the UI starves itself and reads ' +
    'start timing out');

/* A knob turn must not stall behind a refresh. */
roundTrips = 0;
cc(MoveKnob1, 1);
assert(roundTrips <= 1,
    `a knob detent cost ${roundTrips} blocking round-trips on the input path`);

/* decodeDelta is accumulated: one brisk turn arrives as a single event
 * carrying 20+. Track level is 0-200 in steps of 5, so a fast spin should
 * cover a useful chunk without slamming to an end. */
params.set('t1_level', '100');
ui.init();
settle(40);
writes.length = 0;
cc(MoveKnob1, 1);
let w = writes.find(x => x.key === 't1_level');
assert.equal(w?.value, '105', `one detent moved t1_level to ${w?.value}, expected 105`);

params.set('t1_level', '100');
ui.init();
settle(40);
writes.length = 0;
cc(MoveKnob1, 40);
w = writes.find(x => x.key === 't1_level');
assert(w && Number(w.value) > 105 && Number(w.value) < 200,
    `a fast spin drove t1_level to ${w?.value}; it must move a useful chunk but ` +
    'not straight to the end stop');

/* A dead param channel must not rewrite the engine with defaults. */
params.set('t1_level', '150');
params.set('master', '90');
ui.init();
settle(40);
readFailures = 800;
settle(60);
readFailures = 0;
assert.equal(params.get('t1_level'), '150',
    'a timed-out read must leave the DSP alone, not write a default back');
assert.equal(params.get('master'), '90',
    'a timed-out read must leave the DSP alone, not write a default back');

/* ...and the next knob turn must continue from the real value, not a zeroed
 * mirror. This is the silent patch-corruption path. */
writes.length = 0;
cc(MoveKnob1, 1);
w = writes.find(x => x.key === 't1_level');
assert.equal(w?.value, '155',
    `after a dead param channel the UI wrote t1_level=${w?.value}; it must continue ` +
    'from 150, not from a zeroed mirror');

console.log('mark overtake UI: param-channel and knob-response tests passed');
