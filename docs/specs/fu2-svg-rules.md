# fu2-1: SVG art is checked by parsing and an allow-list, not by text scanning

Ruling 35 (orb-os-net docs/design.md §0, as amended 2026-11-11) keeps SVG art byte for byte. The
three checkers that enforce it — Go `checkSvgArt` (tools/orb-manager/sitelimits.go), JS
`OrbStudio.checkSvg` (tools/orb-manager/web/runtime/studio@1/studio.js) and TS
(orb-os-net src/lib/svg-art.ts) — are substring/regex scanners today, and an independent review
showed that markup they don't model can still make a browser load outside resources when the
SVG is opened directly or inlined into a page. The fix replaces scanning with one rule set
applied to a real parse, identical in all three. The review's inputs and the Chromium harness
are in `/tmp/claude-1000/-home-greg-docker-Projects/5278e4ea-1e48-4d88-90a5-999cf1ef2d25/scratchpad/svgreview/`
(vectors.json, chrome/) and `.../scratchpad/svgprobe/`; every one of them must be refused after
this step.

## Read first
- orb-os-net docs/design.md §0 ruling 35 (art rules) and this file.
- The three current checkers and their tests: tools/orb-manager/sitelimits_test.go (`svgVectors`),
  tools/orb-manager/web/runtime/_selftest/headless-studio.test.js, orb-os-net tests/svg-art.test.ts.

## The rule set (all three implementations, same order, same rule ids)

Checked in this order; the first failure is reported with its rule id.

1. `svg_too_big` — more than 256 KB (262,144 bytes). Checked before anything is parsed.
2. `svg_not_utf8` — not valid UTF-8, or contains NUL. A leading BOM is allowed. A literal
   U+FFFD in valid UTF-8 is allowed (JS: read the file as bytes and decode with
   `TextDecoder("utf-8", {fatal: true})`, not FileReader.readAsText).
3. `svg_not_well_formed` — not well-formed XML 1.0 under a strict, namespace-aware parser.
   In particular: unquoted attribute values, undefined entities, `--` inside a comment,
   mismatched tags are all refusals. No DTD is ever loaded.
4. `svg_markup` — any of: a DOCTYPE or other `<!` declaration; a CDATA section; a processing
   instruction other than one leading `<?xml …?>` declaration; a comment containing `<`, `>`
   or `--!`.
5. `svg_root` — the first element is not `svg` in the SVG namespace
   (`http://www.w3.org/2000/svg`).
6. `svg_element` — an element in the SVG namespace that is not on the allowed list:
   `svg g defs symbol use path rect circle ellipse line polyline polygon text tspan textPath
   title desc metadata linearGradient radialGradient stop pattern clipPath mask marker image
   filter feBlend feColorMatrix feComponentTransfer feComposite feDiffuseLighting
   feDisplacementMap feDistantLight feDropShadow feFlood feFuncA feFuncB feFuncG feFuncR
   feGaussianBlur feMerge feMergeNode feMorphology feOffset fePointLight feSpecularLighting
   feSpotLight feTile feTurbulence switch view`.
   Everything else is refused, notably: script, style, foreignObject, every animation element
   (animate, animateColor, animateMotion, animateTransform, set, discard, mpath), feImage,
   a, iframe, embed, object, video, audio, handler, listener, cursor, font and font-face*.
   Art is baked to pixels for a 466 × 466 display, so animation and styling sheets have no use.
7. `svg_foreign_element` — an element in any other namespace, except elements in the editor
   namespaces Inkscape and Sodipodi write (`http://www.inkscape.org/namespaces/inkscape`,
   `http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd`) and, only inside `metadata`, RDF / CC /
   Dublin Core (`http://www.w3.org/1999/02/22-rdf-syntax-ns#`, `http://creativecommons.org/ns#`,
   `http://web.resource.org/cc/`, `http://purl.org/dc/elements/1.1/`). Even then, an element
   whose local name (case-insensitively) is any SVG or HTML element name is refused, because
   an HTML parser inlining the SVG ignores prefixes.
8. `svg_event_attr` — any attribute, in any namespace, whose local name starts with `on`
   (case-insensitive).
9. `svg_link` — `href` in no namespace or the XLink namespace, on any element: after the
   parser's entity decoding and trimming ASCII whitespace, the value must be `#` followed by an
   XML name (same-document reference), or `data:image/(png|jpeg|gif|webp);base64,` (prefix
   case-insensitive) followed by only base64 characters. Anything else is refused. Also
   refused: `xml:base` anywhere; any other attribute whose decoded value contains `url(` with a
   target other than `#name`; any attribute value containing `javascript:` or `data:` other than
   in an allowed `href`.
10. `svg_css` — the `style` attribute (no `<style>` element is allowed at all, rule 6): its
    decoded value must not contain `\`, `/*`, `@`, `<`, `image-set`, `expression`, or a `url(`
    whose target, after optional quotes, is anything but `#name`.

## Each surface keeps its wording

Each implementation keeps its own refusal sentences (site: "an SVG in that design …", hub and
editor: "an SVG in it …") but every refusal carries its rule id (Go: a typed error with `Rule`;
JS: `{ok:false, rule, message}`; TS: an error class with `.rule`). Tests assert rule ids, not
sentences.

## Implementation notes
- Go: `encoding/xml` Decoder with `Strict = true`, `AutoClose = nil`, `Entity = nil`; refuse on
  `xml.Directive` and `xml.ProcInst` tokens per rule 4; walk `StartElement` names/attrs.
- JS (browser): pre-scan for `<!` declarations other than comments (rule 4) before parsing, then
  `new DOMParser().parseFromString(text, "application/xml")`; a `parsererror` element anywhere
  means rule 3; walk the DOM (Element, Comment, CDATASection, ProcessingInstruction nodes).
- TS (site, Node): add the `saxes` package (strict, namespace-aware SAX parser, no deps) and
  walk its events. No other new dependency.
- Keep the 256 KB check first in all three; no regex over the whole document is needed after
  parsing except the rule-4 pre-scan.

## Shared vectors
- One vector file, `tools/orb-manager/testdata/svg-vectors.json` (gorb-os is the source): a list
  of `{name, svg, rule}` where `rule` is `null` for an accepted input. Include every input from
  the review's vectors.json and the svgprobe set, the existing `svgVectors`, and accepted
  real-world shapes: a plain Inkscape save (with sodipodi:namedview and metadata/rdf), an
  Illustrator-style export with `xmlns:xlink` and `xlink:href="#id"`, gradients via
  `fill="url(#g)"`, an embedded PNG via `href="data:image/png;base64,…"`, a BOM-prefixed file.
- Go tests, `_selftest/headless-studio.test.js`, and the site's tests/svg-art.test.ts all load
  that file (the site keeps a byte-identical copy at tests/fixtures/svg-vectors.json and a test
  that its sha256 matches the copy in this step's commit message).
- Wire `_selftest/headless-studio.test.js` into `make check-editor` (it is not run by any target
  or CI job today).

## Acceptance
- `go test ./tools/orb-manager/...` and `make check-editor check-web check-hub` pass in the
  gorb2 image; the site's `npx vitest run tests/svg-art.test.ts` and full suite pass.
- Every vector's outcome is identical in Go, JS and TS (a test per surface iterates the file).
- The Chromium harness in scratchpad/svgprobe (beacon server + drive.sh) shows no request to the
  beacon for any vector the checkers accept, in all three contexts: `<img>`, opened directly as
  image/svg+xml, inlined into an HTML page.
- The shelf clock and flights pickers still accept a plain Inkscape SVG and a gradient SVG
  (check-pictures).

## Out of scope
Sanitising (rewriting) an SVG; changing the 256 KB cap; raster rules.
