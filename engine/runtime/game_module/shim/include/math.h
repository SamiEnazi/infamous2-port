/* The game module's <math.h> (slim split): the math names of the lifted code, UCRT-compatible
 * prototypes (clang treats them as builtins); dllimport where the module calls them out of line
 * and the UCRT's header has it. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double fabs(double); float fabsf(float);
double sqrt(double); float sqrtf(float);
double fma(double, double, double); float fmaf(float, float, float);
double trunc(double); float truncf(float);
double floor(double); float floorf(float);
double ceil(double); float ceilf(float);
double nearbyint(double); float nearbyintf(float);
double rint(double); float rintf(float);
double ldexp(double, int); float ldexpf(float, int);
double frexp(double, int*); float frexpf(float, int*);
double exp(double); float expf(float);
__declspec(dllimport) double exp2(double); __declspec(dllimport) float exp2f(float);
__declspec(dllimport) double log2(double); __declspec(dllimport) float log2f(float);
double copysign(double, double); float copysignf(float, float);
double fmin(double, double); double fmax(double, double);
float fminf(float, float); float fmaxf(float, float);
#ifdef __cplusplus
}
#endif
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")
#ifndef __cplusplus
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#else
/* C++: the lifted .cpp files call them as functions (UCRT has overloads) */
static inline bool isnan(double x) { return __builtin_isnan(x); }
static inline bool isinf(double x) { return __builtin_isinf(x); }
static inline bool isfinite(double x) { return __builtin_isfinite(x); }
static inline bool signbit(double x) { return __builtin_signbit(x); }
static inline bool signbit(float x) { return __builtin_signbit(x); }
#endif
#define FP_NAN 2
#define FP_INFINITE 1
#define FP_ZERO 0
#define FP_SUBNORMAL (-2)
#define FP_NORMAL (-1)
