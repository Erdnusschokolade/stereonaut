#!/usr/bin/env python3
"""Compare a tightly packed NV12/P010 first-frame dump with its software reference."""
import argparse
import array
import json
from pathlib import Path
import sys

p = argparse.ArgumentParser()
p.add_argument('reference', type=Path)
p.add_argument('hardware', type=Path)
p.add_argument('--format', choices=['NV12', 'P010_10LE'], required=True)
p.add_argument('--reference-depth', type=int, choices=[8, 10], required=True)
p.add_argument('--width', type=int, default=1920)
p.add_argument('--height', type=int, default=1080)
args = p.parse_args()
n = args.width * args.height
count = n * 3 // 2

def words(raw):
    a = array.array('H')
    a.frombytes(raw)
    if sys.byteorder != 'little':
        a.byteswap()
    return a

rb, hb = args.reference.read_bytes(), args.hardware.read_bytes()
assert len(rb) == count * (2 if args.reference_depth == 10 else 1)
assert len(hb) == count * (2 if args.format == 'P010_10LE' else 1)
ref = words(rb) if args.reference_depth == 10 else rb
if args.format == 'P010_10LE':
    packed = words(hb)
    padding_nonzero = sum(v & 63 != 0 for v in packed)
    hw = [v >> 6 for v in packed]
else:
    padding_nonzero = None
    hw = list(hb)
    if args.reference_depth == 10:
        hw = [v * 4 for v in hw]
# Semiplanar UV -> planar Y, U, V, without any color conversion.
hw = hw[:n] + hw[n::2] + hw[n+1::2]
if args.reference_depth == 8 and args.format == 'P010_10LE':
    ref = [v * 4 for v in ref]
diff = [abs(a-b) for a,b in zip(ref, hw)]
result = {
    'format': args.format, 'reference_depth': args.reference_depth,
    'samples': count, 'different_samples': sum(d != 0 for d in diff),
    'max_abs_error_code_values': max(diff), 'mean_abs_error_code_values': sum(diff)/count,
    'hardware_unique_luma': len(set(hw[:n])),
    'hardware_luma_low_two_bits_nonzero': sum(v % 4 != 0 for v in hw[:n]),
    'p010_padding_nonzero_samples': padding_nonzero,
    'exact_software_reference_match': not any(diff),
    'ten_bit_precision_proven_for_this_frame': args.reference_depth == 10
        and args.format == 'P010_10LE' and not any(diff)
        and len(set(hw[:n])) > 256 and any(v % 4 for v in hw[:n]),
}
print(json.dumps(result, indent=2))
