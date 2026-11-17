'use strict';
// #20: plate slot 1 is the OS's glass, painted above every row an app has (orb_stage.c:
// pick_ord 0xFFFF, CLK_ROLE_GLASS, 0x10000). The kit refuses it through every row door but
// glass(), so no app can put its art there and bury its own words (Pomodoro's split-flaps
// did, and every byte check passed). glass() must still bake what the doors used to.
//
//   node headless-glass.test.js               the checks
//   node headless-glass.test.js --mutations   does each check bite?
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

if (process.argv.includes('--mutations')) {
  const SRC = path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'), src = fs.readFileSync(SRC, 'utf8');
  const M = [
    ['the glass guard is gone', "if ((e.slot | 0) === 1 && !e[GLASS])", 'if (false)'],
    ['the guard checks every class', "if ((e['class'] | 0) !== CLASS_PLATE) return;", ''],
    ['row skips the guard', 'function row(e, plane, masked, dith) {\n      guardSlot(e);', 'function row(e, plane, masked, dith) {'],
    ['rowFrames skips the guard', 'function rowFrames(e, planes, masked, dith) {\n      guardSlot(e);', 'function rowFrames(e, planes, masked, dith) {'],
    ['rowPick skips the guard', 'function rowPick(e, planes, masked, dith) {\n      guardSlot(e);', 'function rowPick(e, planes, masked, dith) {'],
    ['element skips the guard', 'function element(e) {\n      guardSlot(e);', 'function element(e) {'],
    ['the glass mark rides into the table', 'e = own(e);', ''],
    ['plates above 1 pass the bake', 'if ((e.slot | 0) > 1)', 'if (false)'],
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'glassmut.'));
  let caught = 0;
  for (const [name, a, b] of M) {
    if (!src.includes(a)) { console.log('STALE  ' + name); continue; }
    const f = path.join(dir, 'b.js'); fs.writeFileSync(f, src.replace(a, b));
    const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { GLASS_BAKER: f }), encoding: 'utf8' });
    if (r.status !== 0) caught++;
    console.log((r.status !== 0 ? 'caught ' : 'SURVIVED ') + name);
  }
  console.log(caught === M.length ? 'all ' + M.length + ' mutants caught' : (M.length - caught) + ' of ' + M.length + ' mutants survived');
  process.exit(caught === M.length ? 0 : 1);
}

const G = require(process.env.GLASS_BAKER || path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
let fails = 0, checks = 0;
function check(ok, what) { checks++; if (!ok) { fails++; console.log('FAIL ' + what); } }
function refused(fn) { try { fn(); return false; } catch (e) { return /OS glass/.test(String(e.message)); } }

const PLATE = G.classId('plate'), AUX = G.classId('aux');
function plane(w, h) { return { w: w, h: h, rgba: new Uint8ClampedArray(w * h * 4).fill(200) }; }
const P = plane(16, 16);
const at1 = { class: PLATE, slot: 1, x: 0, y: 0, w: 16, h: 16, order: 0, alpha: 255, n_frames: 1 };

let St = G.stage3({ scr_w: 64, scr_h: 64 });
check(refused(() => St.row(Object.assign({ n: St.id() }, at1), P, true, true)), 'row refuses a plate at slot 1');
check(refused(() => St.rowFrames(Object.assign({ n: St.id() }, at1), [P], true, true)), 'rowFrames refuses a plate at slot 1');
check(refused(() => St.rowPick(Object.assign({ n: St.id() }, at1, { n_frames: 1 }), [P], true, true)), 'rowPick refuses a plate at slot 1');
check(refused(() => St.pool({ class: PLATE, x: 0, y: 0, w: 16, h: 16, order: 0, alpha: 255 }, [P], [{ slot: 1 }], true, true)), 'pool refuses a plate row at slot 1');
check(refused(() => St.element(Object.assign({ n: St.id() }, at1))), 'element refuses a plate at slot 1');

// a plate above slot 1: the orb refuses it at load, so the kit refuses it at the bake
function refusedPlate(fn) { try { fn(); return false; } catch (e) { return /two plates/.test(String(e.message)); } }
check(refusedPlate(() => St.row({ n: St.id(), class: PLATE, slot: 2, x: 0, y: 0, w: 16, h: 16, order: 0, alpha: 255, n_frames: 1 }, P, false, true)), 'row refuses a plate at slot 2');
check(refusedPlate(() => St.element({ n: St.id(), class: PLATE, slot: 5, x: 0, y: 0, w: 16, h: 16, order: 0 })), 'element refuses a plate at slot 5');

// what stays allowed: the app's own plate at 0, and anything else at slot 1
check(!refused(() => St.row({ n: St.id(), class: PLATE, slot: 0, x: 0, y: 0, w: 16, h: 16, order: 0, alpha: 255, n_frames: 1 }, P, false, true)), 'plate 0 is the app’s own');
check(!refused(() => St.row({ n: St.id(), class: AUX, slot: 1, x: 0, y: 0, w: 16, h: 16, order: 2, alpha: 255, n_frames: 1 }, P, true, true)), 'an aux row may take slot 1: only the plate is the glass');

// glass() takes slot 1 and leaves no mark in the table
const before = St.elements.length;
const rec = St.glass({ n: St.id(), x: 0, y: 0, w: 16, h: 16 }, P, true);
check(St.elements.length === before + 1 && (rec['class'] | 0) === PLATE && (rec.slot | 0) === 1, 'glass() lays a plate at slot 1');
check(!Object.keys(rec).some((k) => k.indexOf('glass') >= 0), 'the glass mark stays out of the element record');

// glass() bakes what a slot-1 row baked before the guard: same art, same record
const A = G.stage3({ scr_w: 64, scr_h: 64 }), Bk = G.stage3({ scr_w: 64, scr_h: 64 });
A.glass({ n: A.id(), x: 2, y: 3, w: 16, h: 16, order: 5, alpha: 200 }, P, true);
const legacy = Object.assign({ n: Bk.id() }, at1, { x: 2, y: 3, order: 5, alpha: 200 });
legacy.__glass = true;                                   // the one way past the guard, as glass() takes it
Bk.row(legacy, P, true, true);
check(Buffer.compare(Buffer.from(A.bank.art), Buffer.from(Bk.bank.art)) === 0, 'glass() bakes the same art as the row it replaces');
check(JSON.stringify(A.elements[0]) === JSON.stringify(Bk.elements[0]), 'glass() writes the same record as the row it replaces');

console.log(fails ? fails + ' of ' + checks + ' checks failed' : 'all green (' + checks + ' checks)');
process.exit(fails ? 1 : 0);
