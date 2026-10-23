#ifndef STEREONAUT_XR_MATH_H
#define STEREONAUT_XR_MATH_H

#include <openxr/openxr.h>

/* Matrices (column-major, as the shaders take them) and vector helpers of
 * the XR host. */

struct mat4 { float v[16]; };

struct mat4 multiply(struct mat4 a, struct mat4 b);
/* View matrix of an OpenXR view pose (its inverse). */
struct mat4 view_from_pose(XrPosef pose);
/* Rotation of a pose (camera to LOCAL), without its position. */
struct mat4 rotation_from_pose(XrPosef pose);
/* Rotation and position of a pose. */
struct mat4 pose_matrix(XrPosef pose);
/* Rotation about the x (0), y (1) or z (2) axis, right-handed. */
struct mat4 rotation_axis(int axis, float angle);
/* Vulkan projection of an OpenXR field of view. */
struct mat4 projection_from_fov(XrFovf fov);
XrVector3f transform_direction(struct mat4 m, XrVector3f v);

XrVector3f rotate_vector(XrQuaternionf q, XrVector3f vector);
XrQuaternionf quaternion_conjugate(XrQuaternionf q);
/* Rotation about the vertical axis. */
XrQuaternionf yaw_quaternion(float yaw);
float vector_dot(XrVector3f a, XrVector3f b);

#endif
