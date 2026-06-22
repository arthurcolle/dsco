#include <stddef.h>
/* gsl_math.h — minimal math constants and helpers */
#ifndef __GSL_MATH_H__
#define __GSL_MATH_H__

#include <math.h>
#include <float.h>

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_SQRT2    1.4142135623730950488
#define M_SQRT1_2  0.70710678118654752440
#define M_SQRT3    1.7320508075688772935
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_1_SQRT2PI 0.39894228040143267794

#define GSL_DBL_EPSILON DBL_EPSILON
#define GSL_DBL_MIN     DBL_MIN
#define GSL_DBL_MAX     DBL_MAX
#define GSL_SQRT_DBL_EPSILON sqrt(DBL_EPSILON)

#define GSL_IS_ODD(n)  ((n) & 1)
#define GSL_IS_EVEN(n) (!GSL_IS_ODD(n))

#define GSL_MAX(a,b) ((a) > (b) ? (a) : (b))
#define GSL_MIN(a,b) ((a) < (b) ? (a) : (b))
#define GSL_MAX_DBL(a,b) GSL_MAX(a,b)
#define GSL_MIN_DBL(a,b) GSL_MIN(a,b)

#endif /* __GSL_MATH_H__ */