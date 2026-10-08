#!/usr/bin/env python3
"""Bit-exact and plane-aware comparison of two 1920x1088 P010 readbacks."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


WIDTH = 1920
HEIGHT = 1080
CODED_HEIGHT = 1088
CHROMA_HEIGHT = CODED_HEIGHT // 2
Y_WORDS = WIDTH * CODED_HEIGHT
TOTAL_WORDS = Y_WORDS + WIDTH * CHROMA_HEIGHT


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def planes(words: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    values = words >> 6
    y = values[:Y_WORDS].reshape(CODED_HEIGHT, WIDTH)[:HEIGHT]
    uv = values[Y_WORDS:].reshape(CHROMA_HEIGHT, WIDTH)[: HEIGHT // 2]
    return y, uv[:, 0::2], uv[:, 1::2]


def plane_metrics(baseline: np.ndarray, candidate: np.ndarray) -> dict[str, object]:
    difference = candidate.astype(np.int16) - baseline.astype(np.int16)
    changed = difference != 0
    positions = np.argwhere(changed)
    bounding_box = None
    first_difference = None
    if positions.size:
        bounding_box = {
            "x_min": int(positions[:, 1].min()),
            "x_max": int(positions[:, 1].max()),
            "y_min": int(positions[:, 0].min()),
            "y_max": int(positions[:, 0].max()),
        }
        y, x = (int(value) for value in positions[0])
        first_difference = {
            "x": x,
            "y": y,
            "baseline": int(baseline[y, x]),
            "candidate": int(candidate[y, x]),
            "absolute_difference": int(abs(difference[y, x])),
        }
    base64 = baseline.astype(np.float64)
    candidate64 = candidate.astype(np.float64)
    correlation = float(np.corrcoef(base64.ravel(), candidate64.ravel())[0, 1])
    return {
        "samples": int(baseline.size),
        "different_samples": int(np.count_nonzero(changed)),
        "different_fraction": float(np.mean(changed)),
        "maximum_absolute_difference": int(np.abs(difference).max(initial=0)),
        "mae": float(np.abs(difference).mean()),
        "rmse": float(np.sqrt(np.mean(difference.astype(np.float64) ** 2))),
        "correlation": correlation,
        "changed_bounding_box": bounding_box,
        "first_different_sample": first_difference,
    }


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: compare-p010-readbacks.py BASELINE CANDIDATE OUTPUT_JSON OUTPUT_PNG"
        )
    baseline_path, candidate_path, output_json, output_png = map(Path, sys.argv[1:])
    baseline_bytes = baseline_path.read_bytes()
    candidate_bytes = candidate_path.read_bytes()
    expected_bytes = TOTAL_WORDS * 2
    if len(baseline_bytes) != expected_bytes or len(candidate_bytes) != expected_bytes:
        raise SystemExit(
            f"expected two {expected_bytes}-byte readbacks; got "
            f"{len(baseline_bytes)} and {len(candidate_bytes)}"
        )

    baseline_words = np.frombuffer(baseline_bytes, dtype="<u2")
    candidate_words = np.frombuffer(candidate_bytes, dtype="<u2")
    word_difference = candidate_words.astype(np.int32) - baseline_words.astype(np.int32)
    byte_changed = np.frombuffer(baseline_bytes, dtype=np.uint8) != np.frombuffer(
        candidate_bytes, dtype=np.uint8
    )
    baseline_planes = planes(baseline_words)
    candidate_planes = planes(candidate_words)
    names = ("y", "u", "v")
    result = {
        "baseline": {
            "path": str(baseline_path),
            "bytes": len(baseline_bytes),
            "sha256": digest(baseline_bytes),
            "low_six_bits_nonzero": int(np.count_nonzero(baseline_words & 0x3F)),
        },
        "candidate": {
            "path": str(candidate_path),
            "bytes": len(candidate_bytes),
            "sha256": digest(candidate_bytes),
            "low_six_bits_nonzero": int(np.count_nonzero(candidate_words & 0x3F)),
        },
        "bit_exact_equal": baseline_bytes == candidate_bytes,
        "different_bytes": int(np.count_nonzero(byte_changed)),
        "different_words": int(np.count_nonzero(word_difference)),
        "maximum_absolute_raw_word_difference": int(
            np.abs(word_difference).max(initial=0)
        ),
        "first_different_byte": int(np.flatnonzero(byte_changed)[0])
        if np.any(byte_changed)
        else None,
        "planes_high_10_bits": {
            name: plane_metrics(baseline, candidate)
            for name, baseline, candidate in zip(
                names, baseline_planes, candidate_planes
            )
        },
    }

    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    y_difference = np.abs(
        candidate_planes[0].astype(np.int16) - baseline_planes[0].astype(np.int16)
    )
    image = np.clip(np.rint(y_difference.astype(np.float32) * 255.0 / 1023.0), 0, 255)
    Image.fromarray(image.astype(np.uint8), "L").save(output_png)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
