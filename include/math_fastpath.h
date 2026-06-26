#ifndef DSCO_MATH_FASTPATH_H
#define DSCO_MATH_FASTPATH_H

#include <stdbool.h>
#include <stddef.h>

bool mfp_is_func(const char *name, size_t len);
bool mfp_is_const(const char *name, size_t len);
bool mfp_ident_known(const char *name, size_t len);
bool mfp_is_pure_math(const char *expr, char *reason);
bool mfp_rewrite_func_shorthand(const char *expr, char *out, size_t out_len);
bool mfp_eval(const char *expr, char *out, size_t out_len);

#endif /* DSCO_MATH_FASTPATH_H */
