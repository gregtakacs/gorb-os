#!/usr/bin/env node
// Headless selftest runner: no browser, no display, no device. Runs each suite in its
// own process (they install global stubs) and reports one verdict.
//
//   node headless-test.js              all suites
//   node headless-test.js ui           only the suite whose name contains 'ui'
//
// Node is not on the dev box by default. Bootstrap once with:
//   python3 -m pip install --user --break-system-packages nodeenv
//   python3 -m nodeenv --node=lts /tmp/nodeenv
//   /tmp/nodeenv/bin/node headless-test.js
'use strict';
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const filter = process.argv[2];
const suites = fs.readdirSync(__dirname).filter((f) => /^headless-.*\.test\.js$/.test(f) && (!filter || f.includes(filter))).sort();
if (!suites.length) { console.log('no suite matches ' + filter); process.exit(2); }
let bad = 0;
for (const s of suites) {
  const t0 = Date.now();
  const r = spawnSync(process.execPath, [path.join(__dirname, s)], { encoding: 'utf8' });
  const out = (r.stdout || '').trim().split('\n');
  const tail = out[out.length - 1] || '(no output)';
  const fails = (r.stdout || '').split('\n').filter((l) => l.startsWith('FAIL '));
  const pass = r.status === 0 && !fails.length;
  if (!pass) bad++;
  console.log((pass ? 'ok   ' : 'BAD  ') + s.padEnd(30) + tail + '  ' + (Date.now() - t0) + 'ms');
  fails.forEach((l) => console.log('       ' + l));
  if (!pass && r.stderr) console.log(r.stderr.split('\n').slice(0, 8).map((l) => '       | ' + l).join('\n'));
}
console.log(bad ? bad + ' suite(s) FAILED' : 'all ' + suites.length + ' suites green');
process.exit(bad ? 1 : 0);
