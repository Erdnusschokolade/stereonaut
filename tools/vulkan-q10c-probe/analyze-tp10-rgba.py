#!/usr/bin/env python3
"""Unpack the raw TP10 words read back by v4l2-vulkan-tp10-rgba and compare
them with the software-decoded 10-bit reference (planar yuv420p10le).

TP10 packs three 10-bit samples into one little-endian 32-bit word. The
sample order inside the word is not documented here, so both orders are
tried and reported; the chroma plane interleaves U and V."""
import argparse
import array
import json
import sys
from pathlib import Path

WIDTH, CODED_HEIGHT, HEIGHT = 1920, 1088, 1080
WORDS = WIDTH // 3
Y_BYTES = WORDS * 4 * CODED_HEIGHT

p = argparse.ArgumentParser()
p.add_argument('readback', type=Path)
p.add_argument('reference', type=Path)
p.add_argument('--json', type=Path)
args = p.parse_args()

raw = args.readback.read_bytes()
assert len(raw) == Y_BYTES + WORDS * 4 * (CODED_HEIGHT // 2), len(raw)
words = array.array('I')
words.frombytes(raw)
if sys.byteorder != 'little':
    words.byteswap()
ref = array.array('H')
ref.frombytes(args.reference.read_bytes())
if sys.byteorder != 'little':
    ref.byteswap()
assert len(ref) == WIDTH * HEIGHT * 3 // 2

def unpack(word, order):
    s = (word & 1023, (word >> 10) & 1023, (word >> 20) & 1023)
    return s if order == 'low-first' else s[::-1]

def compare(order):
    result = {}
    # Y
    bad = worst = padding = 0
    first = None
    for y in range(HEIGHT):
        row = y * WORDS
        for w in range(WORDS):
            word = words[row + w]
            padding += (word >> 30) != 0
            for k, value in enumerate(unpack(word, order)):
                x = 3 * w + k
                d = abs(value - ref[y * WIDTH + x])
                if d:
                    bad += 1
                    worst = max(worst, d)
                    if first is None:
                        first = (x, y, ref[y * WIDTH + x], value)
    result['y'] = {'mismatch': bad, 'samples': WIDTH * HEIGHT,
                   'max_abs': worst, 'first': first,
                   'nonzero_padding_words': padding}
    # UV, interleaved U,V
    n = WIDTH * HEIGHT
    u_ref, v_ref = ref[n:n + n // 4], ref[n + n // 4:]
    bad = worst = 0
    first = None
    base = CODED_HEIGHT * WORDS
    for y in range(HEIGHT // 2):
        samples = []
        for w in range(WORDS):
            samples.extend(unpack(words[base + y * WORDS + w], order))
        for x in range(WIDTH // 2):
            for got, want, name in ((samples[2 * x], u_ref, 'u'),
                                    (samples[2 * x + 1], v_ref, 'v')):
                expected = want[y * (WIDTH // 2) + x]
                d = abs(got - expected)
                if d:
                    bad += 1
                    worst = max(worst, d)
                    if first is None:
                        first = (name, x, y, expected, got)
    result['uv'] = {'mismatch': bad, 'samples': n // 2, 'max_abs': worst,
                    'first': first}
    return result

report = {order: compare(order) for order in ('low-first', 'high-first')}
for order, result in report.items():
    print(order, json.dumps(result))
if args.json:
    args.json.write_text(json.dumps(report, indent=2) + '\n')
best = min(report.values(),
           key=lambda r: r['y']['mismatch'] + r['uv']['mismatch'])
sys.exit(0 if best['y']['mismatch'] == 0 and best['uv']['mismatch'] == 0
         else 1)
