#pragma once

// Small column-major matrix and quaternion helpers for the VR test.
#include <cmath>

struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
  Mat4 r;
  for (int c = 0; c < 4; c++)
    for (int row = 0; row < 4; row++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  return r;
}

inline Mat4 translation(float x, float y, float z) {
  Mat4 r;
  r.m[12] = x, r.m[13] = y, r.m[14] = z;
  return r;
}

// Quaternion (x, y, z, w) to a rotation matrix.
inline Mat4 rotation(const float q[4]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat4 r;
  r.m[0] = 1 - 2 * (y * y + z * z), r.m[1] = 2 * (x * y + z * w), r.m[2] = 2 * (x * z - y * w);
  r.m[4] = 2 * (x * y - z * w), r.m[5] = 1 - 2 * (x * x + z * z), r.m[6] = 2 * (y * z + x * w);
  r.m[8] = 2 * (x * z + y * w), r.m[9] = 2 * (y * z - x * w), r.m[10] = 1 - 2 * (x * x + y * y);
  return r;
}

inline void rotate(const float q[4], const float v[3], float out[3]) {
  Mat4 r = rotation(q);
  for (int i = 0; i < 3; i++) out[i] = r.m[i] * v[0] + r.m[4 + i] * v[1] + r.m[8 + i] * v[2];
}

// The view matrix of an eye at pos looking along its orientation q (camera looks down -z).
inline Mat4 eyeView(const float pos[3], const float q[4]) {
  Mat4 r = rotation(q), inv;
  for (int c = 0; c < 3; c++)
    for (int row = 0; row < 3; row++) inv.m[c * 4 + row] = r.m[row * 4 + c];
  return inv * translation(-pos[0], -pos[1], -pos[2]);
}

// An asymmetric frustum from tangents of the half-angles: left, right, up, down.
inline Mat4 frustum(const float tan[4], float near, float far) {
  const float l = -tan[0] * near, r = tan[1] * near, t = tan[2] * near, b = -tan[3] * near;
  Mat4 m;
  m.m[0] = 2 * near / (r - l);
  m.m[5] = 2 * near / (t - b);
  m.m[8] = (r + l) / (r - l);
  m.m[9] = (t + b) / (t - b);
  m.m[10] = -(far + near) / (far - near);
  m.m[11] = -1;
  m.m[14] = -2 * far * near / (far - near);
  m.m[15] = 0;
  return m;
}

inline void qmul(const float a[4], const float b[4], float o[4]) {
  const float r[4] = {
      a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
      a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
      a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
      a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
  };
  for (int i = 0; i < 4; i++) o[i] = r[i];
}
