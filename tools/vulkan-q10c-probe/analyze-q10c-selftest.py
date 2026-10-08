#!/usr/bin/env python3
"""Offline analysis for the fixed 1920x1088 Q10C Vulkan self-test pattern."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


WIDTH = 1920
HEIGHT = 1088
CHROMA_WIDTH = 960
CHROMA_HEIGHT = 544
Y_WORDS = WIDTH * HEIGHT
TOTAL_WORDS = Y_WORDS + 2 * CHROMA_WIDTH * CHROMA_HEIGHT


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def synthetic_words() -> np.ndarray:
    x = np.arange(WIDTH, dtype=np.uint32)[None, :]
    y = np.arange(HEIGHT, dtype=np.uint32)[:, None]
    plane_y = ((x * 13 + y * 29 + (x ^ y) * 3) & 1023).astype(np.uint16) << 6
    x_uv = np.arange(CHROMA_WIDTH, dtype=np.uint32)[None, :]
    y_uv = np.arange(CHROMA_HEIGHT, dtype=np.uint32)[:, None]
    plane_u = ((x_uv * 17 + y_uv * 7 + 113) & 1023).astype(np.uint16) << 6
    plane_v = ((x_uv * 5 + y_uv * 19 + 509) & 1023).astype(np.uint16) << 6
    uv = np.empty((CHROMA_HEIGHT, WIDTH), dtype=np.uint16)
    uv[:, 0::2] = plane_u
    uv[:, 1::2] = plane_v
    return np.concatenate((plane_y.ravel(), uv.ravel()))


def split(words: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    y = words[:Y_WORDS].reshape(HEIGHT, WIDTH)
    uv = words[Y_WORDS:].reshape(CHROMA_HEIGHT, WIDTH)
    return y, uv[:, 0::2], uv[:, 1::2]


def comparison(baseline: np.ndarray, candidate: np.ndarray) -> dict[str, object]:
    difference = candidate.astype(np.int32) - baseline.astype(np.int32)
    changed = difference != 0
    positions = np.argwhere(changed)
    first = None
    if positions.size:
        y, x = (int(item) for item in positions[0])
        first = {
            "x": x,
            "y": y,
            "baseline_raw": int(baseline[y, x]),
            "candidate_raw": int(candidate[y, x]),
            "baseline_10bit": int(baseline[y, x] >> 6),
            "candidate_10bit": int(candidate[y, x] >> 6),
            "absolute_raw_difference": int(abs(difference[y, x])),
        }
    return {
        "samples": int(baseline.size),
        "different_16bit_words": int(np.count_nonzero(changed)),
        "maximum_absolute_raw_difference": int(np.abs(difference).max(initial=0)),
        "mae_raw": float(np.abs(difference).mean()),
        "first_different_sample": first,
        "different_10bit_samples": int(np.count_nonzero((baseline >> 6) != (candidate >> 6))),
    }


def compare_all(baseline: np.ndarray, candidate: np.ndarray) -> dict[str, object]:
    names = ("y", "u", "v")
    return {
        name: comparison(first, second)
        for name, first, second in zip(names, split(baseline), split(candidate))
    }


def save_y_difference(path: Path, baseline: np.ndarray, candidate: np.ndarray) -> None:
    baseline_y = split(baseline)[0].astype(np.int32) >> 6
    candidate_y = split(candidate)[0].astype(np.int32) >> 6
    difference = np.abs(candidate_y - baseline_y)
    pixels = np.clip(np.rint(difference * 255.0 / 1023.0), 0, 255).astype(np.uint8)
    Image.fromarray(pixels, "L").save(path)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: analyze-q10c-selftest.py SOURCE READBACK OUTPUT_DIR")
    source_path, readback_path, output = map(Path, sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    source_data = source_path.read_bytes()
    readback_data = readback_path.read_bytes()
    expected = synthetic_words()
    expected_data = expected.astype("<u2", copy=False).tobytes()
    expected_size = TOTAL_WORDS * 2
    if len(source_data) != expected_size or len(readback_data) != expected_size:
        raise SystemExit(
            f"expected two {expected_size}-byte files; got {len(source_data)} and {len(readback_data)}"
        )
    source = np.frombuffer(source_data, dtype="<u2")
    readback = np.frombuffer(readback_data, dtype="<u2")
    result = {
        "dimensions": {"width": WIDTH, "height": HEIGHT, "chroma_width": CHROMA_WIDTH, "chroma_height": CHROMA_HEIGHT},
        "intended_synthetic_input": {
            "bytes": len(expected_data),
            "sha256": sha256(expected_data),
            "low_six_bits_nonzero": int(np.count_nonzero(expected & 63)),
        },
        "captured_source_after_fence": {
            "bytes": len(source_data),
            "sha256": sha256(source_data),
            "low_six_bits_nonzero": int(np.count_nonzero(source & 63)),
        },
        "captured_readback_after_fence": {
            "bytes": len(readback_data),
            "sha256": sha256(readback_data),
            "low_six_bits_nonzero": int(np.count_nonzero(readback & 63)),
        },
        "intended_vs_captured_source": compare_all(expected, source),
        "intended_vs_readback": compare_all(expected, readback),
        "captured_source_vs_readback": compare_all(source, readback),
    }
    save_y_difference(output / "intended-vs-source-y-difference.png", expected, source)
    save_y_difference(output / "intended-vs-readback-y-difference.png", expected, readback)
    save_y_difference(output / "source-vs-readback-y-difference.png", source, readback)
    (output / "selftest-analysis.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
