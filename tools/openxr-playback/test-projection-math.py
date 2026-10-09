#!/usr/bin/env python3
"""Small host-side checks for the projection and stereo UV contracts."""

import math


def projection_uv(direction, projection):
    x, y, z = direction
    length = math.sqrt(x * x + y * y + z * z)
    x, y, z = x / length, y / length, z / length
    longitude = math.atan2(x, -z)
    latitude = math.asin(max(-1.0, min(1.0, y)))
    if projection == "180":
        if abs(longitude) > math.pi / 2:
            return None
        return longitude / math.pi + 0.5, 0.5 - latitude / math.pi
    return (longitude / (2 * math.pi) + 0.5) % 1.0, 0.5 - latitude / math.pi


def stereo_uv(uv, layout, eye, swap=False):
    eye ^= int(swap)
    u, v = uv
    if layout == "sbs":
        u = 0.5 * eye + 0.5 * u
    elif layout == "tb":
        v = 0.5 * eye + 0.5 * v
    return u, v


def close(actual, expected):
    assert actual is not None
    assert all(math.isclose(a, e, abs_tol=1e-7)
               for a, e in zip(actual, expected)), (actual, expected)


close(projection_uv((0, 0, -1), "360"), (0.5, 0.5))
close(projection_uv((1, 0, 0), "360"), (0.75, 0.5))
close(projection_uv((-1, 0, 0), "360"), (0.25, 0.5))
close(projection_uv((0, 0, 1), "360"), (0.0, 0.5))
assert math.isclose(projection_uv((0, 1, 0), "360")[1], 0.0,
                    abs_tol=1e-7)
assert math.isclose(projection_uv((0, -1, 0), "360")[1], 1.0,
                    abs_tol=1e-7)
close(projection_uv((1, 0, 0), "180"), (1.0, 0.5))
close(projection_uv((-1, 0, 0), "180"), (0.0, 0.5))
assert projection_uv((0, 0, 1), "180") is None

close(stereo_uv((0.25, 0.75), "mono", 0), (0.25, 0.75))
close(stereo_uv((0.25, 0.75), "sbs", 0), (0.125, 0.75))
close(stereo_uv((0.25, 0.75), "sbs", 1), (0.625, 0.75))
close(stereo_uv((0.25, 0.75), "tb", 0), (0.25, 0.375))
close(stereo_uv((0.25, 0.75), "tb", 1), (0.25, 0.875))
close(stereo_uv((0.25, 0.75), "sbs", 0, True), (0.625, 0.75))

print("projection_math=success")
