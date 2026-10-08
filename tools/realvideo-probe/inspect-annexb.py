#!/usr/bin/env python3
"""Count AUD-delimited access units without decoding the HEVC payload."""

import argparse
import json
import mmap
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    args = parser.parse_args()

    starts: list[int] = []
    with args.input.open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            position = 0
            while True:
                found = data.find(b"\x00\x00\x01", position)
                if found < 0:
                    break
                start = found - 1 if found > 0 and data[found - 1] == 0 else found
                prefix = 4 if start != found else 3
                nal_offset = start + prefix
                if nal_offset < len(data) and ((data[nal_offset] >> 1) & 63) == 35:
                    starts.append(start)
                position = nal_offset + 2
            if not starts:
                raise SystemExit("no AUD NAL units found")
            starts[0] = 0
            sizes = [
                (starts[index + 1] if index + 1 < len(starts) else len(data))
                - offset
                for index, offset in enumerate(starts)
            ]
            report = {
                "bytes": len(data),
                "access_units": len(starts),
                "largest_access_unit": max(sizes),
                "first_access_unit_bytes": sizes[0],
                "last_access_unit_bytes": sizes[-1],
            }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
