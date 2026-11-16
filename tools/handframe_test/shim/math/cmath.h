// tools/handframe_test: the engine's reliable-math header stood in by libm, so VSMatrix and the
// vector types compile outside the engine (vectors.h takes math/cmath.h when it is on the path).
#pragma once
#include <math.h>
#include <stdint.h>
inline double g_cosdeg(double v) { return cos(v * (3.14159265358979323846 / 180.)); }
inline double g_sindeg(double v) { return sin(v * (3.14159265358979323846 / 180.)); }
inline double g_cos(double v) { return cos(v); }
inline double g_sin(double v) { return sin(v); }
inline double g_sqrt(double v) { return sqrt(v); }
inline double g_atan2(double v, double w) { return atan2(v, w); }
inline double g_acos(double v) { return acos(v); }
inline double g_asin(double v) { return asin(v); }
inline double g_atan(double v) { return atan(v); }
inline double g_cosbam(unsigned int v) { return cos(v * (3.14159265358979323846 / 2147483648.0)); }
inline double g_sinbam(unsigned int v) { return sin(v * (3.14159265358979323846 / 2147483648.0)); }
