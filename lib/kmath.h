/* NodyOS - maths without libm. */
#ifndef NODY_KMATH_H
#define NODY_KMATH_H

#include "lib/types.h"

#define M_PI  3.14159265358979f
#define M_TAU 6.28318530717959f
#define M_DEG 0.01745329251994f         /* radians per degree */

static inline float m_abs(float x) { return x < 0 ? -x : x; }
static inline float m_min(float a, float b) { return a < b ? a : b; }
static inline float m_max(float a, float b) { return a > b ? a : b; }
static inline float m_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float m_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float m_smooth(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float m_sqrt(float x) { return __builtin_sqrtf(x); }

static inline int i_min(int a, int b) { return a < b ? a : b; }
static inline int i_max(int a, int b) { return a > b ? a : b; }
static inline int i_clamp(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int i_abs(int x) { return x < 0 ? -x : x; }

static inline int   m_ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
static inline int   m_iceil(float x) { int i = (int)x; return i + (x > (float)i); }
static inline int   m_iround(float x) { return m_ifloor(x + 0.5f); }
static inline float m_floor(float x) { return (float)m_ifloor(x); }

float m_sin(float x);
float m_cos(float x);

/* integer hash and value noise */
static inline u32 m_hash(u32 x)
{
    x ^= x >> 16; x *= 0x9E3779B1u;
    x ^= x >> 13; x *= 0x7F4A7C15u;
    x ^= x >> 16;
    return x;
}
static inline u32 m_hash2(u32 a, u32 b) { return m_hash(a ^ m_hash(b + 0x51ED270Bu)); }
static inline float m_rand01(u32 h) { return (float)(m_hash(h) >> 8) * (1.0f / 16777216.0f); }
static inline float m_rand11(u32 h) { return m_rand01(h) * 2.0f - 1.0f; }

float m_noise3(float x, float y, float z);      /* smooth, range [-1, 1] */

#endif
