#!/usr/bin/env python3
"""Bounded offline analysis of the run-04 Q10C/P010 GPU readback."""
from __future__ import annotations

import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

os.environ.setdefault("MPLCONFIGDIR", "/tmp/stereonaut-matplotlib")

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

try:
    from scipy.optimize import linear_sum_assignment
except ImportError:  # The rest of the analysis remains useful without SciPy.
    linear_sum_assignment = None


WIDTH = 1920
HEIGHT = 1080
CODED_HEIGHT = 1088
CHROMA_WIDTH = WIDTH // 2
CHROMA_HEIGHT = HEIGHT // 2
CODED_CHROMA_HEIGHT = CODED_HEIGHT // 2
Y_CODED_WORDS = WIDTH * CODED_HEIGHT
UV_CODED_WORDS = WIDTH * CODED_CHROMA_HEIGHT
READBACK_WORDS = Y_CODED_WORDS + UV_CODED_WORDS
FRAME_WORDS = WIDTH * HEIGHT + 2 * CHROMA_WIDTH * CHROMA_HEIGHT
FRAME_BYTES = FRAME_WORDS * 2


def metrics(actual: np.ndarray, reference: np.ndarray) -> dict[str, float | int]:
    a = actual.astype(np.float64, copy=False).ravel()
    r = reference.astype(np.float64, copy=False).ravel()
    if a.size != r.size:
        raise ValueError(f"metric shape mismatch: {actual.shape} != {reference.shape}")
    difference = a - r
    abs_difference = np.abs(difference)
    a_std = float(a.std())
    r_std = float(r.std())
    correlation = float(np.corrcoef(a, r)[0, 1]) if a_std and r_std else float("nan")
    return {
        "samples": int(a.size),
        "different_samples": int(np.count_nonzero(difference)),
        "max_abs_difference": int(abs_difference.max(initial=0)),
        "mae": float(abs_difference.mean()),
        "rmse": float(np.sqrt(np.mean(difference * difference))),
        "correlation": correlation,
    }


def read_gpu(path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    words = np.fromfile(path, dtype="<u2")
    if words.size != READBACK_WORDS:
        raise SystemExit(f"{path}: expected {READBACK_WORDS * 2} bytes, got {words.nbytes}")
    values = (words >> 6).astype(np.uint16)
    y = values[:Y_CODED_WORDS].reshape(CODED_HEIGHT, WIDTH)
    uv = values[Y_CODED_WORDS:].reshape(CODED_CHROMA_HEIGHT, WIDTH)
    return words, y[:HEIGHT], uv[:CHROMA_HEIGHT, 0::2], uv[:CHROMA_HEIGHT, 1::2]


def parser_self_check(reference: tuple[np.ndarray, np.ndarray, np.ndarray]) -> dict[str, object]:
    """Round-trip a known planar frame through the expected coded P010 layout."""
    y, u, v = reference
    coded_y = np.zeros((CODED_HEIGHT, WIDTH), dtype="<u2")
    coded_uv = np.zeros((CODED_CHROMA_HEIGHT, WIDTH), dtype="<u2")
    coded_y[:HEIGHT] = y << 6
    coded_uv[:CHROMA_HEIGHT, 0::2] = u << 6
    coded_uv[:CHROMA_HEIGHT, 1::2] = v << 6
    serialized = np.concatenate((coded_y.ravel(), coded_uv.ravel())).tobytes()
    parsed = np.frombuffer(serialized, dtype="<u2") >> 6
    parsed_y = parsed[:Y_CODED_WORDS].reshape(CODED_HEIGHT, WIDTH)[:HEIGHT]
    parsed_uv = parsed[Y_CODED_WORDS:].reshape(CODED_CHROMA_HEIGHT, WIDTH)[:CHROMA_HEIGHT]
    results = {
        "serialized_bytes": len(serialized),
        "y": metrics(parsed_y, y),
        "u": metrics(parsed_uv[:, 0::2], u),
        "v": metrics(parsed_uv[:, 1::2], v),
    }
    results["exact"] = all(results[plane]["different_samples"] == 0 for plane in ("y", "u", "v"))
    return results


def decode_all_frames(path: Path) -> tuple[list[dict[str, object]], list[tuple[np.ndarray, np.ndarray, np.ndarray]]]:
    command = [
        "ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo",
        "-pix_fmt", "yuv420p10le", "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    assert process.stdout is not None
    frames: list[tuple[np.ndarray, np.ndarray, np.ndarray]] = []
    identities: list[dict[str, object]] = []
    for index in range(60):
        data = process.stdout.read(FRAME_BYTES)
        if len(data) != FRAME_BYTES:
            process.kill()
            raise SystemExit(f"ffmpeg returned a short frame {index}: {len(data)} bytes")
        words = np.frombuffer(data, dtype="<u2")
        y_end = WIDTH * HEIGHT
        c_words = CHROMA_WIDTH * CHROMA_HEIGHT
        y = words[:y_end].reshape(HEIGHT, WIDTH).copy()
        u = words[y_end:y_end + c_words].reshape(CHROMA_HEIGHT, CHROMA_WIDTH).copy()
        v = words[y_end + c_words:].reshape(CHROMA_HEIGHT, CHROMA_WIDTH).copy()
        frames.append((y, u, v))
        identities.append({
            "frame": index,
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    extra = process.stdout.read(1)
    return_code = process.wait()
    if return_code != 0 or extra:
        raise SystemExit(f"ffmpeg decode did not end cleanly: rc={return_code}, extra={len(extra)}")
    return identities, frames


def compare_frames(gpu: tuple[np.ndarray, np.ndarray, np.ndarray],
                   frames: list[tuple[np.ndarray, np.ndarray, np.ndarray]],
                   output: Path) -> tuple[int, list[dict[str, object]]]:
    rows: list[dict[str, object]] = []
    for index, frame in enumerate(frames):
        plane_metrics = [metrics(a, r) for a, r in zip(gpu, frame)]
        total_squared = sum(m["rmse"] ** 2 * m["samples"] for m in plane_metrics)
        total_samples = sum(m["samples"] for m in plane_metrics)
        row: dict[str, object] = {
            "frame": index,
            "overall_rmse": math.sqrt(total_squared / total_samples),
        }
        for name, result in zip(("y", "u", "v"), plane_metrics):
            for key, value in result.items():
                row[f"{name}_{key}"] = value
        rows.append(row)
    fields = list(rows[0])
    with output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    best = min(rows, key=lambda row: float(row["overall_rmse"]))
    return int(best["frame"]), rows


def aligned_views(actual: np.ndarray, reference: np.ndarray, dx: int, dy: int,
                  horizontal_flip: bool, vertical_flip: bool) -> tuple[np.ndarray, np.ndarray]:
    transformed = reference
    if horizontal_flip:
        transformed = transformed[:, ::-1]
    if vertical_flip:
        transformed = transformed[::-1, :]
    height, width = actual.shape
    ax0, ax1 = max(dx, 0), min(width + dx, width)
    rx0, rx1 = max(-dx, 0), min(width - dx, width)
    ay0, ay1 = max(dy, 0), min(height + dy, height)
    ry0, ry1 = max(-dy, 0), min(height - dy, height)
    return actual[ay0:ay1, ax0:ax1], transformed[ry0:ry1, rx0:rx1]


def geometric_search(actual: np.ndarray, reference: np.ndarray) -> dict[str, object]:
    coarse: list[dict[str, object]] = []
    for hflip in (False, True):
        for vflip in (False, True):
            for dy in range(-16, 17, 2):
                for dx in range(-128, 129, 4):
                    a, r = aligned_views(actual, reference, dx, dy, hflip, vflip)
                    result = metrics(a[::8, ::8], r[::8, ::8])
                    coarse.append({
                        "dx": dx, "dy": dy,
                        "horizontal_flip": hflip, "vertical_flip": vflip,
                        "correlation": result["correlation"], "mae": result["mae"],
                    })
    seeds = sorted(coarse, key=lambda item: (-float(item["correlation"]), float(item["mae"])))[:8]
    candidates: dict[tuple[int, int, bool, bool], dict[str, object]] = {}
    for seed in seeds:
        for dy in range(int(seed["dy"]) - 2, int(seed["dy"]) + 3):
            for dx in range(int(seed["dx"]) - 4, int(seed["dx"]) + 5):
                if not -16 <= dy <= 16 or not -128 <= dx <= 128:
                    continue
                key = (dx, dy, bool(seed["horizontal_flip"]), bool(seed["vertical_flip"]))
                if key in candidates:
                    continue
                a, r = aligned_views(actual, reference, *key)
                result = metrics(a[::4, ::4], r[::4, ::4])
                candidates[key] = {
                    "dx": dx, "dy": dy,
                    "horizontal_flip": key[2], "vertical_flip": key[3],
                    "sampled_correlation": result["correlation"],
                    "sampled_mae": result["mae"],
                }
    top = sorted(candidates.values(),
                 key=lambda item: (-float(item["sampled_correlation"]), float(item["sampled_mae"])))[:10]
    for item in top:
        a, r = aligned_views(actual, reference, int(item["dx"]), int(item["dy"]),
                             bool(item["horizontal_flip"]), bool(item["vertical_flip"]))
        item["full_metrics"] = metrics(a, r)
        item["overlap_shape"] = list(a.shape)
    return {
        "bounds": {"dx": [-128, 128], "dy": [-16, 16]},
        "coarse_steps": {"dx": 4, "dy": 2, "sample": 8},
        "refine_steps": {"dx": 1, "dy": 1, "sample": 4},
        "top_candidates": top,
    }


def row_pitch_search(words: np.ndarray, reference: np.ndarray) -> list[dict[str, object]]:
    candidates = list(range(1920, 1937)) + [2048, 2560]
    results: list[dict[str, object]] = []
    values = (words >> 6).astype(np.uint16)
    for pitch_words in candidates:
        for row_offset in range(0, 9):
            last = (row_offset + HEIGHT - 1) * pitch_words + WIDTH
            if last > values.size:
                continue
            rows = np.lib.stride_tricks.as_strided(
                values[row_offset * pitch_words:],
                shape=(HEIGHT, WIDTH),
                strides=(pitch_words * values.itemsize, values.itemsize),
                writeable=False,
            )
            result = metrics(rows[::4, ::4], reference[::4, ::4])
            results.append({
                "pitch_words": pitch_words,
                "pitch_bytes": pitch_words * 2,
                "row_offset": row_offset,
                "sampled_correlation": result["correlation"],
                "sampled_mae": result["mae"],
            })
    return sorted(results, key=lambda item: (-float(item["sampled_correlation"]), float(item["sampled_mae"])))[:12]


def reverse_ten_bits(values: np.ndarray) -> np.ndarray:
    result = np.zeros_like(values, dtype=np.uint16)
    for bit in range(10):
        result |= ((values >> bit) & 1) << (9 - bit)
    return result


def transformation_metrics(name: str, actual: np.ndarray, expected: np.ndarray) -> dict[str, object]:
    return {"name": name, **metrics(actual, expected)}


def value_transformations(raw_words: np.ndarray, actual: np.ndarray,
                          reference: np.ndarray) -> dict[str, object]:
    raw_y = raw_words[:Y_CODED_WORDS].reshape(CODED_HEIGHT, WIDTH)[:HEIGHT]
    swapped = raw_y.byteswap()
    interpretations = {
        "high_10_bits": raw_y >> 6,
        "low_10_bits": raw_y & 1023,
        "byteswap_high_10_bits": swapped >> 6,
        "byteswap_low_10_bits": swapped & 1023,
        "reverse_high_10_bits": reverse_ten_bits(raw_y >> 6),
    }
    interpretation_results = [
        transformation_metrics(name, values, reference)
        for name, values in interpretations.items()
    ]
    for rotation in range(1, 10):
        values = ((actual << rotation) | (actual >> (10 - rotation))) & 1023
        interpretation_results.append(
            transformation_metrics(f"rotate_10_bits_left_{rotation}", values, reference)
        )

    ref64 = reference.astype(np.float64)
    act64 = actual.astype(np.float64)
    offset = float(np.mean(act64 - ref64))
    median_offset = float(np.median(act64 - ref64))
    slope, intercept = np.polyfit(ref64.ravel(), act64.ravel(), 1)
    predicted = np.clip(np.rint(slope * ref64 + intercept), 0, 1023).astype(np.uint16)
    ss_res = float(np.sum((act64 - (slope * ref64 + intercept)) ** 2))
    ss_tot = float(np.sum((act64 - act64.mean()) ** 2))
    mappings = [
        transformation_metrics("actual_equals_reference", actual, reference),
        transformation_metrics("actual_equals_1023_minus_reference", actual, 1023 - reference),
        transformation_metrics("reference_plus_mean_offset", actual,
                               np.clip(np.rint(ref64 + offset), 0, 1023).astype(np.uint16)),
        transformation_metrics("reference_plus_median_offset", actual,
                               np.clip(np.rint(ref64 + median_offset), 0, 1023).astype(np.uint16)),
        transformation_metrics("linear_regression_actual_from_reference", actual, predicted),
        transformation_metrics(
            "reference_limited_to_full",
            actual,
            np.clip(np.rint((ref64 - 64.0) * 1023.0 / 876.0), 0, 1023).astype(np.uint16),
        ),
        transformation_metrics(
            "reference_full_to_limited",
            actual,
            np.clip(np.rint(ref64 * 876.0 / 1023.0 + 64.0), 0, 1023).astype(np.uint16),
        ),
    ]

    for shift in range(1, 5):
        mappings.append(transformation_metrics(
            f"actual_right_shift_{shift}", actual >> shift, reference
        ))
        mappings.append(transformation_metrics(
            f"actual_left_shift_{shift}_clipped",
            np.clip(actual.astype(np.uint32) << shift, 0, 1023).astype(np.uint16),
            reference,
        ))

    bit_permutation: dict[str, object] = {"available": linear_sum_assignment is not None}
    if linear_sum_assignment is not None:
        sampled_actual = actual[::4, ::4].ravel()
        sampled_reference = reference[::4, ::4].ravel()
        agreement = np.empty((10, 10), dtype=np.float64)
        for source_bit in range(10):
            source = (sampled_actual >> source_bit) & 1
            for destination_bit in range(10):
                destination = (sampled_reference >> destination_bit) & 1
                agreement[source_bit, destination_bit] = np.mean(source == destination)
        source_bits, destination_bits = linear_sum_assignment(-agreement)
        permuted = np.zeros_like(actual)
        assignment = []
        for source_bit, destination_bit in zip(source_bits, destination_bits):
            source_index = int(source_bit)
            destination_index = int(destination_bit)
            permuted |= (((actual >> source_index) & 1) << destination_index).astype(np.uint16)
            assignment.append({
                "source_actual_bit": source_index,
                "destination_reference_bit": destination_index,
                "sampled_agreement": float(agreement[source_index, destination_index]),
            })
        bit_permutation.update({
            "assignment": assignment,
            "metrics": metrics(permuted, reference),
            "agreement_matrix": agreement.tolist(),
        })

    return {
        "raw_interpretations": sorted(interpretation_results, key=lambda item: float(item["rmse"])),
        "mapping_hypotheses": sorted(mappings, key=lambda item: float(item["rmse"])),
        "regression": {
            "actual_equals_slope_times_reference_plus_intercept": {
                "slope": float(slope), "intercept": float(intercept),
                "r_squared": 1.0 - ss_res / ss_tot if ss_tot else float("nan"),
            },
            "mean_offset": offset,
            "median_offset": median_offset,
        },
        "best_bit_permutation": bit_permutation,
    }


def shift_search(actual: np.ndarray, reference: np.ndarray, x_bound: int,
                 y_bound: int) -> dict[str, object]:
    candidates = []
    for dy in range(-y_bound, y_bound + 1):
        for dx in range(-x_bound, x_bound + 1):
            a, r = aligned_views(actual, reference, dx, dy, False, False)
            result = metrics(a[::2, ::2], r[::2, ::2])
            candidates.append({
                "dx": dx, "dy": dy,
                "sampled_correlation": result["correlation"],
                "sampled_mae": result["mae"],
            })
    top = sorted(candidates,
                 key=lambda item: (-float(item["sampled_correlation"]), float(item["sampled_mae"])))[:5]
    for item in top:
        a, r = aligned_views(actual, reference, int(item["dx"]), int(item["dy"]), False, False)
        item["full_metrics"] = metrics(a, r)
    return {"bounds": {"dx": [-x_bound, x_bound], "dy": [-y_bound, y_bound]},
            "top_candidates": top}


def chroma_analysis(raw_words: np.ndarray, u: np.ndarray, v: np.ndarray,
                    ref_u: np.ndarray, ref_v: np.ndarray) -> dict[str, object]:
    uv_words = (raw_words[Y_CODED_WORDS:] >> 6).astype(np.uint16)
    half = CHROMA_WIDTH * CODED_CHROMA_HEIGHT
    planar_u = uv_words[:half].reshape(CODED_CHROMA_HEIGHT, CHROMA_WIDTH)[:CHROMA_HEIGHT]
    planar_v = uv_words[half:].reshape(CODED_CHROMA_HEIGHT, CHROMA_WIDTH)[:CHROMA_HEIGHT]
    variants = {
        "interleaved_uv": (u, v),
        "interleaved_vu": (v, u),
        "planar_halves_uv": (planar_u, planar_v),
        "planar_halves_vu": (planar_v, planar_u),
    }
    variant_results = []
    for name, (candidate_u, candidate_v) in variants.items():
        variant_results.append({
            "name": name,
            "u": metrics(candidate_u, ref_u),
            "v": metrics(candidate_v, ref_v),
            "combined_rmse": math.sqrt((
                metrics(candidate_u, ref_u)["rmse"] ** 2 +
                metrics(candidate_v, ref_v)["rmse"] ** 2
            ) / 2.0),
        })
    return {
        "component_and_deinterleave_variants": sorted(
            variant_results, key=lambda item: float(item["combined_rmse"])
        ),
        "u_shift_search": shift_search(u, ref_u, 8, 4),
        "v_shift_search": shift_search(v, ref_v, 8, 4),
        "swapped_u_against_reference_u_shift_search": shift_search(v, ref_u, 8, 4),
        "swapped_v_against_reference_v_shift_search": shift_search(u, ref_v, 8, 4),
    }


def yuv_to_rgb(y: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    up_u = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)[:HEIGHT, :WIDTH]
    up_v = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)[:HEIGHT, :WIDTH]
    yf = (y.astype(np.float32) - 64.0) / 876.0
    uf = (up_u.astype(np.float32) - 512.0) / 896.0
    vf = (up_v.astype(np.float32) - 512.0) / 896.0
    red = yf + 1.5748 * vf
    green = yf - 0.1873 * uf - 0.4681 * vf
    blue = yf + 1.8556 * uf
    rgb = np.stack((red, green, blue), axis=-1)
    return np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)


def save_images(output: Path, gpu: tuple[np.ndarray, np.ndarray, np.ndarray],
                reference: tuple[np.ndarray, np.ndarray, np.ndarray], best_frame: int) -> None:
    Image.fromarray(yuv_to_rgb(*reference), "RGB").save(output / f"reference-frame-{best_frame:02d}.png")
    Image.fromarray(yuv_to_rgb(*gpu), "RGB").save(output / "gpu-readback.png")
    signed = gpu[0].astype(np.int16) - reference[0].astype(np.int16)
    absolute = np.abs(signed)
    gray = np.clip(np.rint(absolute.astype(np.float32) * 255.0 / 1023.0), 0, 255).astype(np.uint8)
    Image.fromarray(gray, "L").save(output / "y-absolute-difference.png")
    false_color = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    positive = np.maximum(signed.astype(np.int32), 0)
    negative = np.maximum(-signed.astype(np.int32), 0)
    false_color[..., 0] = np.clip(positive * 255 // 1023, 0, 255).astype(np.uint8)
    false_color[..., 2] = np.clip(negative * 255 // 1023, 0, 255).astype(np.uint8)
    Image.fromarray(false_color, "RGB").save(output / "y-difference-false-color.png")


def save_histograms(output: Path, actual: np.ndarray, reference: np.ndarray) -> dict[str, object]:
    actual_hist = np.bincount(actual.ravel(), minlength=1024)
    reference_hist = np.bincount(reference.ravel(), minlength=1024)
    difference = actual.astype(np.int16) - reference.astype(np.int16)
    difference_hist = np.bincount((difference.ravel() + 1023), minlength=2047)
    with (output / "luma-histogram.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("value", "gpu_count", "reference_count"))
        writer.writerows((value, int(actual_hist[value]), int(reference_hist[value])) for value in range(1024))
    with (output / "luma-difference-histogram.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("gpu_minus_reference", "count"))
        writer.writerows((value - 1023, int(difference_hist[value])) for value in range(2047))

    figure, axis = plt.subplots(figsize=(10, 4))
    axis.plot(actual_hist, label="GPU readback", linewidth=1)
    axis.plot(reference_hist, label="software reference", linewidth=1)
    axis.set(xlabel="10-bit luma value", ylabel="sample count", title="Luma histograms")
    axis.legend()
    figure.tight_layout()
    figure.savefig(output / "luma-histogram.png", dpi=140)
    plt.close(figure)

    figure, axis = plt.subplots(figsize=(10, 4))
    values = np.arange(-1023, 1024)
    axis.plot(values, difference_hist, linewidth=1)
    axis.set(xlabel="GPU minus reference", ylabel="sample count", title="Luma difference histogram")
    axis.set_yscale("symlog", linthresh=1)
    figure.tight_layout()
    figure.savefig(output / "luma-difference-histogram.png", dpi=140)
    plt.close(figure)
    top_differences = np.argsort(difference_hist)[-20:][::-1]
    return {
        "top_difference_values": [
            {"difference": int(index - 1023), "count": int(difference_hist[index])}
            for index in top_differences
        ]
    }


def consecutive_ranges(values: np.ndarray) -> list[list[int]]:
    if not values.size:
        return []
    ranges: list[list[int]] = []
    start = previous = int(values[0])
    for value in values[1:]:
        current = int(value)
        if current != previous + 1:
            ranges.append([start, previous])
            start = current
        previous = current
    ranges.append([start, previous])
    return ranges


def luma_structure(actual: np.ndarray) -> dict[str, object]:
    all_zero_rows = np.flatnonzero(np.all(actual == 0, axis=1))
    all_zero_columns = np.flatnonzero(np.all(actual == 0, axis=0))
    quadrants = []
    for row_index, (y0, y1) in enumerate(((0, 270), (270, 540), (540, 810), (810, 1080))):
        for column_index, (x0, x1) in enumerate(((0, 480), (480, 960), (960, 1440), (1440, 1920))):
            quadrants.append({
                "row_quarter": row_index,
                "column_quarter": column_index,
                "zero_fraction": float(np.mean(actual[y0:y1, x0:x1] == 0)),
            })
    row_jump = np.mean(np.abs(np.diff(actual.astype(np.int16), axis=0)), axis=1)
    boundary_jumps = []
    for multiple in (8, 16, 32, 64, 128):
        boundary = np.arange(1, HEIGHT) % multiple == 0
        boundary_jumps.append({
            "row_multiple": multiple,
            "mean_jump_at_boundary": float(row_jump[boundary].mean()),
            "mean_jump_elsewhere": float(row_jump[~boundary].mean()),
        })
    return {
        "zero_fraction": float(np.mean(actual == 0)),
        "all_zero_row_count": int(all_zero_rows.size),
        "all_zero_row_ranges": consecutive_ranges(all_zero_rows),
        "all_zero_column_count": int(all_zero_columns.size),
        "all_zero_column_ranges": consecutive_ranges(all_zero_columns),
        "quarter_zero_fractions": quadrants,
        "row_boundary_discontinuities": boundary_jumps,
    }


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: analyze-q10c-readback.py READBACK HEVC OUTPUT_DIR")
    readback = Path(sys.argv[1])
    hevc = Path(sys.argv[2])
    output = Path(sys.argv[3])
    output.mkdir(parents=True, exist_ok=True)

    raw_words, gpu_y, gpu_u, gpu_v = read_gpu(readback)
    identities, frames = decode_all_frames(hevc)
    best_frame, frame_rows = compare_frames((gpu_y, gpu_u, gpu_v), frames,
                                             output / "frame-comparison.csv")
    reference = frames[best_frame]
    unique_hashes: dict[str, list[int]] = {}
    for identity in identities:
        unique_hashes.setdefault(str(identity["sha256"]), []).append(int(identity["frame"]))

    y_stats = {
        "minimum": int(gpu_y.min()),
        "maximum": int(gpu_y.max()),
        "unique_values": int(np.unique(gpu_y).size),
        "mean": float(gpu_y.mean()),
        "standard_deviation": float(gpu_y.std()),
        "zero_samples": int(np.count_nonzero(gpu_y == 0)),
        "baseline_against_best_frame": metrics(gpu_y, reference[0]),
    }
    histogram_summary = save_histograms(output, gpu_y, reference[0])
    save_images(output, (gpu_y, gpu_u, gpu_v), reference, best_frame)

    analysis = {
        "inputs": {
            "readback": str(readback),
            "readback_sha256": hashlib.sha256(readback.read_bytes()).hexdigest(),
            "readback_bytes": readback.stat().st_size,
            "hevc": str(hevc),
            "hevc_sha256": hashlib.sha256(hevc.read_bytes()).hexdigest(),
            "decoded_frames": len(frames),
            "unique_decoded_frame_hashes": unique_hashes,
        },
        "all_frame_comparison": {
            "best_frame": best_frame,
            "best_frame_metrics": frame_rows[best_frame],
            "first_frame_metrics": frame_rows[0],
        },
        "parser_self_check": parser_self_check(frames[0]),
        "luma_statistics": y_stats,
        "luma_structure": luma_structure(gpu_y),
        "luma_histograms": histogram_summary,
        "geometry": geometric_search(gpu_y, reference[0]),
        "row_pitch_and_coded_height": row_pitch_search(raw_words, reference[0]),
        "value_transformations": value_transformations(raw_words, gpu_y, reference[0]),
        "chroma": chroma_analysis(raw_words, gpu_u, gpu_v, reference[1], reference[2]),
        "format_observations": {
            "p010_low_six_bits_nonzero": int(np.count_nonzero(raw_words & 0x3F)),
            "visible_luma_values_not_divisible_by_4": int(np.count_nonzero(gpu_y & 0x3)),
        },
    }
    (output / "analysis.json").write_text(json.dumps(analysis, indent=2, sort_keys=True) + "\n")
    print(json.dumps({
        "best_frame": best_frame,
        "best_overall_rmse": frame_rows[best_frame]["overall_rmse"],
        "luma": y_stats,
        "best_geometry": analysis["geometry"]["top_candidates"][0],
        "best_value_mapping": analysis["value_transformations"]["mapping_hypotheses"][0],
        "best_chroma_variant": analysis["chroma"]["component_and_deinterleave_variants"][0],
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
