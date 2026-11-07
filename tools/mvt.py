"""A minimal Mapbox Vector Tile (v2) reader: layers -> features with tags and
geometry in tile units. Enough for the map preview; not the hub's decoder."""
import gzip, struct

def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80: return r, i

def _fields(b):
    i, n = 0, len(b)
    while i < n:
        k, i = _varint(b, i)
        f, t = k >> 3, k & 7
        if t == 0: v, i = _varint(b, i)
        elif t == 2:
            l, i = _varint(b, i); v = b[i:i + l]; i += l
        elif t == 1: v = b[i:i + 8]; i += 8
        elif t == 5: v = b[i:i + 4]; i += 4
        else: raise ValueError("wire type %d" % t)
        yield f, t, v

def _packed(b):
    out, i = [], 0
    while i < len(b):
        v, i = _varint(b, i); out.append(v)
    return out

def _value(b):
    for f, t, v in _fields(b):
        if f == 1: return v.decode("utf-8", "replace")
        if f == 2: return struct.unpack("<f", v)[0]
        if f == 3: return struct.unpack("<d", v)[0]
        if f in (4, 5): return v
        if f == 6: return (v >> 1) ^ -(v & 1)
        if f == 7: return bool(v)

def _geom(cmds, gtype):
    """Rings/lines as lists of (x, y); points as single-point lists."""
    out, cur, x, y, i = [], [], 0, 0, 0
    z = lambda v: (v >> 1) ^ -(v & 1)
    while i < len(cmds):
        c = cmds[i]; i += 1
        op, cnt = c & 7, c >> 3
        if op == 1:                         # MoveTo
            for _ in range(cnt):
                if cur: out.append(cur)
                x += z(cmds[i]); y += z(cmds[i + 1]); i += 2
                cur = [(x, y)]
        elif op == 2:                       # LineTo
            for _ in range(cnt):
                x += z(cmds[i]); y += z(cmds[i + 1]); i += 2
                cur.append((x, y))
        elif op == 7 and cur:               # ClosePath
            cur.append(cur[0])
    if cur: out.append(cur)
    return out

def read(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b": raw = gzip.decompress(raw)
    layers = {}
    for f, t, lb in _fields(raw):
        if f != 3: continue
        name, extent, keys, vals, feats = "", 4096, [], [], []
        for lf, lt, lv in _fields(lb):
            if lf == 1: name = lv.decode()
            elif lf == 5: extent = lv
            elif lf == 3: keys.append(lv.decode())
            elif lf == 4: vals.append(_value(lv))
            elif lf == 2: feats.append(lv)
        out = []
        for fb in feats:
            tags, gtype, cmds = [], 0, []
            for ff, ft, fv in _fields(fb):
                if ff == 2: tags = _packed(fv)
                elif ff == 3: gtype = fv
                elif ff == 4: cmds = _packed(fv)
            props = {keys[tags[k]]: vals[tags[k + 1]] for k in range(0, len(tags), 2)}
            out.append((gtype, props, _geom(cmds, gtype)))
        layers[name] = (extent, out)
    return layers
