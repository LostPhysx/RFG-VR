#pragma once
// Small math helpers for mapping OpenXR poses (right-handed, +Y up, -Z forward, metres) into the
// game's left-handed world (+Y up, +Z forward; Volition matrix rows = right/up/forward vectors).

#include <algorithm>
#include <cmath>

#include <openxr/openxr.h>

namespace rfgvr::vrmath {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-6f ? a * (1.f / l) : Vec3{0, 0, 1};
}

// Basis matrix: rows are the right, up and forward axes expressed in the parent frame
// (v_parent = v_local.x * r + v_local.y * u + v_local.z * f).
struct Basis {
    Vec3 r{1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
};

inline Vec3 toParent(const Basis& b, Vec3 v) { return b.r * v.x + b.u * v.y + b.f * v.z; }
inline Vec3 toLocal(const Basis& b, Vec3 v) { return {dot(v, b.r), dot(v, b.u), dot(v, b.f)}; }  // orthonormal b

// child expressed in parent -> child expressed in the parent's parent
inline Basis compose(const Basis& child, const Basis& parent) {
    return {toParent(parent, child.r), toParent(parent, child.u), toParent(parent, child.f)};
}

// Yaw-only version of a basis: forward and right projected onto the ground plane, up = world up.
// Built from the engine's own right vector (no cross product), so no handedness assumption.
inline Basis yawOnly(const Basis& b) {
    Vec3 f = normalize({b.f.x, 0, b.f.z});
    Vec3 r{b.r.x, 0, b.r.z};
    r = r - f * dot(r, f);  // re-orthogonalise against forward
    if (length(r) < 1e-3f) r = {b.r.x, 0, b.r.z};
    r = normalize(r);
    return {r, {0, 1, 0}, f};
}

// OpenXR (RH) -> game (LH): negate Z of positions; quaternion (x,y,z,w) -> (-x,-y,z,w).
inline Vec3 positionToLh(const XrVector3f& p) { return {p.x, p.y, -p.z}; }
inline XrQuaternionf orientationToLh(const XrQuaternionf& q) { return {-q.x, -q.y, q.z, q.w}; }

inline Vec3 rotate(const XrQuaternionf& q, Vec3 v) {
    // v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
    Vec3 qv{q.x, q.y, q.z};
    Vec3 t = cross(qv, v) * 2.f;
    return v + t * q.w + cross(qv, t);
}

// Basis whose rows are the rotated unit axes (LH quaternion expected).
inline Basis basisFromQuat(const XrQuaternionf& q) {
    return {rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1})};
}

// Symmetric vertical FOV (degrees) that contains the asymmetric one, for the engine's culling frustum.
inline float coveringVerticalFovDeg(const XrFovf& fov, float margin = 1.05f) {
    float t = std::max(std::tan(fov.angleUp), std::tan(-fov.angleDown)) * margin;
    return 2.f * std::atan(t) * 180.f / 3.14159265f;
}

}  // namespace rfgvr::vrmath
