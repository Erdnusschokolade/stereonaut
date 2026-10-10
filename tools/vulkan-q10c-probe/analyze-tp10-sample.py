#!/usr/bin/env python3
"""Compare the planar 10-bit words written by v4l2-vulkan-tp10-sample with
the software-decoded reference (planar yuv420p10le).
usage: analyze-tp10-sample.py SAMPLES REFERENCE [JSON] [--size WxH]"""
import array
import json
import sys

args = [a for a in sys.argv[1:] if not a.startswith('--size')]
size = [a for a in sys.argv[1:] if a.startswith('--size')]
W, H = (map(int, size[0].split('=')[1].split('x')) if size else (1920, 1080))
sys.argv = [sys.argv[0]] + args

samples = array.array('I')
samples.frombytes(open(sys.argv[1], 'rb').read())
ref = array.array('H')
ref.frombytes(open(sys.argv[2], 'rb').read())
if sys.byteorder != 'little':
    samples.byteswap()
    ref.byteswap()
assert len(samples) == len(ref) == W * H * 3 // 2, (len(samples), len(ref))
n = W * H
report = {}
for name, start, end, width in (('y', 0, n, W),
                                ('u', n, n + n // 4, W // 2),
                                ('v', n + n // 4, n * 3 // 2, W // 2)):
    bad = worst = 0
    first = None
    for i in range(start, end):
        d = abs(samples[i] - ref[i])
        if d:
            bad += 1
            worst = max(worst, d)
            if first is None:
                k = i - start
                first = [k % width, k // width, ref[i], samples[i]]
    report[name] = {'mismatch': bad, 'samples': end - start,
                    'max_abs': worst, 'first': first}
print(json.dumps(report))
if len(sys.argv) > 3:
    open(sys.argv[3], 'w').write(json.dumps(report, indent=2) + '\n')
sys.exit(0 if all(r['mismatch'] == 0 for r in report.values()) else 1)
