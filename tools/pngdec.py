"""Minimal stdlib PNG decoder (8-bit, non-interlaced) for the kit tools.

System python on this box has no Pillow, and the acceptance gate has to run
in a bare environment to be a gate anyone can execute. Supports the colour
types the watch kits ship (0/2/3/4/6); bit depth 8 and no interlacing only,
which is what prep_watch_kit.py emits.
"""
import struct, zlib

def decode(path):
    d = open(path,'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    i = 8; idat = b''; plte = None; trns = None; ihdr = None; gctl=None
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]
        typ = d[i+4:i+8]; body = d[i+8:i+8+ln]; i += 12 + ln
        if typ == b'IHDR': ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'IDAT': idat += body
        elif typ == b'PLTE': plte = body
        elif typ == b'tRNS': trns = body
        elif typ == b'IEND': break
    W,H,bd,ct,comp,filt,inter = ihdr
    assert bd == 8 and inter == 0, ihdr
    nch = {0:1, 2:3, 3:1, 4:2, 6:4}[ct]
    raw = zlib.decompress(idat)
    stride = W*nch
    out = bytearray(W*H*nch)
    prev = bytearray(stride)
    p = 0
    for y in range(H):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for x in range(nch, stride): line[x] = (line[x] + line[x-nch]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x-nch] if x >= nch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x-nch] if x >= nch else 0
                b = prev[x]
                c = prev[x-nch] if x >= nch else 0
                pa = abs(b-c); pb = abs(a-c); pc = abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return dict(W=W,H=H,ct=ct,nch=nch,px=out,plte=plte,trns=trns)

def gray8(img):
    """-> list of ints, one sample per pixel (palette expanded to luminance)."""
    W,H,nch,px = img['W'],img['H'],img['nch'],img['px']
    if img['ct'] == 0:
        return list(px)
    if img['ct'] == 3:
        pl = img['plte']
        return [ (pl[p*3]*77+pl[p*3+1]*151+pl[p*3+2]*28)>>8 for p in px ]
    if img['ct'] == 2:
        return [ (px[i*3]*77+px[i*3+1]*151+px[i*3+2]*28)>>8 for i in range(W*H) ]
    if img['ct'] == 6:
        return [ (px[i*4]*77+px[i*4+1]*151+px[i*4+2]*28)>>8 for i in range(W*H) ]
    raise AssertionError(img['ct'])

def alpha8(img):
    """-> list of alphas per pixel (255 where the format has no alpha)."""
    W,H,nch,px = img['W'],img['H'],img['nch'],img['px']
    if img['ct'] == 6:  return [ px[i*4+3] for i in range(W*H) ]
    if img['ct'] == 4:  return [ px[i*2+1] for i in range(W*H) ]
    if img['ct'] == 3:
        t = img['trns'] or b''
        return [ (t[p] if p < len(t) else 255) for p in px ]
    return [255]*(W*H)
