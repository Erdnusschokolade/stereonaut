#!/usr/bin/env python3
"""Compare three tightly packed coded-height NV12 GPU readbacks to I420."""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def compare_plane(actual: bytes, expected: bytes) -> dict:
    left = np.frombuffer(actual, dtype=np.uint8)
    right = np.frombuffer(expected, dtype=np.uint8)
    difference = np.abs(left.astype(np.int16) - right.astype(np.int16))
    indexes = np.flatnonzero(left != right)
    result = {
        "bytes": int(left.size),
        "different_bytes": int(indexes.size),
        "maximum_absolute_difference": int(difference.max(initial=0)),
    }
    if indexes.size:
        first = int(indexes[0])
        result["first_difference"] = {
            "offset": first,
            "actual": int(left[first]),
            "expected": int(right[first]),
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("readback_prefix", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--visible-height", type=int, required=True)
    parser.add_argument("--coded-height", type=int, required=True)
    parser.add_argument("--frames", type=int, nargs=3, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    width = args.width
    visible_height = args.visible_height
    coded_height = args.coded_height
    if width <= 0 or visible_height <= 0 or coded_height < visible_height:
        raise SystemExit("invalid dimensions")
    if width % 2 or visible_height % 2 or coded_height % 2:
        raise SystemExit("4:2:0 dimensions must be even")

    visible_y = width * visible_height
    visible_chroma = width * visible_height // 4
    visible_frame_size = visible_y + 2 * visible_chroma
    coded_y = width * coded_height
    coded_frame_size = coded_y + width * coded_height // 2
    reference = args.reference.read_bytes()
    if len(reference) != 3 * visible_frame_size:
        raise SystemExit(
            f"reference size {len(reference)} != {3 * visible_frame_size}"
        )

    report = {
        "width": width,
        "visible_height": visible_height,
        "coded_height": coded_height,
        "selected_frames": args.frames,
        "reference_sha256": digest(reference),
        "frames": [],
    }
    total_differences = 0
    for slot, frame_index in enumerate(args.frames):
        path = Path(f"{args.readback_prefix}-frame-{frame_index:08d}.nv12")
        readback = path.read_bytes()
        if len(readback) != coded_frame_size:
            raise SystemExit(
                f"{path}: size {len(readback)} != {coded_frame_size}"
            )

        actual_y = readback[:visible_y]
        coded_uv = readback[coded_y : coded_y + width * visible_height // 2]
        actual_u = coded_uv[0::2]
        actual_v = coded_uv[1::2]
        actual_i420 = actual_y + actual_u + actual_v
        expected = reference[
            slot * visible_frame_size : (slot + 1) * visible_frame_size
        ]
        planes = {
            "Y": compare_plane(actual_y, expected[:visible_y]),
            "U": compare_plane(
                actual_u, expected[visible_y : visible_y + visible_chroma]
            ),
            "V": compare_plane(
                actual_v, expected[visible_y + visible_chroma :]
            ),
        }
        differing = sum(item["different_bytes"] for item in planes.values())
        total_differences += differing
        report["frames"].append(
            {
                "frame": frame_index,
                "readback_path": str(path),
                "readback_sha256": digest(readback),
                "visible_i420_sha256": digest(actual_i420),
                "expected_i420_sha256": digest(expected),
                "different_bytes": differing,
                "planes": planes,
            }
        )

    report["total_different_bytes"] = total_differences
    report["exact_match"] = total_differences == 0
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if total_differences == 0 else 1)


if __name__ == "__main__":
    main()
