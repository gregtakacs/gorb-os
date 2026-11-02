#!/usr/bin/env python3
"""make_bluenoise.py — the dither's threshold table (D43 §5.3), generated once.

The bake quantizes every plate to RGB565 through an ordered dither whose threshold is
a function of the absolute panel pixel only, so an unchanged input pixel bakes to an
unchanged output pixel in every keyframe and a moving plate's delta stays honest. The
threshold comes from a 64x64 blue-noise tile: a permutation of 0..4095 whose small
values are spread as evenly as possible, at every density. Blue noise rather than
Bayer because the owner ruled it (D43 §11.2): a Bayer matrix shows its cross-hatch on
a 1.75 in OLED, blue noise reads as fine grain.

This script is the table's provenance, not part of any build. The table it prints is
committed as constant bytes in the JS baker and in bake_clock.py, and a test pins both
copies to the hash below; re-running this only matters if the table is ever replaced,
and then the hash moves on purpose.

Algorithm: Ulichney's void-and-cluster (1993) on a torus, Gaussian sigma 1.5:
  1. a random ~10% binary pattern, relaxed by moving the tightest cluster into the
     largest void until the move is a no-op;
  2. its ones are ranked downward by removing the tightest cluster each time;
  3. zeros are ranked upward by filling the largest void until half the tile is set;
  4. the rest are ranked by filling the largest void of the remaining zeros.
The seed is fixed and RandomState is numpy's frozen legacy stream, so a re-run
reproduces the same table on any numpy that still ships it.

Usage:
  make_bluenoise.py              print the table's sha256 and a level histogram
  make_bluenoise.py --b64        print the 4096 threshold bytes as base64
  make_bluenoise.py --check      exit 1 unless the generated table hashes to TABLE_SHA256
"""
import base64
import hashlib
import sys

import numpy as np

N = 64
SIGMA = 1.5
SEED = 43                      # D43; any fixed seed works, this one is the shipped one
TABLE_SHA256 = "683a759b831dc8d0c83205827301e65adbb2d06f6b5b11af800a694f6f8f2bc7"


def _kernel():
    """The toroidal Gaussian centred on (0, 0), as the energy one set pixel adds."""
    d = np.minimum(np.arange(N), N - np.arange(N)).astype(np.float64)
    g = np.exp(-(d[:, None] ** 2 + d[None, :] ** 2) / (2 * SIGMA * SIGMA))
    return g


def _energy(pattern, k):
    return np.real(np.fft.ifft2(np.fft.fft2(pattern) * np.fft.fft2(k)))


def _add(energy, k, y, x, sign):
    energy += sign * np.roll(np.roll(k, y, axis=0), x, axis=1)


def _tightest(energy, pattern):
    e = np.where(pattern == 1, energy, -np.inf)
    i = int(np.argmax(e))
    return divmod(i, N)


def _largest_void(energy, pattern):
    e = np.where(pattern == 0, energy, np.inf)
    i = int(np.argmin(e))
    return divmod(i, N)


def generate():
    k = _kernel()
    rng = np.random.RandomState(SEED)
    pattern = (rng.random_sample((N, N)) < 0.1).astype(np.float64)
    ones = int(pattern.sum())
    energy = _energy(pattern, k)
    # 1. relax the initial pattern
    while True:
        cy, cx = _tightest(energy, pattern)
        pattern[cy, cx] = 0
        _add(energy, k, cy, cx, -1)
        vy, vx = _largest_void(energy, pattern)
        if (vy, vx) == (cy, cx):
            pattern[cy, cx] = 1
            _add(energy, k, cy, cx, +1)
            break
        pattern[vy, vx] = 1
        _add(energy, k, vy, vx, +1)
    initial = pattern.copy()
    rank = np.full((N, N), -1, dtype=np.int64)
    # 2. rank the initial ones downward
    p = initial.copy()
    e = _energy(p, k)
    for r in range(ones - 1, -1, -1):
        y, x = _tightest(e, p)
        p[y, x] = 0
        _add(e, k, y, x, -1)
        rank[y, x] = r
    # 3. fill voids up to half
    p = initial.copy()
    e = _energy(p, k)
    for r in range(ones, N * N // 2):
        y, x = _largest_void(e, p)
        p[y, x] = 1
        _add(e, k, y, x, +1)
        rank[y, x] = r
    # 4. the second half: the energy of the remaining zeros, filled from the sparsest
    inv = 1.0 - p
    e = _energy(inv, k)
    for r in range(N * N // 2, N * N):
        y, x = _tightest(e, inv)          # tightest cluster of zeros = largest void of ones
        inv[y, x] = 0
        _add(e, k, y, x, -1)
        rank[y, x] = r
    assert sorted(rank.ravel().tolist()) == list(range(N * N)), "rank is not a permutation"
    return rank


def table_bytes(rank=None):
    """4096 thresholds, row-major, rank scaled onto 0..255 (16 cells per level)."""
    if rank is None:
        rank = generate()
    return bytes((rank.ravel() * 256 // (N * N)).astype(np.uint8).tolist())


def main(argv):
    t = table_bytes()
    h = hashlib.sha256(t).hexdigest()
    if "--b64" in argv:
        print(base64.b64encode(t).decode())
        return 0
    if "--check" in argv:
        ok = TABLE_SHA256 is not None and h == TABLE_SHA256
        print(("ok " if ok else "MISMATCH ") + h)
        return 0 if ok else 1
    hist = np.bincount(np.frombuffer(t, np.uint8), minlength=256)
    print("sha256 " + h)
    print("levels: min %d max %d per level (expect 16 each)" % (hist.min(), hist.max()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
