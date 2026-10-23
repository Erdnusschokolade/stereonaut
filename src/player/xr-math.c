#include "xr-math.h"

#include <math.h>

struct mat4 multiply(struct mat4 a, struct mat4 b)
{
    struct mat4 result = {{0}};
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k)
                result.v[col * 4 + row] += a.v[k * 4 + row] * b.v[col * 4 + k];
    return result;
}

struct mat4 view_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Invert the rigid OpenXR view pose.  The rotational part of the view
     * matrix is the transpose of the pose's quaternion rotation. */
    struct mat4 result = {{
        1 - 2*y*y - 2*z*z, 2*x*y - 2*z*w,     2*x*z + 2*y*w,     0,
        2*x*y + 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z - 2*x*w,     0,
        2*x*z - 2*y*w,     2*y*z + 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
    result.v[12] = -(result.v[0] * pose.position.x + result.v[4] * pose.position.y + result.v[8] * pose.position.z);
    result.v[13] = -(result.v[1] * pose.position.x + result.v[5] * pose.position.y + result.v[9] * pose.position.z);
    result.v[14] = -(result.v[2] * pose.position.x + result.v[6] * pose.position.y + result.v[10] * pose.position.z);
    return result;
}

struct mat4 rotation_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Camera-to-LOCAL rotation. Position is deliberately excluded so
     * equirectangular video cannot acquire geometry parallax from head or
     * physical eye translation. */
    return (struct mat4){{
        1 - 2*y*y - 2*z*z, 2*x*y + 2*z*w,     2*x*z - 2*y*w,     0,
        2*x*y - 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z + 2*x*w,     0,
        2*x*z + 2*y*w,     2*y*z - 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
}

/* Rotation about the x (0), y (1) or z (2) axis, right-handed. */
struct mat4 rotation_axis(int axis, float angle)
{
    float c = cosf(angle), s = sinf(angle);
    struct mat4 m = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    int a = (axis + 1) % 3, b = (axis + 2) % 3;
    m.v[a * 4 + a] = c;
    m.v[b * 4 + b] = c;
    m.v[a * 4 + b] = s;  /* column a, row b */
    m.v[b * 4 + a] = -s; /* column b, row a */
    return m;
}

XrVector3f transform_direction(struct mat4 m, XrVector3f v)
{
    return (XrVector3f){
        m.v[0] * v.x + m.v[4] * v.y + m.v[8] * v.z,
        m.v[1] * v.x + m.v[5] * v.y + m.v[9] * v.z,
        m.v[2] * v.x + m.v[6] * v.y + m.v[10] * v.z,
    };
}

struct mat4 projection_from_fov(XrFovf fov)
{
    const float near_z = 0.05f, far_z = 100.0f;
    float left = tanf(fov.angleLeft), right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown), up = tanf(fov.angleUp);
    struct mat4 result = {{0}};
    result.v[0] = 2.0f / (right - left);
    /* Vulkan's framebuffer Y direction is opposite to OpenXR's view-space
     * Y direction when a positive viewport height is used. */
    result.v[5] = 2.0f / (down - up);
    result.v[8] = (right + left) / (right - left);
    result.v[9] = (up + down) / (down - up);
    result.v[10] = far_z / (near_z - far_z);
    result.v[11] = -1.0f;
    result.v[14] = -(far_z * near_z) / (far_z - near_z);
    return result;
}

XrVector3f rotate_vector(XrQuaternionf q, XrVector3f vector)
{
    XrVector3f u = {q.x, q.y, q.z};
    float dot_uv = u.x * vector.x + u.y * vector.y + u.z * vector.z;
    float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    XrVector3f cross = {
        u.y * vector.z - u.z * vector.y,
        u.z * vector.x - u.x * vector.z,
        u.x * vector.y - u.y * vector.x,
    };
    return (XrVector3f){
        2.0f * dot_uv * u.x + (q.w * q.w - dot_uu) * vector.x +
            2.0f * q.w * cross.x,
        2.0f * dot_uv * u.y + (q.w * q.w - dot_uu) * vector.y +
            2.0f * q.w * cross.y,
        2.0f * dot_uv * u.z + (q.w * q.w - dot_uu) * vector.z +
            2.0f * q.w * cross.z,
    };
}

XrQuaternionf quaternion_conjugate(XrQuaternionf q)
{
    return (XrQuaternionf){-q.x, -q.y, -q.z, q.w};
}

XrQuaternionf yaw_quaternion(float yaw)
{
    return (XrQuaternionf){0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
}

float vector_dot(XrVector3f a, XrVector3f b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct mat4 pose_matrix(XrPosef pose)
{
    struct mat4 m = rotation_from_pose(pose);
    m.v[12] = pose.position.x;
    m.v[13] = pose.position.y;
    m.v[14] = pose.position.z;
    return m;
}
