/* gsl_bst.h — Binary Search Tree */
#ifndef __GSL_BST_H__
#define __GSL_BST_H__

#include <stdlib.h>

typedef struct gsl_bst_node_struct gsl_bst_node;

typedef struct {
    gsl_bst_node *root;
    int (*compare)(const void *, const void *);
    size_t size;
} gsl_bst;

gsl_bst *gsl_bst_alloc(int (*compare)(const void *, const void *));
void gsl_bst_free(gsl_bst *tree);
int gsl_bst_insert(gsl_bst *tree, void *item);
void *gsl_bst_find(const gsl_bst *tree, const void *item);
int gsl_bst_remove(gsl_bst *tree, const void *item);
size_t gsl_bst_size(const gsl_bst *tree);
int gsl_bst_is_empty(const gsl_bst *tree);

#endif /* __GSL_BST_H__ */