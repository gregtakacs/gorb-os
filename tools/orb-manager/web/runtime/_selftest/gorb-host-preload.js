// Exists so an editor suite can run under an explicit host description without the page:
// run it with NODE_OPTIONS=--require=<this file> and GORB_HOST=<json>. Sets globalThis.GorbHost
// from GORB_HOST when that env var is present; otherwise it does nothing.
'use strict';
if (process.env.GORB_HOST) globalThis.GorbHost = JSON.parse(process.env.GORB_HOST);
