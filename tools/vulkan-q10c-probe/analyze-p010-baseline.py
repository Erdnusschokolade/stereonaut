#!/usr/bin/env python3
"""Offline spatial/source-integrity analysis of the fixed P010 baseline."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--previous", type=Path)
    args = parser.parse_args()
    paths = [args.directory / f"copy-{name}.bin"
             for name in ("source-before", "source-after", "readback")]
    data = [path.read_bytes() for path in paths]
    if any(len(blob) != 6266880 for blob in data):
        raise SystemExit("expected three 1920x1088 P010 files of 6266880 bytes")
    before, after, readback = [np.frombuffer(blob, dtype="<u2") for blob in data]
    y_words = 1920 * 1088
    changed = (before >> 6) != (readback >> 6)
    y_mask = changed[:y_words].reshape(1088, 1920)
    ys, xs = np.where(y_mask)
    blocks = np.unique(np.column_stack((ys // 16, xs // 32)), axis=0)
    rows, counts = np.unique(ys, return_counts=True)
    result = {
        "source_before_after_equal": data[0] == data[1],
        "source_changed_words": int(np.count_nonzero(before != after)),
        "different_10bit_samples": {
            "y": int(changed[:y_words].sum()),
            "u": int(changed[y_words::2].sum()),
            "v": int(changed[y_words+1::2].sum()),
        },
        "y_bbox": {"x_min": int(xs.min()), "x_max": int(xs.max()),
                   "y_min": int(ys.min()), "y_max": int(ys.max())} if xs.size else None,
        "changed_by_row": [[int(y), int(n)] for y, n in zip(rows, counts)],
        "affected_32x16_blocks_xy": [[int(x), int(y)] for y, x in blocks],
        "hashes": {path.name: hashlib.sha256(blob).hexdigest()
                   for path, blob in zip(paths, data)},
        "first_32_readback_y": (readback[:32] >> 6).tolist(),
        "first_32_expected_y": (before[:32] >> 6).tolist(),
    }
    if args.previous:
        previous_data = args.previous.read_bytes()
        if len(previous_data) != len(data[0]):
            raise SystemExit("previous readback size differs")
        previous = np.frombuffer(previous_data, dtype="<u2")
        result["previous_readback_comparison"] = {
            "sha256": hashlib.sha256(previous_data).hexdigest(),
            "current_bad_samples_equal_previous_raw": int(
                np.count_nonzero(changed & (previous == readback))),
            "current_bad_samples": int(changed.sum()),
        }
    output = args.directory / "copy-spatial-analysis.json"
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
