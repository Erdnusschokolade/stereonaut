#!/usr/bin/env python3
"""Compare the planar 10-bit words written by v4l2-vulkan-tp10-sample with
the software-decoded reference (planar yuv420p10le, 1920x1080)."""
import array
import json
import sys

samples = array.array('I')
samples.frombytes(open(sys.argv[1], 'rb').read())
ref = array.array('H')
ref.frombytes(open(sys.argv[2], 'rb').read())
if sys.byteorder != 'little':
    samples.byteswap()
    ref.byteswap()
assert len(samples) == len(ref) == 1920 * 1080 * 3 // 2
n = 1920 * 1080
report = {}
for name, start, end, width in (('y', 0, n, 1920),
                                ('u', n, n + n // 4, 960),
                                ('v', n + n // 4, n * 3 // 2, 960)):
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
