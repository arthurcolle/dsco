/* gsl_dijkstra.h — Dijkstra's Algorithm (Graph Shortest Path) */
#ifndef __GSL_DIJKSTRA_H__
#define __GSL_DIJKSTRA_H__

#include "gsl_matrix.h"
#include "gsl_vector.h"

int gsl_dijkstra(const gsl_matrix *graph, size_t src, gsl_vector *dist, gsl_vector *prev);

#endif /* __GSL_DIJKSTRA_H__ */