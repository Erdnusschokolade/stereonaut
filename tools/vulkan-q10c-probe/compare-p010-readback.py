#!/usr/bin/env python3
"""Compare the one-shot GPU P010 readback with the planar 10-bit reference."""
from __future__ import annotations

import array
import json
from pathlib import Path
import sys

WIDTH = 1920
VISIBLE_HEIGHT = 1080
CODED_HEIGHT = 1088
CHROMA_WIDTH = 960
VISIBLE_CHROMA_HEIGHT = 540
CODED_CHROMA_HEIGHT = 544
Y_WORDS = WIDTH * CODED_HEIGHT
UV_WORDS = WIDTH * CODED_CHROMA_HEIGHT
READBACK_BYTES = (Y_WORDS + UV_WORDS) * 2
REFERENCE_BYTES = (WIDTH * VISIBLE_HEIGHT + 2 * CHROMA_WIDTH * VISIBLE_CHROMA_HEIGHT) * 2


def words(path: Path, expected_bytes: int) -> array.array:
    data = path.read_bytes()
    if len(data) != expected_bytes:
        raise SystemExit(f"{path}: expected {expected_bytes} bytes, got {len(data)}")
    values = array.array("H")
    values.frombytes(data)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def compare(name: str, actual: list[int], expected: array.array) -> dict[str, int]:
    if len(actual) != len(expected):
        raise SystemExit(f"{name}: length mismatch {len(actual)} != {len(expected)}")
    differences = [abs(a - b) for a, b in zip(actual, expected)]
    return {
        "samples": len(actual),
        "different_samples": sum(value != 0 for value in differences),
        "max_abs_difference": max(differences, default=0),
    }


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: compare-p010-readback.py p010-readback.bin gradient10-reference.yuv")
    gpu = words(Path(sys.argv[1]), READBACK_BYTES)
    reference = words(Path(sys.argv[2]), REFERENCE_BYTES)

    low_six_nonzero = sum(value & 0x3F != 0 for value in gpu)
    decoded = [value >> 6 for value in gpu]
    actual_y = []
    for row in range(VISIBLE_HEIGHT):
        begin = row * WIDTH
        actual_y.extend(decoded[begin : begin + WIDTH])
    uv_base = Y_WORDS
    actual_u = []
    actual_v = []
    for row in range(VISIBLE_CHROMA_HEIGHT):
        begin = uv_base + row * WIDTH
        row_values = decoded[begin : begin + WIDTH]
        actual_u.extend(row_values[0::2])
        actual_v.extend(row_values[1::2])

    y_count = WIDTH * VISIBLE_HEIGHT
    chroma_count = CHROMA_WIDTH * VISIBLE_CHROMA_HEIGHT
    expected_y = reference[:y_count]
    expected_u = reference[y_count : y_count + chroma_count]
    expected_v = reference[y_count + chroma_count :]
    result = {
        "readback_bytes": READBACK_BYTES,
        "p010_low_six_bits_nonzero": low_six_nonzero,
        "y": compare("Y", actual_y, expected_y),
        "u": compare("U", actual_u, expected_u),
        "v": compare("V", actual_v, expected_v),
        "actual_unique_luma_values": len(set(actual_y)),
        "actual_luma_values_not_divisible_by_4": sum(value % 4 != 0 for value in actual_y),
    }
    result["exact_visible_match"] = (
        low_six_nonzero == 0
        and result["y"]["different_samples"] == 0
        and result["u"]["different_samples"] == 0
        and result["v"]["different_samples"] == 0
    )
    result["more_than_8_bit_precision"] = (
        result["actual_unique_luma_values"] > 256
        and result["actual_luma_values_not_divisible_by_4"] > 0
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["exact_visible_match"] and result["more_than_8_bit_precision"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
