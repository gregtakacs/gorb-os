#!/usr/bin/env node
'use strict';
// Every OS editor service the runtime serves (web/runtime/theme@1/) reaches every place the
// clock editor runs: Orb Manager's page (host@1/page.html, which hosts the clock since E7
// Stage 2), the flat test folder the deployer writes, and every browser test page that mounts
// the class (clock-logic.js). K1–K3 added services without the pages, which left those pages
// mounting a component that calls OrbTheme with no OrbTheme loaded (theme-editor-kit-plan K4).
const fs = require('fs');
const path = require('path');
const { RUNTIME } = require('./runtime-paths.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const svc = fs.readdirSync(path.join(RUNTIME, 'theme@1')).filter((f) => f.endsWith('.js')).sort();
ok(svc.join() === 'measure.js,preview.js,send.js,session.js', 'the runtime serves the four editor services', svc.join());
const doc = fs.readFileSync(path.join(RUNTIME, 'host@1', 'page.html'), 'utf8');
svc.forEach((f) => ok(doc.includes('<script src="/runtime/theme@1/' + f + '"></script>'), 'Orb Manager’s editor page loads ' + f));
const dep = fs.readFileSync(path.join(__dirname, 'deploy_test_folder.py'), 'utf8');
svc.forEach((f) => ok(dep.includes('(os.path.join(RUNTIME, "theme@1", "' + f + '"), "' + f + '")'), 'the deployer copies ' + f + ' into the test folder'));
const pages = fs.readdirSync(__dirname).filter((f) => f.endsWith('.html') && fs.readFileSync(path.join(__dirname, f), 'utf8').includes('src="../clock-logic.js"'));
ok(pages.length >= 9, 'the browser pages that mount the class', pages.join(', '));
pages.forEach((p) => {
  const s = fs.readFileSync(path.join(__dirname, p), 'utf8');
  const miss = svc.filter((f) => !s.includes('<script src="../' + f + '"></script>'));
  ok(!miss.length && s.indexOf('../send.js') < s.indexOf('src="../clock-logic.js"'), p + ' loads every service before the class', miss.join());
});

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
