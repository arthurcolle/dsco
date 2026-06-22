/* gsl_heap.h — Binary Heap */
#ifndef __GSL_HEAP_H__
#define __GSL_HEAP_H__

#include <stdlib.h>

typedef struct {
    size_t size;
    size_t max_size;
    void **p;
    int (*compare)(const void *a, const void *b);
} gsl_heap;

gsl_heap *gsl_heap_alloc(size_t n, int (*compare)(const void *, const void *));
void gsl_heap_free(gsl_heap *h);
int gsl_heap_push(gsl_heap *h, void *item);
void *gsl_heap_pop(gsl_heap *h);
void *gsl_heap_peek(const gsl_heap *h);
int gsl_heap_is_empty(const gsl_heap *h);
size_t gsl_heap_size(const gsl_heap *h);

#endif /* __GSL_HEAP_H__ */