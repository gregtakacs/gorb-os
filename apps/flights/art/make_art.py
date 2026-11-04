#!/usr/bin/env python3
"""The tracker's theme art (docs/flight-tracker-design.md §11.3), drawn as code.

    python3 apps/flights/art/make_art.py

Writes every SVG beside this file, art.json (each asset's size and pivot) and
review.html (the scope preview, each plane at blip size in every altitude band).
The geometry lives here, not in the SVGs: edit this file and rerun.

Planes are drawn nose up on a 100 x 100 box, pivot at the centre, out of named
parts (body, tail, engines, props, canopy, markings). A shape set is the
geometry; a family is a shape set plus a renderer, and the renderer decides
which parts take the altitude band's colour and which keep their material.
Altitude colour is baked, one copy per band (§11.1 ruling 4), so the band
colour is just one more input to a renderer.
"""
import json, math, os

HERE = os.path.dirname(os.path.abspath(__file__))
GLASS, CX, RING_R = 466, 233, 184           # apps/flights/main.c: W, CX, RING_R
# Legibility, judged at true size (owner, 2026-11-03): the panel is 1.75 in across, ~266 ppi.
# Planes read best at 3.5-4.0 mm (37-42 px); 14 px text, the footer's, is the smallest
# anything may be, so baked numerals sit just above it.
PLANE_PX, NUMERAL_PX = 38, 15

OUTLINE, ACCENT = '#0B1016', '#6E7B88'
OUTLINE_W = 8

# Altitude bands, as apps/flights/main.c alt_rgb() has them.
BANDS = [('ground', '#888888'), ('no alt', '#C0C0C0'), ('< 3k ft', '#FF5A3C'),
         ('< 10k', '#FFB23C'), ('< 20k', '#C8FF3C'), ('< 30k', '#39FF14'), ('30k +', '#3CE0FF')]

# Metal fills, in the plane's own 100 x 100 space so the light falls across the
# whole aircraft rather than each part separately.
GRADIENTS = {
    'g-silver': [(0, '#FAFBFC'), (.45, '#C3CAD2'), (.55, '#DCE1E6'), (1, '#86909A')],
    'g-brass': [(0, '#F6DC8E'), (.45, '#C2922F'), (.6, '#E3BC5E'), (1, '#7C561A')],
}


def gradient_defs(ids):
    return ''.join(
        f'<linearGradient id="{i}" gradientUnits="userSpaceOnUse" x1="20" y1="0" x2="80" y2="100">'
        + ''.join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in GRADIENTS[i]) + '</linearGradient>'
        for i in ids)


def rect(x, y, w, h, r=0, rot=None):
    t = f' transform="rotate({rot[0]} {rot[1]} {rot[2]})"' if rot else ''
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}"{t}/>'


def path(d):
    return f'<path d="{d}"/>'


def circle(cx, cy, r):
    return f'<circle cx="{cx}" cy="{cy}" r="{r}"/>'


def ellipse(cx, cy, rx, ry):
    return f'<ellipse cx="{cx}" cy="{cy}" rx="{rx}" ry="{ry}"/>'


def mirror(d):
    """Mirror an absolute M/L/Q/C/Z path about x = 50 (the wing on the other side).
    Every argument of those commands is an x, y pair, so pairs flip one at a time."""
    for c in 'MLQCZ':
        d = d.replace(c, f' {c} ')
    out, toks, i = [], d.split(), 0
    while i < len(toks):
        if toks[i] in 'MLQCZ':
            out.append(toks[i]); i += 1
        else:
            out.append(f'{100 - float(toks[i]):g} {toks[i + 1]}'); i += 2
    return path(' '.join(out))


def pair(d):
    return [path(d), mirror(d)]


def cockpit(y):
    return path(f'M46.8 {y + 2.5} Q50 {y} 53.2 {y + 2.5} L52.6 {y + 5.5} L47.4 {y + 5.5} Z')


def shape(what, body, tail=(), engines=(), props=(), canopy=(), mark=(), lines=(), hi=(), discs=()):
    """One plane. body/tail/engines/props make the silhouette. The rest is detail on
    top: canopy (dark glass, intakes, spinners), lines (flaps, elevators, cowls), hi
    (a lit edge), discs (spinning props), mark (markings). Only the detailed single
    planes draw detail; a plane per category keeps to shape (ruling 6)."""
    return {'what': what, 'lines': list(lines), 'hi': list(hi), 'discs': list(discs),
            'body': list(body), 'tail': list(tail), 'engines': list(engines),
            'props': list(props), 'canopy': list(canopy), 'mark': list(mark)}


MODERN = {
    'light': shape('Light single (A1): high wing, nose prop',
        [path('M50 11C54 11 56 14 56 20L56 48L53 87L47 87L44 48L44 20C44 14 46 11 50 11Z'), rect(6, 29, 88, 13, 3)],
        tail=[rect(34, 77, 32, 9, 2.5)],
        props=[rect(38, 7, 24, 3.5, 1.7), circle(50, 11, 3.5)],
        canopy=[rect(46, 23, 8, 6, 2)]),
    'turboprop': shape('Turboprop (A2): straight wing, two props',
        [path('M50 7C54 7 56 12 56 20L56 80L53 93L47 93L44 80L44 20C44 12 46 7 50 7Z'), *pair('M56 35L96 37L96 46L56 48Z')],
        tail=pair('M54 81L70 84L70 90L54 90Z'),
        engines=[rect(64, 26, 8, 24, 4), rect(28, 26, 8, 24, 4)],
        props=[rect(56, 24, 24, 3.5, 1.7), rect(20, 24, 24, 3.5, 1.7)],
        canopy=[cockpit(10)]),
    'bizjet': shape('Business jet (A2/A3): tail engines, T-tail',
        [path('M50 9C54 9 56 14 56 22L56 80L53 91L47 91L44 80L44 22C44 14 46 9 50 9Z'), *pair('M56 44L88 58L88 64L56 60Z')],
        tail=pair('M53 82L67 88L67 92L53 91Z'),
        engines=[rect(56, 62, 9, 16, 4.5), rect(35, 62, 9, 16, 4.5)],
        canopy=[cockpit(13)]),
    'airliner': shape('Airliner (A3): swept wing, two engines',
        [path('M50 4C55 4 57 10 57 18L57 78L54 95L46 95L43 78L43 18C43 10 45 4 50 4Z'), *pair('M57 38L95 58L95 66L57 58Z')],
        tail=pair('M55 78L72 89L72 95L55 91Z'),
        engines=[rect(65, 37, 8, 14, 2.5), rect(27, 37, 8, 14, 2.5)],
        canopy=[cockpit(8)]),
    'heavy': shape('Heavy (A5): wide body, four engines',
        [path('M50 2C56 2 59 9 59 18L59 78L55 96L45 96L41 78L41 18C41 9 44 2 50 2Z'), *pair('M59 36L97 60L97 68L59 58Z')],
        tail=pair('M57 78L77 90L77 96L57 91Z'),
        engines=[rect(64, 37, 8, 14, 2.5), rect(79, 45, 8, 14, 2.5), rect(28, 37, 8, 14, 2.5), rect(13, 45, 8, 14, 2.5)],
        canopy=[cockpit(6)]),
    'helicopter': shape('Rotorcraft (A7): rotor disc, tail boom',
        [ellipse(50, 40, 13, 17), path('M47 54L53 54L52 87L48 87Z'),
         rect(5, 38, 90, 4, 2, (45, 50, 40)), rect(5, 38, 90, 4, 2, (-45, 50, 40))],
        tail=[rect(40, 74, 20, 5, 2), rect(53, 79, 3.5, 13, 1.5)],
        canopy=[ellipse(50, 30, 7.5, 5), circle(50, 40, 4.5)]),
    'generic': shape('Unknown category: a plain arrowhead',
        [path('M50 10L78 86L50 72L22 86Z')],
        tail=[path('M50 46L62 76L50 70L38 76Z')]),
}

# The vintage set: the same categories, flown by older aircraft.
VINTAGE = {
    'light': shape('Light single (A1): elliptical-wing fighter',
        [path('M50 6C53.5 6 55 10 55 18L55 50L52.5 88L47.5 88L45 50L45 18C45 10 46.5 6 50 6Z'), ellipse(50, 40, 46, 10)],
        tail=[ellipse(50, 82, 16, 5)],
        props=[rect(36, 3.5, 28, 3.5, 1.7), circle(50, 7, 4)],
        canopy=[ellipse(50, 32, 3.2, 6)],
        mark=[circle(24, 40, 5.5), circle(76, 40, 5.5)]),
    'turboprop': shape('Turboprop (A2): twin radial transport',
        [path('M50 6C54.5 6 56 11 56 20L56 76L53 92L47 92L44 76L44 20C44 11 45.5 6 50 6Z'), *pair('M56 34L93 38Q97 39 96 43L56 50Z')],
        tail=pair('M54 80L70 82Q73 83 72 86L54 88Z'),
        engines=[rect(64, 26, 9, 22, 4.5), rect(27, 26, 9, 22, 4.5)],
        props=[rect(58, 22.5, 21, 3, 1.5), rect(21, 22.5, 21, 3, 1.5)],
        canopy=[cockpit(10)],
        mark=pair('M54 80L70 82Q73 83 72 86L54 88Z')),
    'bizjet': shape('Business jet (A2/A3): twin-boom early jet',
        [path('M50 14C54.5 14 56 19 56 26L56 52L53 62L47 62L44 52L44 26C44 19 45.5 14 50 14Z'),
         rect(8, 38, 84, 11, 4), rect(33, 44, 5, 40, 2.5), rect(62, 44, 5, 40, 2.5)],
        tail=[rect(31, 80, 38, 6, 2)],
        canopy=[ellipse(50, 26, 3.5, 5), circle(50, 60, 3)],
        mark=[circle(20, 43.5, 4.5), circle(80, 43.5, 4.5)]),
    'airliner': shape('Airliner (A3): four-engine propliner, triple tail',
        [path('M50 4C54.5 4 56 10 56 20L56 74L53 92L47 92L44 74L44 20C44 10 45.5 4 50 4Z'), *pair('M56 38L95 46L95 51L56 54Z')],
        tail=[*pair('M54 84L72 86L72 91L54 91Z'), rect(70, 82, 4, 11, 1.5), rect(26, 82, 4, 11, 1.5)],
        engines=[rect(62, 30, 7, 16, 3.5), rect(76, 34, 7, 16, 3.5), rect(31, 30, 7, 16, 3.5), rect(17, 34, 7, 16, 3.5)],
        props=[rect(59, 28.5, 13, 2.6, 1.3), rect(73, 32.5, 13, 2.6, 1.3), rect(28, 28.5, 13, 2.6, 1.3), rect(14, 32.5, 13, 2.6, 1.3)],
        canopy=[cockpit(8)],
        mark=[*pair('M54 84L72 86L72 91L54 91Z'), rect(70, 82, 4, 11, 1.5), rect(26, 82, 4, 11, 1.5)]),
    'heavy': shape('Heavy (A5): four-engine bomber',
        [path('M50 3C55.5 3 57 9 57 18L57 80L54 95L46 95L43 80L43 18C43 9 44.5 3 50 3Z'), *pair('M57 36L98 44L98 50L57 56Z')],
        tail=pair('M55 80L76 83Q79 84 78 88L55 91Z'),
        engines=[rect(63, 28, 8, 22, 4), rect(79, 31, 8, 22, 4), rect(29, 28, 8, 22, 4), rect(13, 31, 8, 22, 4)],
        props=[rect(60, 26.5, 14, 2.6, 1.3), rect(76, 29.5, 14, 2.6, 1.3), rect(26, 26.5, 14, 2.6, 1.3), rect(10, 29.5, 14, 2.6, 1.3)],
        canopy=[ellipse(50, 10, 4.5, 5)],
        mark=[circle(88, 46.8, 4), circle(12, 46.8, 4)]),
    'helicopter': shape('Rotorcraft (A7): autogyro, nose prop and free rotor',
        [path('M50 10C54 10 55.5 14 55.5 22L55 60L52 86L48 86L45 60L44.5 22C44.5 14 46 10 50 10Z'), rect(30, 42, 40, 7, 3),
         *[rect(48.5, 4, 3, 40, 1.5, (a, 50, 44)) for a in (0, 120, 240)]],
        tail=[rect(38, 78, 24, 6, 2)],
        props=[rect(38, 6.5, 24, 3, 1.5), circle(50, 10, 3.5)],
        canopy=[ellipse(50, 26, 3, 4.5), circle(50, 44, 4.5)],
        mark=[rect(38, 78, 24, 6, 2)]),
    'generic': shape('Unknown category: a compass pointer',
        [path('M50 8L76 88L50 74L24 88Z')],
        mark=[path('M50 30L58 60L50 55L42 60Z')]),
}


def _sil(p):
    return ''.join(p['body'] + p['tail'] + p['engines'] + p['props'])


def _j(*lists):
    return ''.join(x for l in lists for x in l)


# Two of three (owner, 2026-11-03): at 37-42 px a plane carries shape, colour or detail,
# never all three. Modern and blueprint are shape and colour; silver and brass are shape
# and material, with the band only a hint of tint; paper is colour and folds, one shape.
# A coloured rim round the metal families was tried and rejected: a blur at true size.
TINT = 0.3                                          # how much band colour the metals take


def render_modern(p, band, uid):
    """Shape and colour: a flat silhouette in the band colour, a dark outline, nothing else."""
    s = _sil(p)
    return (f'<g fill="{OUTLINE}" stroke="{OUTLINE}" stroke-width="{OUTLINE_W}" stroke-linejoin="round">{s}</g>'
            f'<g style="fill:{band}">{s}</g>')


def _metal(p, band, grad, ink):
    s = _sil(p)
    tint = f'<g style="fill:{band}" fill-opacity="{TINT}">{s}</g>' if band else ''
    return (f'<g fill="{ink}" stroke="{ink}" stroke-width="{OUTLINE_W}" stroke-linejoin="round">{s}</g>'
            f'<g fill="url(#{grad})">{s}</g>{tint}')


def render_silver(p, band, uid):
    """Shape and material: polished metal with a hint of the band colour."""
    return _metal(p, band, 'g-silver', OUTLINE)


def render_brass(p, band, uid):
    """Shape and material: polished brass with a hint of the band colour."""
    return _metal(p, band, 'g-brass', '#2B1A08')


def render_blueprint(p, band, uid):
    """Shape and colour: the silhouette's outline only, in the band colour, so the
    drawing under it shows through. A mask keeps overlaps (wing over fuselage) out of
    the line."""
    s = _sil(p)
    return (f'<mask id="{uid}" maskUnits="userSpaceOnUse" x="-10" y="-10" width="120" height="120">'
            f'<g fill="#fff" stroke="#fff" stroke-width="9" stroke-linejoin="round">{s}</g><g fill="#000">{s}</g></mask>'
            f'<rect x="-10" y="-10" width="120" height="120" style="fill:{band}" mask="url(#{uid})"/>')


# The paper dart, seen from above: two wings folded down from a centre crease, the keel
# showing between them at the tail. One shape for every category, so it spends its
# budget on folds and light instead.
PAPER_WINGS = (path('M50 4L14 88L47 80Z'), path('M50 4L86 88L53 80Z'))
PAPER_KEEL = path('M50 4L53 80L50 94L47 80Z')
PAPER_DART = shape('One paper dart for every category', [*PAPER_WINGS, PAPER_KEEL])


def render_paper(p, band, uid):
    """Colour and detail: coloured paper lit from the left. A soft shadow under it, the
    far wing and the inner folds in shade, the keel darkest, the near leading edge
    catching the light."""
    s = _sil(p)
    near, far, keel = PAPER_WINGS[0], PAPER_WINGS[1], PAPER_KEEL
    return (f'<filter id="{uid}" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="2"/></filter>'
            f'<g transform="translate(3 4)" fill="#000" fill-opacity="0.3" filter="url(#{uid})">{s}</g>'
            f'<g fill="{OUTLINE}" stroke="{OUTLINE}" stroke-width="5" stroke-linejoin="round">{s}</g>'
            f'<g style="fill:{band}">{s}</g>'
            f'<g fill="#000"><g fill-opacity="0.16">{far}</g><g fill-opacity="0.38">{keel}</g>'
            f'<path d="M50 4L47 80L41 81.5Z" fill-opacity="0.12"/><path d="M50 4L53 80L59 81.5Z" fill-opacity="0.28"/></g>'
            f'<path d="M49.5 6L15.5 86.5" stroke="#fff" stroke-opacity="0.4" stroke-width="1.6" stroke-linecap="round"/>')


# The paper fleet: a different paper plane for each category, drawn flat, so shape
# carries the category and the band carries altitude. One crease, faint.
PAPER_FLEET = {
    'light': shape('Light single (A1): broad glider, blunt nose',
        [path('M50 12L60 22L90 56L88 66L56 60L53 86L47 86L44 60L12 66L10 56L40 22Z')]),
    'turboprop': shape('Turboprop (A2): square-wing plane',
        [path('M50 6L57 38L94 46L94 58L56 60L54 88L46 88L44 60L6 58L6 46L43 38Z')]),
    'bizjet': shape('Business jet (A2/A3): slim swept dart with winglets',
        [path('M50 6L64 58L80 74L80 82L56 74L50 90L44 74L20 82L20 74L36 58Z')]),
    'airliner': shape('Airliner (A3): the classic dart',
        [path('M50 4L86 88L53 80L50 94L47 80L14 88Z')]),
    'heavy': shape('Heavy (A5): wide delta, notched trailing edge',
        [path('M50 6L96 76L80 84L56 76L50 92L44 76L20 84L4 76Z')]),
    'helicopter': shape('Rotorcraft (A7): paper whirligig, blades folded opposite ways',
        [rect(44, 36, 12, 34, 1), rect(56, 36, 36, 11, 1), rect(8, 59, 36, 11, 1), rect(46, 70, 8, 22, 1)]),
    'generic': shape('Unknown category: a folded chevron',
        [path('M50 10L82 72L50 58L18 72Z')]),
}


def render_paper_fleet(p, band, uid):
    """Shape and colour: each category's paper plane, flat, with a faint centre crease."""
    s = _sil(p)
    return (f'<g fill="{OUTLINE}" stroke="{OUTLINE}" stroke-width="6" stroke-linejoin="round">{s}</g>'
            f'<g style="fill:{band}">{s}</g>'
            f'<path d="M50 12L50 86" stroke="#000" stroke-opacity="0.2" stroke-width="1"/>')


# ---- one detailed plane for every category (ruling 10) ----
# A family's single plane gives up shape, so it spends the budget on detail instead:
# a soft shadow, the far half in shade, darker engines, flap and elevator lines,
# dark glass and intakes, a lit edge along the near side of the fuselage.

def _detail(p, band, uid, ink, base, tint):
    s = _sil(p)
    t = f'<g style="fill:{band}" fill-opacity="{TINT}">{s}</g>' if tint and band else ''
    return (f'<filter id="{uid}s" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="2"/></filter>'
            f'<clipPath id="{uid}r"><rect x="50" y="-10" width="60" height="120"/></clipPath>'
            f'<g transform="translate(3 4)" fill="#000" fill-opacity="0.3" filter="url(#{uid}s)">{s}</g>'
            f'<g fill="{ink}" stroke="{ink}" stroke-width="6" stroke-linejoin="round">{s}</g>'
            f'<g {base}>{s}</g>{t}'
            f'<g fill="#000" fill-opacity="0.15" clip-path="url(#{uid}r)">{s}</g>'
            f'<g fill="#000" fill-opacity="0.28">{_j(p["engines"])}</g>'
            f'<g fill="none" stroke="#000" stroke-opacity="0.4" stroke-width="1" stroke-linecap="round">{_j(p["lines"])}</g>'
            f'<g fill="#16202B">{_j(p["canopy"])}</g>'
            f'<g fill="#000" fill-opacity="0.16" stroke="#000" stroke-opacity="0.3" stroke-width="0.8">{_j(p["discs"])}</g>'
            f'<g fill="none" stroke="#fff" stroke-opacity="0.45" stroke-width="1.6" stroke-linecap="round">{_j(p["hi"])}</g>')


def render_detail_modern(p, band, uid):
    return _detail(p, band, uid, OUTLINE, f'style="fill:{band}"', False)


def render_detail_silver(p, band, uid):
    return _detail(p, band, uid, OUTLINE, 'fill="url(#g-silver)"', True)


def render_detail_brass(p, band, uid):
    return _detail(p, band, uid, '#2B1A08', 'fill="url(#g-brass)"', True)


def render_detail_blueprint(p, band, uid):
    """The outline as before, and the detail drawn as the drawing's own lines."""
    return render_blueprint(p, band, uid) + (
        f'<g fill="none" style="stroke:{band}" stroke-width="1.6" stroke-opacity="0.85" stroke-linecap="round">'
        f'{_j(p["engines"], p["lines"], p["canopy"])}</g>'
        f'<line x1="50" y1="12" x2="50" y2="88" style="stroke:{band}" stroke-width="1.2" stroke-dasharray="4 3" stroke-opacity="0.7"/>')


_A, _H, _V = MODERN['airliner'], MODERN['heavy'], VINTAGE['turboprop']
SINGLE_AIRLINER = shape('One airliner for every category: shaded, flaps and elevators lined',
    _A['body'], _A['tail'], _A['engines'],
    canopy=[cockpit(8), ellipse(69, 38.5, 3, 1.4), ellipse(31, 38.5, 3, 1.4)],
    lines=[*pair('M57 55L93 62.5'), *pair('M55 87.5L71 91.5')], hi=[path('M46.2 13L46.2 76')])
SINGLE_HEAVY = shape('One four-engine jet for every category: polished, shaded, flaps and elevators lined',
    _H['body'], _H['tail'], _H['engines'],
    canopy=[cockpit(6), ellipse(68, 38.5, 3, 1.4), ellipse(83, 46.5, 3, 1.4), ellipse(32, 38.5, 3, 1.4), ellipse(17, 46.5, 3, 1.4)],
    lines=[*pair('M59 55L95 64.5'), *pair('M57 88L76 93')], hi=[path('M44.2 12L44.2 78')])
SINGLE_RADIAL = shape('One twin radial transport for every category: polished, props spinning, cowls ringed',
    _V['body'], _V['tail'], _V['engines'],
    canopy=[cockpit(10), circle(68.5, 24, 2.2), circle(31.5, 24, 2.2)],
    lines=[*pair('M56 47L92 41.5'), *pair('M54 85.5L70 84'), ellipse(68.5, 29, 4.5, 2.5), ellipse(31.5, 29, 4.5, 2.5)],
    discs=[circle(68.5, 24, 11), circle(31.5, 24, 11)], hi=[path('M45.3 14L45.3 74')])


# name -> (title, what carries it, shape set, renderer, band a standalone file shows
# (None: untinted material), gradients)
FAMILIES = {
    'modern': ('Modern', 'Shape and colour: a flat silhouette in the band colour.',
               MODERN, render_modern, '#EEF2F5', []),
    'silver': ('Modern silver', 'Shape and material: polished metal with a hint of the band colour.',
               MODERN, render_silver, None, ['g-silver']),
    'brass': ('Vintage brass', 'Shape and material: older aircraft in polished brass, with a hint of the band colour.',
              VINTAGE, render_brass, None, ['g-brass']),
    'blueprint': ('Blueprint', 'Shape and colour: a wireframe outline in the band colour; the grid shows through.',
                  MODERN, render_blueprint, '#EAF3FF', []),
    'paper': ('Paper', 'Shape and colour: a different flat paper plane for each category.',
              PAPER_FLEET, render_paper_fleet, '#F2F0EA', []),
}

# family -> (what carries it, the one plane, renderer): the family's second form
SINGLES = {
    'modern': ('Colour and detail: one shaded airliner for every category, in the band colour.',
               SINGLE_AIRLINER, render_detail_modern),
    'silver': ('Material and detail: one polished four-engine jet for every category, with a hint of the band colour.',
               SINGLE_HEAVY, render_detail_silver),
    'brass': ('Material and detail: one brass twin radial transport for every category, props spinning, with a hint of the band colour.',
              SINGLE_RADIAL, render_detail_brass),
    'blueprint': ('Colour and detail: one airliner drawn as a full technical outline in the band colour.',
                  SINGLE_AIRLINER, render_detail_blueprint),
    'paper': ('Colour and detail: one shaded paper dart for every category, folded from paper in the band colour.',
              PAPER_DART, render_paper),
}


def _pick(fam, key):
    """key is a category, or 'single' for the family's one detailed plane."""
    if key == 'single':
        return SINGLES[fam][1], SINGLES[fam][2]
    return FAMILIES[fam][2][key], FAMILIES[fam][3]


def keys(fam):
    return [*FAMILIES[fam][2], 'single']


def plane_svg(fam, key):
    p, fn = _pick(fam, key)
    _, _, _, _, band, grads = FAMILIES[fam]
    defs = f'<defs>{gradient_defs(grads)}</defs>' if grads else ''
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">'
            f'{defs}{fn(p, band, "m")}</svg>')


def plane_symbol(fam, key):
    """The same plane for the review page, its band colour read from --b."""
    p, fn = _pick(fam, key)
    return f'<symbol id="p-{fam}-{key}" viewBox="0 0 100 100">{fn(p, "var(--b)", f"m-{fam}-{key}")}</symbol>'


def pt(r, deg):
    a = math.radians(deg)
    return r * math.sin(a), -r * math.cos(a)


# ---- radar arms: pivot at (0, 0), pointing up (bearing 0), turning clockwise ----

def arm_wedge(L=RING_R, trail=40, colour='#3DFF8E', lead='#D8FFE8', peak=0.42):
    """Phosphor wedge: a bright lead line and a fading trail behind it. The trail is
    baked (owner, 2026-11-03), so its length, spread and colours are bake parameters
    the editor sets, not runtime ones."""
    assert 5 <= trail <= 90, 'the box below assumes the trail stays in one quadrant'
    n = max(8, round(trail * 0.8))                   # slices: smooth at any spread
    slices = []
    for i in range(n):
        a0, a1 = -trail * i / n, -trail * (i + 1) / n
        x0, y0 = pt(L, a0); x1, y1 = pt(L, a1)
        op = peak * (1 - i / n) ** 2
        slices.append(f'<path d="M0 0L{x0:.2f} {y0:.2f}A{L} {L} 0 0 0 {x1:.2f} {y1:.2f}Z" '
                      f'fill="{colour}" fill-opacity="{op:.3f}"/>')
    x0 = math.ceil(-L * math.sin(math.radians(trail))) - 4
    vb = (x0, -L - 4, -x0 + 6, L + 10)
    body = (f'<defs><filter id="g" x="-50%" y="-5%" width="200%" height="110%">'
            f'<feGaussianBlur stdDeviation="2.2"/></filter></defs>'
            f'{"".join(slices)}'
            f'<line x1="0" y1="0" x2="0" y2="{-L}" stroke="{colour}" stroke-width="6" filter="url(#g)"/>'
            f'<line x1="0" y1="0" x2="0" y2="{-L}" stroke="{lead}" stroke-width="2"/>')
    return vb, body


def arm_needle():
    """Instrument needle: white blade, orange tip, counterweight tail, a layered arbor."""
    L = RING_R - 4
    body = (f'<path d="M0 {-L}L3.2 -24L4 0L-4 0L-3.2 -24Z" fill="#F1F2EE" stroke="#1A1D21" stroke-width="1.2" stroke-linejoin="round"/>'
            f'<path d="M0 {-L}L1.9 {-L + 34}L-1.9 {-L + 34}Z" fill="#F0662E"/>'
            f'<path d="M-3.6 0L-4.5 26L4.5 26L3.6 0Z" fill="#F1F2EE" stroke="#1A1D21" stroke-width="1.2" stroke-linejoin="round"/>'
            f'<circle cx="0" cy="30" r="7.5" fill="#F1F2EE" stroke="#1A1D21" stroke-width="1.2"/>'
            f'<circle cx="0" cy="0" r="11" fill="#1A1D21"/>'
            f'<circle cx="0" cy="0" r="8.5" fill="#B9BEC4"/>'
            f'<circle cx="0" cy="0" r="5.5" fill="#E4E7EA" stroke="#8E959C" stroke-width="0.8"/>'
            f'<circle cx="0" cy="0" r="1.8" fill="#1A1D21"/>')
    return (-14, -L - 4, 28, L + 4 + 42), body


def arm_pointer():
    """Brass pointer: a tapered blade with an open moon ring near the tip, a ringed
    counterweight and a slotted arbor screw."""
    L, ink = RING_R - 4, '#2B1A08'
    ring = lambda y, ro, ri: (f'M{-ro} {y}a{ro} {ro} 0 1 0 {2 * ro} 0a{ro} {ro} 0 1 0 {-2 * ro} 0Z'
                              f'M{-ri} {y}a{ri} {ri} 0 1 1 {2 * ri} 0a{ri} {ri} 0 1 1 {-2 * ri} 0Z')
    st = f'fill="url(#arm-brass)" stroke="{ink}" stroke-width="1" stroke-linejoin="round"'
    body = (f'<defs><linearGradient id="arm-brass" gradientUnits="userSpaceOnUse" x1="-9" y1="0" x2="9" y2="0">'
            f'<stop offset="0" stop-color="#7C561A"/><stop offset=".35" stop-color="#F6DC8E"/>'
            f'<stop offset=".6" stop-color="#C2922F"/><stop offset="1" stop-color="#6E4C16"/></linearGradient></defs>'
            f'<path d="M-2.6 -142L2.6 -142L4.2 -10L-4.2 -10Z" {st}/>'
            f'<path d="{ring(-150, 8, 4.5)}" fill-rule="evenodd" {st}/>'
            f'<path d="M0 {-L}L3.2 -157L-3.2 -157Z" {st}/>'
            f'<path d="M-3.4 0L-2.6 22L2.6 22L3.4 0Z" {st}/>'
            f'<path d="{ring(28, 7.5, 3)}" fill-rule="evenodd" {st}/>'
            f'<circle cx="0" cy="0" r="11" fill="{ink}"/><circle cx="0" cy="0" r="9" fill="url(#arm-brass)"/>'
            f'<circle cx="0" cy="0" r="5" fill="#8A6420"/>'
            f'<line x1="-4" y1="0" x2="4" y2="0" stroke="{ink}" stroke-width="1.4" transform="rotate(30)"/>')
    return (-14, -L - 4, 28, L + 4 + 40), body


def arm_drafting(colour='#F4F8FF', ground='#1A4479'):
    """Drafting line: a ruled arm with an arrowhead, range ticks along it, and a
    compass-point hub."""
    L = RING_R
    ticks_ = ''.join(f'<line x1="-3" y1="{-y}" x2="3" y2="{-y}" stroke="{colour}" stroke-width="1.4"/>'
                     for y in (46, 92, 138))
    body = (f'<line x1="0" y1="-6" x2="0" y2="{-L + 12}" stroke="{colour}" stroke-width="2.4"/>'
            f'<path d="M0 {-L}L5.5 {-L + 14}L-5.5 {-L + 14}Z" fill="{colour}"/>{ticks_}'
            f'<line x1="0" y1="6" x2="0" y2="14" stroke="{colour}" stroke-width="2"/>'
            f'<line x1="-5" y1="14" x2="5" y2="14" stroke="{colour}" stroke-width="2"/>'
            f'<circle cx="0" cy="0" r="6.5" fill="{ground}" stroke="{colour}" stroke-width="2"/>'
            f'<line x1="-3.5" y1="0" x2="3.5" y2="0" stroke="{colour}" stroke-width="1.2"/>'
            f'<line x1="0" y1="-3.5" x2="0" y2="3.5" stroke="{colour}" stroke-width="1.2"/>')
    return (-9, -L - 4, 18, L + 4 + 20), body


ARMS = {
    'wedge': ('Phosphor wedge', 'Lead line with a glow and a fading trail; blend screen over the scope.', arm_wedge),
    'needle': ('Instrument needle', 'White blade, orange tip, counterweight and arbor; blend normal.', arm_needle),
    'pointer': ('Brass pointer', 'Tapered brass blade, moon ring near the tip, ringed counterweight, slotted arbor.', arm_pointer),
    'drafting': ('Drafting line', 'Ruled white arm with an arrowhead and range ticks; compass-point hub.', arm_drafting),
}


def arm_svg(fn):
    (x, y, w, h), body = fn()
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {w} {h}" width="{w}" height="{h}">'
            f'{body}</svg>'), (-x, -y), (w, h)


# ---- backgrounds: the full glass, centre (233, 233); range rings are drawn live -----

def ticks(r_in, r_long, r_short, step, every, colour, w_long, w_short):
    out = []
    for d in range(0, 360, step):
        long_ = d % every == 0
        x0, y0 = pt(r_in, d); x1, y1 = pt(r_long if long_ else r_short, d)
        out.append(f'<line x1="{CX + x0:.2f}" y1="{CX + y0:.2f}" x2="{CX + x1:.2f}" y2="{CX + y1:.2f}" '
                   f'stroke="{colour}" stroke-width="{w_long if long_ else w_short}"/>')
    return ''.join(out)


def numerals(r, label, family, colour, weight='bold'):
    """Bearing labels every 30 degrees outside the ring, upright, skipping north and
    south: the header sits over north and the info card over south."""
    out = []
    for d in range(30, 360, 30):
        if d == 180:
            continue
        x, y = pt(r, d)
        out.append(f'<text x="{CX + x:.1f}" y="{CX + y:.1f}" text-anchor="middle" dominant-baseline="central" '
                   f'font-family="{family}" font-size="{NUMERAL_PX}" font-weight="{weight}" fill="{colour}">{label(d)}</text>')
    return ''.join(out)


def bg_scope():
    """Phosphor scope: deep green glass, graticule, a bezel tick ring, bearing numerals."""
    g = ['<defs><radialGradient id="glass" cx="50%" cy="50%" r="50%">'
         '<stop offset="0" stop-color="#0E2A1A"/><stop offset="0.78" stop-color="#06160D"/>'
         '<stop offset="1" stop-color="#010503"/></radialGradient></defs>',
         f'<rect width="{GLASS}" height="{GLASS}" fill="#010503"/>',
         f'<circle cx="{CX}" cy="{CX}" r="{CX}" fill="url(#glass)"/>']
    for k in range(-4, 5):                               # graticule squares, 46 px
        if k == 0:
            continue
        o = CX + k * 46
        g.append(f'<line x1="{o}" y1="0" x2="{o}" y2="{GLASS}" stroke="#1C4A2E" stroke-opacity="0.35" stroke-width="0.8"/>')
        g.append(f'<line x1="0" y1="{o}" x2="{GLASS}" y2="{o}" stroke="#1C4A2E" stroke-opacity="0.35" stroke-width="0.8"/>')
    g.append(f'<line x1="{CX}" y1="{CX - RING_R}" x2="{CX}" y2="{CX + RING_R}" stroke="#2E7A4C" stroke-opacity="0.55" stroke-width="1"/>')
    g.append(f'<line x1="{CX - RING_R}" y1="{CX}" x2="{CX + RING_R}" y2="{CX}" stroke="#2E7A4C" stroke-opacity="0.55" stroke-width="1"/>')
    for k in range(-8, 9):                               # cross-hair ticks, 23 px
        if k:
            o = CX + k * 23
            g.append(f'<line x1="{o}" y1="{CX - 3}" x2="{o}" y2="{CX + 3}" stroke="#2E7A4C" stroke-opacity="0.7"/>')
            g.append(f'<line x1="{CX - 3}" y1="{o}" x2="{CX + 3}" y2="{o}" stroke="#2E7A4C" stroke-opacity="0.7"/>')
    g.append(ticks(187, 197, 192, 5, 30, '#3DBB6E', 1.6, 0.9))
    g.append(numerals(212, lambda d: f'{d:03d}', 'DejaVu Sans Mono, monospace', '#3DBB6E'))
    g.append(f'<circle cx="{CX}" cy="{CX}" r="{CX - 2}" fill="none" stroke="#000" stroke-opacity="0.6" stroke-width="5"/>')
    return ''.join(g)


def bg_chart():
    """Aviation chart: paper, a pale lat/long grid, soft terrain, a VOR-style compass
    rose at the range ring. Numerals turn with the rose, as a chart's do."""
    g = [f'<rect width="{GLASS}" height="{GLASS}" fill="#EAE3CF"/>']
    blobs = [((120, 140), 70, 3, 0.4), ((345, 320), 95, 4, 1.7), ((330, 110), 50, 5, 2.9)]
    for (bx, by), r, k, ph in blobs:                     # terrain: wobbly closed curves, two steps
        for step, fill in ((1.0, '#E2D7B8'), (0.6, '#D9CBA4')):
            pts = []
            for i in range(48):
                a = 2 * math.pi * i / 48
                rr = r * step * (1 + 0.18 * math.sin(k * a + ph) + 0.08 * math.sin(2 * k * a + ph * 2))
                pts.append(f'{bx + rr * math.cos(a):.1f},{by + rr * math.sin(a):.1f}')
            g.append(f'<polygon points="{" ".join(pts)}" fill="{fill}"/>')
    for k in range(1, 8):                                # lat/long grid, 58 px
        o = k * 58 + 1
        g.append(f'<line x1="{o}" y1="0" x2="{o}" y2="{GLASS}" stroke="#8FB0C6" stroke-opacity="0.45" stroke-width="0.8"/>')
        g.append(f'<line x1="0" y1="{o}" x2="{GLASS}" y2="{o}" stroke="#8FB0C6" stroke-opacity="0.45" stroke-width="0.8"/>')
    for (x0, y0), (x1, y1) in (((40, 380), (430, 60)), ((60, 90), (420, 410))):   # two airways
        g.append(f'<line x1="{x0}" y1="{y0}" x2="{x1}" y2="{y1}" stroke="#4A78A8" stroke-opacity="0.45" stroke-width="1.6"/>')
    g.append(f'<circle cx="{CX}" cy="{CX}" r="{RING_R}" fill="none" stroke="#2F5C8C" stroke-width="1.2"/>')
    g.append(ticks(RING_R, RING_R - 12, RING_R - 6, 5, 10, '#2F5C8C', 1.4, 0.9))
    for d in range(30, 360, 30):
        if d == 180:                                     # the info card covers south
            continue
        x, y = pt(RING_R - 26, d)
        g.append(f'<text x="{CX + x:.1f}" y="{CX + y:.1f}" transform="rotate({d} {CX + x:.1f} {CX + y:.1f})" '
                 f'text-anchor="middle" dominant-baseline="central" font-family="DejaVu Sans, sans-serif" '
                 f'font-size="{NUMERAL_PX}" font-weight="bold" fill="#2F5C8C">{d // 10}</text>')
    x, y = pt(RING_R - 20, 0)
    g.append(f'<path d="M{CX} {CX + y - 9}L{CX + 6} {CX + y + 5}L{CX} {CX + y + 1}L{CX - 6} {CX + y + 5}Z" fill="#2F5C8C"/>')
    g.append(f'<circle cx="{CX}" cy="{CX}" r="{CX - 2}" fill="none" stroke="#5B5240" stroke-opacity="0.35" stroke-width="4"/>')
    return ''.join(g)


def bg_brass():
    """Brass instrument: a black enamel face in a polished brass bezel with four
    slotted screws, cream engraving, a lubber mark at north."""
    stops = [(0, '#F6DC8E'), (.35, '#9C6B22'), (.55, '#EBC46A'), (.8, '#6E4C16'), (1, '#D9B05A')]
    g = ['<defs><radialGradient id="enamel" cx="50%" cy="45%" r="55%">'
         '<stop offset="0" stop-color="#262A26"/><stop offset="1" stop-color="#0D0E0C"/></radialGradient>'
         f'<linearGradient id="bezel" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{GLASS}" y2="{GLASS}">'
         + ''.join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in stops) + '</linearGradient></defs>',
         f'<rect width="{GLASS}" height="{GLASS}" fill="#0B0A08"/>',
         f'<circle cx="{CX}" cy="{CX}" r="{CX - 1}" fill="url(#bezel)"/>',
         f'<circle cx="{CX}" cy="{CX}" r="216" fill="url(#enamel)" stroke="#2B1A08" stroke-width="2"/>',
         f'<circle cx="{CX}" cy="{CX}" r="211" fill="none" stroke="#C9A24E" stroke-opacity="0.6" stroke-width="1.2"/>',
         ticks(187, 197, 192, 5, 30, '#E9DDBF', 2, 1),
         numerals(205, lambda d: {90: 'E', 270: 'W'}.get(d, str(d)), 'DejaVu Serif, serif', '#E9DDBF'),
         f'<path d="M{CX} {CX - 176}L{CX + 6} {CX - 188}L{CX - 6} {CX - 188}Z" fill="#E8C46A"/>']
    for d in (45, 135, 225, 315):                        # bezel screws
        x, y = pt(224, d)
        g.append(f'<circle cx="{CX + x:.1f}" cy="{CX + y:.1f}" r="4.5" fill="#C9A24E" stroke="#5A3E10" stroke-width="0.8"/>'
                 f'<line x1="{CX + x - 3:.1f}" y1="{CX + y:.1f}" x2="{CX + x + 3:.1f}" y2="{CX + y:.1f}" '
                 f'stroke="#5A3E10" stroke-width="1.2" transform="rotate({d + 20} {CX + x:.1f} {CX + y:.1f})"/>')
    return ''.join(g)


def bg_blueprint():
    """Blueprint: drafting blue, a fine grid with a major line every fifth, dash-dot
    centre lines, a protractor tick ring and one dimensioned radius."""
    step = GLASS / 40
    g = ['<defs><radialGradient id="paper" cx="50%" cy="50%" r="50%">'
         '<stop offset="0" stop-color="#1F5290"/><stop offset="1" stop-color="#163B69"/></radialGradient></defs>',
         f'<rect width="{GLASS}" height="{GLASS}" fill="url(#paper)"/>']
    for k in range(-20, 21):
        o = CX + k * step
        major = k % 5 == 0
        a = f'stroke="#FFFFFF" stroke-opacity="{0.2 if major else 0.07}" stroke-width="{0.9 if major else 0.6}"'
        g.append(f'<line x1="{o:.2f}" y1="0" x2="{o:.2f}" y2="{GLASS}" {a}/><line x1="0" y1="{o:.2f}" x2="{GLASS}" y2="{o:.2f}" {a}/>')
    cl = 'stroke="#DCE9F8" stroke-opacity="0.55" stroke-width="1" stroke-dasharray="14 4 2 4"'
    g.append(f'<line x1="{CX}" y1="{CX - RING_R}" x2="{CX}" y2="{CX + RING_R}" {cl}/>'
             f'<line x1="{CX - RING_R}" y1="{CX}" x2="{CX + RING_R}" y2="{CX}" {cl}/>')
    g.append(ticks(187, 197, 192, 5, 30, '#DCE9F8', 1.6, 0.9))
    g.append(numerals(212, lambda d: f'{d:03d}', 'DejaVu Sans Mono, monospace', '#F4F8FF'))
    ex, ey = pt(RING_R, 225); ux, uy = ex / RING_R, ey / RING_R; nx, ny = -uy, ux
    bx, by = ex - 9 * ux, ey - 9 * uy
    g.append(f'<line x1="{CX}" y1="{CX}" x2="{CX + bx:.1f}" y2="{CX + by:.1f}" stroke="#DCE9F8" stroke-opacity="0.6" stroke-width="1"/>'
             f'<path d="M{CX + ex:.1f} {CX + ey:.1f}L{CX + bx + 3.5 * nx:.1f} {CX + by + 3.5 * ny:.1f}'
             f'L{CX + bx - 3.5 * nx:.1f} {CX + by - 3.5 * ny:.1f}Z" fill="#DCE9F8" fill-opacity="0.6"/>')
    mx, my = pt(112, 225)
    g.append(f'<text x="{CX + mx - 6:.1f}" y="{CX + my - 8:.1f}" text-anchor="end" font-family="DejaVu Sans Mono, monospace" '
             f'font-size="14" fill="#DCE9F8" fill-opacity="0.75">R184</text>')
    g.append(f'<circle cx="{CX}" cy="{CX}" r="{CX - 2}" fill="none" stroke="#DCE9F8" stroke-opacity="0.35" stroke-width="2"/>')
    return ''.join(g)


BACKGROUNDS = {
    'scope': ('Phosphor scope', 'Live rings in dim green; any band colour reads on it.', bg_scope,
              {'ring': '#2A6A44', 'ring_opa': 0.8, 'home': '#FFFFFF', 'blipbg': '#06160D',
               'head': '#39FF14', 'card': '#FFFFFF', 'foot': '#909090', 'sel': '#FFFFFF'}),
    'chart': ('Aviation chart', 'Live rings in chart blue; the dark outline carries planes on paper.', bg_chart,
              {'ring': '#2F5C8C', 'ring_opa': 0.55, 'home': '#2F5C8C', 'blipbg': '#EAE3CF',
               'head': '#1E6B2E', 'card': '#14202C', 'foot': '#5B5240', 'sel': '#14202C'}),
    'brass': ('Brass instrument', 'Black enamel in a brass bezel; cream engraving, brass text.', bg_brass,
              {'ring': '#C9A24E', 'ring_opa': 0.45, 'home': '#E8C46A', 'blipbg': '#161815',
               'head': '#E8C46A', 'card': '#F2E6C8', 'foot': '#A8946A', 'sel': '#E8C46A'}),
    'blueprint': ('Blueprint', 'Drafting blue with a fine grid; live rings and text in white.', bg_blueprint,
                  {'ring': '#DCE9F8', 'ring_opa': 0.45, 'home': '#FFFFFF', 'blipbg': '#1C4A84',
                   'head': '#FFFFFF', 'card': '#FFFFFF', 'foot': '#B8CCE6', 'sel': '#FFFFFF'}),
}

# What each family is shown with first; every control still mixes freely.
PAIRS = {'modern': ('scope', 'wedge'), 'silver': ('scope', 'needle'),
         'brass': ('brass', 'pointer'), 'blueprint': ('blueprint', 'drafting'), 'paper': ('chart', 'needle')}


def bg_svg(fn):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {GLASS} {GLASS}" '
            f'width="{GLASS}" height="{GLASS}">{fn()}</svg>')


def write(rel, text):
    with open(os.path.join(HERE, rel), 'w', encoding='utf-8') as f:
        f.write(text + ('\n' if not text.endswith('\n') else ''))


def main():
    for d in ('planes', 'arms', 'backgrounds'):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
    for f in os.listdir(os.path.join(HERE, 'planes')):     # this script owns planes/: no stale names
        if f.endswith('.svg'):
            os.remove(os.path.join(HERE, 'planes', f))
    manifest = {'glass': GLASS, 'ring_r': RING_R, 'plane_px': PLANE_PX, 'min_text_px': 14, 'bands': BANDS,
                'families': {}, 'planes': {}, 'arms': {}, 'backgrounds': {}}
    for fam, (title, band_on, shapes, _, band, _) in FAMILIES.items():
        manifest['families'][fam] = {'title': title, 'band': band_on, 'single': SINGLES[fam][0], 'file_band': band,
                                     'background': PAIRS[fam][0], 'arm': PAIRS[fam][1]}
        manifest['planes'][fam] = {}
        for n in keys(fam):
            write(f'planes/{fam}-{n}.svg', plane_svg(fam, n))
            manifest['planes'][fam][n] = {'file': f'planes/{fam}-{n}.svg', 'what': _pick(fam, n)[0]['what'],
                                          'size': [100, 100], 'pivot': [50, 50], 'nose': 'up'}
    arm_markup = {}
    for n, (title, desc, fn) in ARMS.items():
        svg, pivot, size = arm_svg(fn)
        write(f'arms/{n}.svg', svg)
        arm_markup[n] = (svg, pivot, size)
        manifest['arms'][n] = {'file': f'arms/{n}.svg', 'title': title, 'what': desc,
                               'size': list(size), 'pivot': list(pivot), 'length': RING_R}
    bg_markup = {}
    for n, (title, desc, fn, look) in BACKGROUNDS.items():
        svg = bg_svg(fn)
        write(f'backgrounds/{n}.svg', svg)
        bg_markup[n] = svg
        manifest['backgrounds'][n] = {'file': f'backgrounds/{n}.svg', 'title': title, 'what': desc,
                                      'size': [GLASS, GLASS], **look}
    write('art.json', json.dumps(manifest, indent=1))
    write('review.html', review(arm_markup, bg_markup))
    art_pack(arm_markup, bg_markup)


# ---- the editor's art (flight-tracker-design §12.2) ---------------------------------

def art_pack(arm_markup, bg_markup):
    """apps/flights/editor/flights-art.js: the art as data for the editor's preview and
    bake. A plane is the inside of its 100 x 100 SVG with "{{B}}" where its band colour
    goes (the review's var(--b)); a family whose colour-off planes are untinted metal
    carries those as "plain". The wedge is the editor's own port of arm_wedge, so its
    trail and colours can move; the other arms and the backgrounds ride whole."""
    fams = {}
    for fam, (title, band_on, _shapes, _, neutral, grads) in FAMILIES.items():
        f = {'title': title, 'band': band_on, 'single': SINGLES[fam][0], 'neutral': neutral,
             'pair': list(PAIRS[fam]), 'gradients': list(grads), 'shapes': {}, 'plain': {}}
        for n in keys(fam):
            p, fn = _pick(fam, n)
            f['shapes'][n] = fn(p, '{{B}}', 'm-%s-%s' % (fam, n))
            if neutral is None:
                f['plain'][n] = fn(p, None, 'm-%s-%s' % (fam, n))
        fams[fam] = f
    arms = {}
    for n, (svg, (px, py), (w, h)) in arm_markup.items():
        arms[n] = {'title': ARMS[n][0], 'what': ARMS[n][1], 'svg': svg, 'pivot': [px, py], 'size': [w, h]}
    bgs = {n: {'title': BACKGROUNDS[n][0], 'what': BACKGROUNDS[n][1], 'svg': svg, 'look': BACKGROUNDS[n][3]}
           for n, svg in bg_markup.items()}
    data = {'glass': GLASS, 'cx': CX, 'ring_r': RING_R, 'plane_px': PLANE_PX, 'bands': BANDS,
            'gradients': gradient_defs(GRADIENTS), 'families': fams, 'arms': arms, 'backgrounds': bgs}
    out = os.path.join(HERE, '..', 'editor', 'flights-art.js')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('/* flights-art.js: written by apps/flights/art/make_art.py; do not edit. */\n'
                'window.FlightsArt = ' + json.dumps(data, separators=(',', ':')) + ';\n')


# ---- review page ------------------------------------------------------------------

def inner(svg):
    return svg[svg.index('>') + 1:svg.rindex('</svg>')]


def seg(group, items, on):
    return ''.join(f'<button type="button" id="{group}-{k}" data-v="{k}" aria-pressed="{str(k == on).lower()}">{t}</button>'
                   for k, t in items)


def review(arm_markup, bg_markup):
    import html as H
    symbols = gradient_defs(GRADIENTS) + ''.join(plane_symbol(f, n) for f in FAMILIES for n in keys(f))
    bgs = ''.join(f'<symbol id="bg-{n}" viewBox="0 0 {GLASS} {GLASS}">{inner(s)}</symbol>' for n, s in bg_markup.items())
    arms = ''
    for n, (svg, (px, py), (w, h)) in arm_markup.items():
        vb = svg.split('viewBox="')[1].split('"')[0]
        arms += f'<symbol id="arm-{n}" viewBox="{vb}">{inner(svg)}</symbol>'
    arm_geo = {n: {'px': p[0], 'py': p[1], 'w': s[0], 'h': s[1]} for n, (_, p, s) in arm_markup.items()}
    looks = {n: b[3] for n, b in BACKGROUNDS.items()}
    cards, rows = '', ''
    for fam, (_, _, shapes, _, fband, _) in FAMILIES.items():
        bg = looks[PAIRS[fam][0]]['blipbg']
        for n in keys(fam):
            form = 'single' if n == 'single' else 'types'
            hid = '' if (fam, form) == ('modern', 'types') else ' hidden'
            tag = f'data-fam="{fam}" data-form="{form}"{hid}'
            label = 'every category' if n == 'single' else n
            cards += (f'<figure class="plane" {tag}><svg viewBox="0 0 100 100" class="big" '
                      f'style="--b:{fband or "transparent"};background:{bg}"><use href="#p-{fam}-{n}"/></svg>'
                      f'<figcaption><b>{label}</b><span>{H.escape(_pick(fam, n)[0]["what"])}</span></figcaption></figure>')
            rows += (f'<tr {tag}><th scope="row">{label}</th>' + ''.join(
                f'<td data-i="{i}"><svg class="blip" viewBox="0 0 100 100" style="--b:{c}"><use href="#p-{fam}-{n}"/></svg></td>'
                for i, (_, c) in enumerate(BANDS)) + '</tr>')
    head = ''.join(f'<th scope="col"><i style="background:{c}"></i>{H.escape(b)}</th>' for b, c in BANDS)
    arm_cards = ''.join(
        f'<figure class="asset"><svg viewBox="0 0 {s[0]} {s[1]}" class="armfull"><use href="#arm-{n}" width="{s[0]}" height="{s[1]}"/></svg>'
        f'<figcaption><b>{H.escape(ARMS[n][0])}</b><span>{H.escape(ARMS[n][1])} '
        f'Pivot {p[0]}, {p[1]} in a {s[0]} × {s[1]} box.</span></figcaption></figure>'
        for n, (svg, p, s) in arm_markup.items())
    bg_cards = ''.join(
        f'<figure class="asset"><svg viewBox="0 0 {GLASS} {GLASS}" class="bgfull"><use href="#bg-{n}"/></svg>'
        f'<figcaption><b>{H.escape(BACKGROUNDS[n][0])}</b><span>{H.escape(BACKGROUNDS[n][1])}</span></figcaption></figure>'
        for n in bg_markup)
    fams = {f: {'title': v[0], 'band': v[1], 'single': SINGLES[f][0], 'pair': PAIRS[f], 'neutral': v[4] or 'transparent'}
            for f, v in FAMILIES.items()}
    data = json.dumps({'arms': arm_geo, 'looks': looks, 'bands': BANDS, 'families': fams,
                       'glass': GLASS, 'ring': RING_R, 'plane_px': PLANE_PX})
    return TEMPLATE.replace('%SYMBOLS%', symbols + bgs + arms).replace('%CARDS%', cards) \
        .replace('%HEAD%', head).replace('%ROWS%', rows).replace('%ARMS%', arm_cards) \
        .replace('%BGS%', bg_cards).replace('%DATA%', data).replace('%GLASS%', str(GLASS)) \
        .replace('%PLANE_PX%', str(PLANE_PX)) \
        .replace('%SEG_FAM%', seg('fam', [(f, v[0]) for f, v in FAMILIES.items()], 'modern')) \
        .replace('%SEG_BG%', seg('bg', [(k, v[0]) for k, v in BACKGROUNDS.items()], 'scope')) \
        .replace('%SEG_ARM%', seg('arm', [(k, v[0]) for k, v in ARMS.items()] + [('none', 'None')], 'wedge')) \
        .replace('%FAM_BAND%', H.escape(FAMILIES['modern'][1]))


TEMPLATE = r'''<title>Flights Art Review</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600&family=IBM+Plex+Sans:wght@400;500&family=IBM+Plex+Mono:wght@400&family=Montserrat:wght@500&display=swap">
<style>
/* Layout: an instrument bench. The live scope leads; parts sit below it in plain rows.
   Single dark look on purpose: the glass is dark, and the art is judged on it. */
:root {
  --bench: #101418; --panel: #171C21; --rule: #2A323A; --ink: #E4E8EB; --muted: #8C969F;
  --signal: #F0662E;
  --display: "Barlow Condensed", "Arial Narrow", sans-serif;
  --body: "IBM Plex Sans", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, monospace;
  color-scheme: dark;
}
body { background: var(--bench); color: var(--ink); font: 15px/1.55 var(--body); }
[hidden] { display: none !important; }
main { max-width: 1080px; margin: 0 auto; padding-block: 28px 64px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
  display: grid; gap: 44px; }
h1, h2 { font-family: var(--display); font-weight: 600; letter-spacing: .02em; text-wrap: balance; margin: 0; }
h1 { font-size: 2.1rem; line-height: 1.1; }
h2 { font-size: 1.35rem; text-transform: uppercase; letter-spacing: .08em; color: var(--muted); }
p { margin: 0; max-width: 65ch; }
section { display: grid; gap: 16px; min-width: 0; }
.lede { color: var(--muted); }
.scope-wrap { display: grid; grid-template-columns: minmax(0, 466px) minmax(0, 1fr); gap: 28px; align-items: start; }
@media (max-width: 760px) { .scope-wrap { grid-template-columns: minmax(0, 1fr); } }
#scope { width: 100%; max-width: 466px; aspect-ratio: 1; border-radius: 50%; display: block;
  box-shadow: 0 0 0 6px #05070A, 0 0 0 7px var(--rule); }
.true #scope { width: var(--orb-px); max-width: none; justify-self: center; margin-block: 8px; }
.stage { display: grid; gap: 18px; justify-items: center; min-width: 0; }
.card-check { grid-column: 1 / -1; justify-self: center; max-width: 100%; width: var(--card-w); height: var(--card-h); border: 1.5px dashed var(--muted); border-radius: var(--card-r);
  display: grid; place-items: center; text-align: center; font-size: .78rem; color: var(--muted); padding: 6px; box-sizing: border-box; }
.dpi-row { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 12px; }
.dpi-row input { width: 5.5em; font: 500 .95rem var(--mono); color: var(--ink); background: var(--panel);
  border: 1px solid var(--rule); border-radius: 4px; padding: 5px 8px; }
.controls { display: grid; gap: 18px; }
fieldset { border: 0; padding: 0; margin: 0; display: grid; gap: 8px; }
legend { font: 600 .8rem var(--display); text-transform: uppercase; letter-spacing: .1em; color: var(--muted); padding: 0 0 6px; }
.seg { display: flex; flex-wrap: wrap; gap: 6px; }
.seg button { font: 500 .9rem var(--body); color: var(--ink); background: var(--panel); border: 1px solid var(--rule);
  border-radius: 4px; padding: 6px 12px; cursor: pointer; }
.seg button[aria-pressed="true"] { border-color: var(--signal); color: #fff; background: #2A1A12; }
button:focus-visible, input:focus-visible { outline: 2px solid var(--signal); outline-offset: 2px; }
input[type=range] { width: 100%; accent-color: var(--signal); }
.readout { font: .85rem var(--mono); color: var(--muted); font-variant-numeric: tabular-nums; }
.legend-bands { display: flex; flex-wrap: wrap; gap: 6px 14px; font-size: .82rem; color: var(--muted); }
.legend-bands i, th i { display: inline-block; width: 9px; height: 9px; border-radius: 50%; margin-right: 6px; vertical-align: 0; }
.planes { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 12px; }
figure { margin: 0; }
.plane, .asset { background: var(--panel); border: 1px solid var(--rule); border-radius: 6px; padding: 14px; display: grid; gap: 10px; align-content: start; }
.plane .big { width: 100%; max-width: 128px; justify-self: center; aspect-ratio: 1; border-radius: 4px; }
figcaption { display: grid; gap: 2px; font-size: .85rem; color: var(--muted); }
figcaption b { font: 600 1.05rem var(--display); letter-spacing: .03em; color: var(--ink); text-transform: capitalize; }
.table-wrap { overflow-x: auto; border: 1px solid var(--rule); border-radius: 6px; }
table { border-collapse: collapse; width: 100%; min-width: 560px; }
th, td { padding: 8px 10px; text-align: center; border-bottom: 1px solid var(--rule); }
thead th { font: 500 .78rem var(--body); color: var(--muted); white-space: nowrap; }
tbody th { text-align: left; font: 600 1rem var(--display); text-transform: capitalize; }
#blips td { background: var(--blipbg, #06160D); }
.blip { width: var(--blip, 32px); height: var(--blip, 32px); display: block; margin: 0 auto; }
.assets { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 12px; }
.armfull { height: 260px; width: 100%; background: #0E1A2A; border-radius: 4px; }
.bgfull { width: 100%; aspect-ratio: 1; border-radius: 50%; }
.open { display: grid; gap: 10px; padding-left: 1.2em; margin: 0; max-width: 70ch; }
code { font: .88em var(--mono); color: var(--ink); }
@media (prefers-reduced-motion: reduce) { #scope * { animation: none !important; } }
</style>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>%SYMBOLS%</defs></svg>
<main>
  <header style="display:grid;gap:10px">
    <h1>Flights art, five families</h1>
    <p class="lede">Plane families, radar arms and backgrounds for the tracker's themes. Each family comes in two forms: a plane per category, or one detailed plane for every category. Picking a family shows it with its own background and arm; every control still mixes freely. Range rings are drawn live, as the app draws them. The traffic is made up.</p>
  </header>

  <section aria-labelledby="h-scope">
    <h2 id="h-scope">On the glass</h2>
    <div class="scope-wrap">
      <div class="stage">
        <svg id="scope" viewBox="0 0 %GLASS% %GLASS%" role="img" aria-label="Scope preview with simulated traffic"></svg>
      </div>
      <div class="controls">
        <fieldset><legend>Plane family</legend><div class="seg" id="c-fam">%SEG_FAM%</div>
          <p class="lede" style="font-size:.88rem" id="fam-band">%FAM_BAND%</p></fieldset>
        <fieldset><legend>Plane shapes</legend><div class="seg" id="c-form">
          <button type="button" id="form-types" data-v="types" aria-pressed="true">One per category</button>
          <button type="button" id="form-single" data-v="single" aria-pressed="false">One detailed plane</button></div></fieldset>
        <fieldset><legend>Altitude colour</legend><div class="seg" id="c-col">
          <button type="button" id="col-bands" data-v="bands" aria-pressed="true">By band</button>
          <button type="button" id="col-off" data-v="off" aria-pressed="false">Off</button></div>
          <p class="lede" style="font-size:.88rem">Off: every plane in the family's own colour, and ground traffic at 45% so it recedes. With the app's Ground setting off there is no ground traffic to dim.</p></fieldset>
        <fieldset><legend>Background</legend><div class="seg" id="c-bg">%SEG_BG%</div></fieldset>
        <fieldset><legend>Radar arm</legend><div class="seg" id="c-arm">%SEG_ARM%</div></fieldset>
        <fieldset><legend>Plane size on the glass</legend>
          <input type="range" id="c-size" min="30" max="48" step="2" value="%PLANE_PX%" aria-describedby="size-out">
          <span class="readout" id="size-out">38 px</span></fieldset>
        <fieldset><legend>View</legend><div class="seg" id="c-view">
          <button type="button" id="view-true" data-v="true" aria-pressed="true">Orb size</button>
          <button type="button" id="view-big" data-v="big" aria-pressed="false">Enlarged</button></div>
          <div class="dpi-row"><label for="c-dpi">Monitor dpi</label>
            <input type="number" id="c-dpi" min="60" max="400" step="1" value="177" aria-describedby="dpi-out">
            <span class="readout" id="dpi-out"></span></div>
          <div class="dpi-row"><label for="c-card">The card outline measures</label>
            <input type="number" id="c-card" min="0.5" max="8" step="0.01" placeholder="3.37" aria-describedby="card-out">
            <span class="readout" id="card-out">in wide. A real card is 3.37 in.</span></div></fieldset>
        <fieldset><legend>App text</legend><div class="seg" id="c-text">
          <button type="button" id="text-on" data-v="on" aria-pressed="true">Shown</button>
          <button type="button" id="text-off" data-v="off" aria-pressed="false">Hidden</button></div></fieldset>
        <fieldset><legend>Altitude bands</legend><div class="legend-bands" id="bands"></div></fieldset>
        <p class="lede" style="font-size:.88rem">Each contact uses the shape for its ADS-B category and the colour for its altitude band. The sweep turns once every 4 s. The header, footer and info card are the app's own text, in Montserrat at the orb's 16 and 14 px, placed where <code>main.c</code> puts them.</p>
        <p class="lede" style="font-size:.88rem">Orb size draws the 466 px face across 1.75 in, the panel's width. The panel packs about 266 pixels per inch, more than your monitor, so edges look a little crisper on the orb. The size you see here is what you'll see at arm's length.</p>
      </div>
      <div class="card-check" id="card-check">Hold a bank card over this outline. If the edges match, the orb above is at its real size.</div>
    </div>
  </section>

  <section aria-labelledby="h-planes">
    <h2 id="h-planes">Planes</h2>
    <p class="lede">Drawn nose up on a 100 × 100 box with the pivot at the centre. Shown here in each family's file colour, the metals untinted; the table below shows every altitude band.</p>
    <div class="planes">%CARDS%</div>
  </section>

  <section aria-labelledby="h-blips">
    <h2 id="h-blips">At blip size, every band</h2>
    <p class="lede">One baked copy per band; with altitude colour off, two (airborne and ground). The family, colour switch, size slider, background and view above drive this table too. In orb size, each plane is drawn as large as it will be on the glass.</p>
    <div class="table-wrap"><table id="blips"><thead><tr><th scope="col">Shape</th>%HEAD%</tr></thead><tbody>%ROWS%</tbody></table></div>
  </section>

  <section aria-labelledby="h-arms">
    <h2 id="h-arms">Radar arms and backgrounds</h2>
    <div class="assets">%ARMS%%BGS%</div>
  </section>

  <section aria-labelledby="h-open">
    <h2 id="h-open">Still to decide</h2>
    <ol class="open">
      <li>Whether the detailed planes' lines and shading read at orb size, or need fewer lines.</li>
      <li>The paper fleet's shapes: whether each category's paper plane is easy to tell apart at orb size, the whirligig especially.</li>
      <li>Shapes per aircraft model (an A380 apart from a 737) as an option under each category, later.</li>
      <li>The vintage stand-ins for each category: a twin-boom early jet for business jets, an autogyro for rotorcraft, a propliner for airliners.</li>
    </ol>
  </section>
</main>
<script>
const D = %DATA%;
const NS = 'http://www.w3.org/2000/svg';
const scope = document.getElementById('scope');
const ORB_IN = 1.75, CARD_MM = [85.6, 53.98, 3.18];     // panel width; ISO/IEC 7810 ID-1 card
const store = { get(k) { try { return localStorage.getItem(k); } catch { return null; } },
                set(k, v) { try { localStorage.setItem(k, v); } catch {} } };
// #brass opens a family with its pairing; -single picks its detailed plane, -off turns
// altitude colour off: #brass-single-off
const hash = location.hash.slice(1).split('-');
const start = hash[0] in D.families ? hash[0] : 'modern';
const state = { fam: start, bg: D.families[start].pair[0], arm: D.families[start].pair[1], size: D.plane_px,
                view: 'true', text: 'on', col: hash.includes('off') ? 'off' : 'bands',
                form: hash.includes('single') ? 'single' : 'types', dpi: +(store.get('flights-art-dpi') || 177) };
document.getElementById('c-dpi').value = state.dpi;
const GROUPS = ['fam', 'form', 'col', 'bg', 'arm', 'view', 'text'];
const GROUND_OPA = 0.45;                              // colour off: ground traffic recedes
const press = () => { for (const id of GROUPS) for (const o of document.querySelectorAll('#c-' + id + ' button')) o.setAttribute('aria-pressed', o.dataset.v === state[id]); };

// Page px for a length on the monitor: physical px = inches * dpi, and the browser's
// pixel ratio (OS scaling times zoom) turns physical px into page px.
const pagePx = inches => inches * state.dpi / (window.devicePixelRatio || 1);
function scale() {
  const orb = pagePx(ORB_IN), k = state.view === 'true' ? orb / D.glass : 1, mm = v => pagePx(v / 25.4) + 'px';
  const r = document.documentElement.style;
  document.body.classList.toggle('true', state.view === 'true');
  document.getElementById('card-check').hidden = state.view !== 'true';
  r.setProperty('--orb-px', orb + 'px');
  r.setProperty('--card-w', mm(CARD_MM[0])); r.setProperty('--card-h', mm(CARD_MM[1])); r.setProperty('--card-r', mm(CARD_MM[2]));
  r.setProperty('--blip', (state.size * k) + 'px');
  document.getElementById('dpi-out').textContent = `orb ${(ORB_IN * state.dpi).toFixed(0)} screen px, browser scale ${(window.devicePixelRatio || 1).toFixed(2)}`;
  document.getElementById('size-out').textContent = `${state.size} px on the orb, ${(state.size / D.glass * ORB_IN * 25.4).toFixed(1)} mm`;
}

// The app's own text (apps/flights/main.c draw_text) in info mode on the first contact.
// LVGL's Montserrat: 16 px on an 18 px line (baseline 3 up), 14 px on a 16 px line.
const CARD = ['UAL1532  A320', 'SFO → DEN', '34000 ft', '452 kt  trk 220'];   // distance and bearing dropped: the map shows them
const el = (n, a = {}) => { const e = document.createElementNS(NS, n); for (const k in a) e.setAttribute(k, a[k]); return e; };
function appText(C, look) {
  const t = (y, size, fill) => el('text', { x: C, y, 'text-anchor': 'middle', fill, 'font-size': size,
    'font-family': 'Montserrat, sans-serif', 'font-weight': 500, style: 'white-space:pre' });
  const head = t(16 + 15, 16, look.head); head.textContent = '1 of ' + TRAFFIC.length;
  const foot = t(D.glass - 14 - 3, 14, look.foot); foot.textContent = 'live  adsb.lol';
  scope.append(head, foot);
  const card = el('text', { fill: look.card, 'font-size': 16, 'font-family': 'Montserrat, sans-serif', 'font-weight': 500, style: 'white-space:pre' });
  CARD.forEach((line, i) => { const s = el('tspan', { y: D.glass - 40 - 3 - (CARD.length - 1 - i) * 18 }); s.textContent = line; card.append(s); });
  scope.append(card);
  // a label is as wide as its longest line, centred, lines flush left inside it
  const w = Math.max(...[...card.children].map(s => s.getComputedTextLength()));
  for (const s of card.children) s.setAttribute('x', (C - w / 2).toFixed(1));
}

// made-up traffic: [category, altitude ft (null = none, 0 = ground), range 0..1, bearing, track]
const TRAFFIC = [['airliner', 34000, .82, 40, 220], ['heavy', 38000, .55, 300, 95], ['airliner', 12000, .38, 140, 330],
  ['bizjet', 24000, .7, 205, 15], ['turboprop', 8500, .3, 260, 60], ['light', 2400, .18, 95, 180],
  ['helicopter', 1200, .12, 330, 120], ['generic', null, .9, 160, 270], ['airliner', 0, .06, 20, 300],
  ['light', 4500, .62, 120, 250], ['heavy', 31000, .95, 250, 40], ['turboprop', 16000, .47, 10, 200]];
const band = a => a === 0 ? D.bands[0][1] : a == null ? D.bands[1][1] : a < 3000 ? D.bands[2][1] : a < 10000 ? D.bands[3][1]
  : a < 20000 ? D.bands[4][1] : a < 30000 ? D.bands[5][1] : D.bands[6][1];

function draw() {
  const look = D.looks[state.bg], C = D.glass / 2;
  scope.replaceChildren();
  scope.append(el('use', { href: '#bg-' + state.bg, width: D.glass, height: D.glass }));
  for (const r of [D.ring, D.ring / 2]) scope.append(el('circle', { cx: C, cy: C, r, fill: 'none', stroke: look.ring, 'stroke-opacity': look.ring_opa, 'stroke-width': 1.5 }));
  if (state.arm !== 'none') {
    const g = D.arms[state.arm];
    const spin = el('g'); spin.style.cssText = `transform-origin:${C}px ${C}px;animation:spin 4s linear infinite`;
    spin.append(el('use', { href: '#arm-' + state.arm, x: C - g.px, y: C - g.py, width: g.w, height: g.h,
      style: state.arm === 'wedge' ? 'mix-blend-mode:screen' : '' }));
    scope.append(spin);
  }
  TRAFFIC.forEach(([cat, alt, rr, brg, trk], i) => {
    const a = brg * Math.PI / 180, x = C + rr * D.ring * Math.sin(a), y = C - rr * D.ring * Math.cos(a), s = state.size;
    const g = el('g', { transform: `translate(${x.toFixed(1)} ${y.toFixed(1)}) rotate(${trk})` });
    const off = state.col === 'off';
    g.style.cssText = `--b:${off ? D.families[state.fam].neutral : band(alt)}`;
    if (off && alt === 0) g.setAttribute('opacity', GROUND_OPA);
    g.append(el('use', { href: `#p-${state.fam}-${state.form === 'single' ? 'single' : cat}`, x: -s / 2, y: -s / 2, width: s, height: s }));
    scope.append(g);
    if (i === 0 && state.text === 'on')             // the selected contact's ring, sized to the plane
      scope.append(el('circle', { cx: x, cy: y, r: s * 0.62, fill: 'none', stroke: look.sel, 'stroke-width': 1.5 }));
  });
  scope.append(el('circle', { cx: C, cy: C, r: 3, fill: look.home }));
  if (state.text === 'on') appText(C, look);
  for (const n of document.querySelectorAll('[data-fam]')) n.hidden = n.dataset.fam !== state.fam || n.dataset.form !== state.form;
  for (const td of document.querySelectorAll('#blips td')) {        // the table follows the colour switch
    const i = +td.dataset.i, off = state.col === 'off', svg = td.firstElementChild;
    svg.style.setProperty('--b', off ? D.families[td.parentElement.dataset.fam].neutral : D.bands[i][1]);
    svg.style.opacity = off && i === 0 ? GROUND_OPA : '';
  }
  document.getElementById('fam-band').textContent = D.families[state.fam][state.form === 'single' ? 'single' : 'band'];
  document.documentElement.style.setProperty('--blipbg', look.blipbg);
  press();
  scale();
}
const css = document.createElement('style'); css.textContent = '@keyframes spin{to{transform:rotate(360deg)}}'; document.head.append(css);
for (const id of GROUPS) document.getElementById('c-' + id).addEventListener('click', e => {
  const b = e.target.closest('button'); if (!b) return;
  state[id] = b.dataset.v;
  if (id === 'fam') [state.bg, state.arm] = D.families[state.fam].pair;
  draw();
});
document.getElementById('c-size').addEventListener('input', e => { state.size = +e.target.value; draw(); });
document.getElementById('c-dpi').addEventListener('input', e => {
  const v = +e.target.value; if (v >= 60 && v <= 400) { state.dpi = v; store.set('flights-art-dpi', v); scale(); } });
// A measured outline corrects the dpi: it was drawn 3.37 in wide at the current dpi.
document.getElementById('c-card').addEventListener('change', e => {
  const m = +e.target.value, out = document.getElementById('card-out');
  if (!(m > 0.5 && m < 8)) { out.textContent = 'Enter the outline width in inches, such as 2.33.'; return; }
  const v = Math.round(state.dpi * (CARD_MM[0] / 25.4) / m);
  if (v < 60 || v > 400) { out.textContent = `That works out to ${v} dpi, outside 60 to 400. Measure again.`; return; }
  state.dpi = v; store.set('flights-art-dpi', v); document.getElementById('c-dpi').value = v; e.target.value = '';
  out.textContent = `Set to ${v} dpi. Measure again to check.`; scale();
});
addEventListener('resize', scale);                    // browser zoom changes the pixel ratio
document.fonts?.ready.then(draw);                     // re-measure the card once Montserrat is in
document.getElementById('bands').innerHTML = D.bands.map(([n, c]) => `<span><i style="background:${c}"></i>${n}</span>`).join('');
draw();
</script>
'''

if __name__ == '__main__':
    main()
