/* NodyOS - maths without libm. */
#include "lib/kmath.h"

/* sin by range reduction to [-pi/2, pi/2] and a 9th-order odd polynomial.
 * Worst-case error is about 4e-6, far below what a pixel can show. */
float m_sin(float x)
{
    float k = m_floor(x * (1.0f / M_PI) + 0.5f);
    float r = x - k * M_PI;
    float r2 = r * r;
    float s = r * (1.0f + r2 * (-1.0f / 6.0f + r2 * (1.0f / 120.0f + r2 * (-1.0f / 5040.0f + r2 * (1.0f / 362880.0f)))));
    return ((int)k & 1) ? -s : s;
}

float m_cos(float x)
{
    return m_sin(x + M_PI * 0.5f);
}

static inline float lattice(int x, int y, int z)
{
    u32 h = m_hash((u32)x * 0x1F123BB5u ^ m_hash((u32)y * 0x5F356495u ^ m_hash((u32)z + 0x2545F491u)));
    return (float)(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float m_noise3(float x, float y, float z)
{
    int xi = m_ifloor(x), yi = m_ifloor(y), zi = m_ifloor(z);
    float fx = m_smooth(x - (float)xi), fy = m_smooth(y - (float)yi), fz = m_smooth(z - (float)zi);
    float c000 = lattice(xi, yi, zi),         c100 = lattice(xi + 1, yi, zi);
    float c010 = lattice(xi, yi + 1, zi),     c110 = lattice(xi + 1, yi + 1, zi);
    float c001 = lattice(xi, yi, zi + 1),     c101 = lattice(xi + 1, yi, zi + 1);
    float c011 = lattice(xi, yi + 1, zi + 1), c111 = lattice(xi + 1, yi + 1, zi + 1);
    float a = m_lerp(m_lerp(c000, c100, fx), m_lerp(c010, c110, fx), fy);
    float b = m_lerp(m_lerp(c001, c101, fx), m_lerp(c011, c111, fx), fy);
    return m_lerp(a, b, fz);
}
