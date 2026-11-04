// Where the OS theme runtime lives (D43 §9.3). The clock's editor is an app; the baker
// and the dc-runtime it builds against are the hub's, served at /runtime/<id>/ and kept
// under tools/orb-manager/web/runtime/. The node suites load them from there.
'use strict';
const path = require('path');
const ROOT = path.resolve(__dirname, '..', '..', '..', '..');
const RUNTIME = path.join(ROOT, 'tools', 'orb-manager', 'web', 'runtime');
module.exports = {
  ROOT,
  RUNTIME,
  BAKER: path.join(RUNTIME, 'baker@1', 'orbtheme-baker.js'),
  SUPPORT: path.join(RUNTIME, 'support@1', 'support.js'),
  SEND: path.join(RUNTIME, 'theme@1', 'send.js'),
  SESSION: path.join(RUNTIME, 'theme@1', 'session.js'),
};
