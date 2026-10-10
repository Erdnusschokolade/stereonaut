#!/usr/bin/env python3
"""Small host-side checks for the projection and stereo UV contracts."""

import math


def projection_uv(direction, projection, fisheye_fov=190.0):
    x, y, z = direction
    length = math.sqrt(x * x + y * y + z * z)
    x, y, z = x / length, y / length, z / length
    longitude = math.atan2(x, -z)
    latitude = math.asin(max(-1.0, min(1.0, y)))
    if projection == "fisheye":
        # Equidistant lens, as in the fragment shaders.
        d = (math.cos(latitude) * math.sin(longitude), math.sin(latitude),
             -math.cos(latitude) * math.cos(longitude))
        radius = math.acos(max(-1.0, min(1.0, -d[2]))) / math.radians(
            fisheye_fov / 2)
        if radius > 1.0:
            return None
        planar = math.hypot(d[0], d[1])
        ax, ay = (d[0] / planar, d[1] / planar) if planar > 1e-6 else (0, 0)
        return 0.5 + 0.5 * radius * ax, 0.5 - 0.5 * radius * ay
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


def view_ray(fov_tangents, ndc):
    """Reconstruct the unnormalized varying emitted by the vertex shader."""
    left, right, down, up = fov_tangents
    x = left + (right - left) * (ndc[0] * 0.5 + 0.5)
    y = down + (up - down) * (ndc[1] * 0.5 + 0.5)
    return x, y, -1.0


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

# The varying must stay in tangent space until fragment interpolation. These
# asymmetric values reproduce the kind of per-eye FOV reported by SteamVR.
close(view_ray((-1.62, 1.26, -1.78, 1.12), (0.0, 0.0)),
      (-0.18, -0.33, -1.0))
close(view_ray((-1.62, 1.26, -1.78, 1.12), (0.5, -0.5)),
      (0.54, -1.055, -1.0))



# Dome placement (stereonaut-renderer.c): rotation_axis() and world_to_dome
# = Rz(roll) * Rx(-tilt) * Ry(yaw); the eye offset is followed to the unit
# sphere as in the fragment shaders.
def rotation_axis(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    m = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]  # m[row][col]
    a, b = (axis + 1) % 3, (axis + 2) % 3
    m[a][a] = c
    m[b][b] = c
    m[b][a] = s
    m[a][b] = -s
    return m


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)]
            for r in range(3)]


def apply(m, v):
    return tuple(sum(m[r][k] * v[k] for k in range(3)) for r in range(3))


def world_to_dome(yaw, tilt, roll):
    return mat_mul(rotation_axis(2, roll),
                   mat_mul(rotation_axis(0, -tilt), rotation_axis(1, yaw)))


def dome_lookup(direction, eye):
    length = math.sqrt(sum(d * d for d in direction))
    d = [x / length for x in direction]
    b = sum(e * x for e, x in zip(eye, d))
    t = -b + math.sqrt(b * b - sum(e * e for e in eye) + 1.0)
    return tuple(e + t * x for e, x in zip(eye, d))


rad = math.radians
# "Drehung +30" puts the picture centre 30 degrees to the right.
right30 = (math.sin(rad(30)), 0.0, -math.cos(rad(30)))
close(apply(world_to_dome(rad(30), 0, 0), right30), (0.0, 0.0, -1.0))
# "Neigung +20" puts it 20 degrees up.
up20 = (0.0, math.sin(rad(20)), -math.cos(rad(20)))
close(apply(world_to_dome(0, rad(20), 0), up20), (0.0, 0.0, -1.0))
# "Kippen +10" rolls it clockwise: the picture's top leans right.
top = apply(world_to_dome(0, 0, rad(10)),
            (math.sin(rad(10)), math.cos(rad(10)), 0.0))
close(top, (0.0, 1.0, 0.0))
# Yaw, then tilt: the centre is up and to the right.
centre = apply(world_to_dome(rad(30), rad(20), 0),
               (math.sin(rad(30)) * math.cos(rad(20)), math.sin(rad(20)),
                -math.cos(rad(30)) * math.cos(rad(20))))
close(centre, (0.0, 0.0, -1.0))
# The renderer's dome_to_world is the inverse.
inverse = mat_mul(rotation_axis(1, -rad(30)),
                  mat_mul(rotation_axis(0, rad(20)),
                          rotation_axis(2, -rad(10))))
identity = mat_mul(inverse, world_to_dome(rad(30), rad(20), rad(10)))
for r in range(3):
    close(identity[r], [1.0 if c == r else 0.0 for c in range(3)])

# Without offset the lookup is the plain direction.
close(dome_lookup((0.3, 0.2, -1.0), (0, 0, 0)),
      tuple(x / math.sqrt(1.13) for x in (0.3, 0.2, -1.0)))
# Zoom (eye forward) magnifies: what is seen 30 degrees off axis lies
# closer to the picture centre.
hit = dome_lookup(right30, (0.0, 0.0, -0.3))
assert math.degrees(math.atan2(hit[0], -hit[2])) < 30.0 - 5.0, hit
assert math.isclose(sum(x * x for x in hit), 1.0, abs_tol=1e-9)
# Height -0.2 (eye lower, picture up): looking straight ahead shows a point
# below the picture centre.
hit = dome_lookup((0.0, 0.0, -1.0), (0.0, -0.2, 0.0))
assert hit[1] < 0.0, hit

# Fisheye: straight ahead is the circle centre; 95 degrees to the right is
# the right edge of a 190-degree lens, 47.5 degrees up half way to the top;
# behind the lens nothing is shown.
close(projection_uv((0.0, 0.0, -1.0), "fisheye"), (0.5, 0.5))
close(projection_uv((math.sin(rad(95)), 0.0, -math.cos(rad(95))), "fisheye"),
      (1.0, 0.5))
close(projection_uv((0.0, math.sin(rad(47.5)), -math.cos(rad(47.5))),
                    "fisheye"), (0.5, 0.25))
assert projection_uv((0.0, 0.0, 1.0), "fisheye") is None
# A 220-degree lens maps the same 95 degrees inside the circle.
u, v = projection_uv((math.sin(rad(95)), 0.0, -math.cos(rad(95))), "fisheye",
                     220.0)
assert math.isclose(u, 0.5 + 0.5 * 95 / 110, abs_tol=1e-9), u

print("projection_math=success")
