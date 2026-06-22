/* gsl_splinalg.h — Sparse Linear Algebra */
#ifndef __GSL_SPLINALG_H__
#define __GSL_SPLINALG_H__

#include "gsl_spmatrix.h"
#include "gsl_vector.h"

int gsl_splinalg_itersolve_gmres(const gsl_spmatrix *A, const gsl_vector *b, gsl_vector *x,
                                  const double tol, const size_t max_iter);
int gsl_splinalg_itersolve_bicgstab(const gsl_spmatrix *A, const gsl_vector *b, gsl_vector *x,
                                     const double tol, const size_t max_iter);

#endif /* __GSL_SPLINALG_H__ */