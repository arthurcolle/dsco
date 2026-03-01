#ifndef DSCO_PIPELINE_H
#define DSCO_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include "coroutine.h"

/*
 * Coroutine-based streaming pipeline engine.
 * Inspired by Simon Tatham's coroutines-in-C technique.
 *
 * Chains data through transform stages using stackless coroutines.
 * Each stage is a generator that pulls from the previous stage.
 *
 * Usage:
 *   pipeline_t *p = pipeline_create("input data\nline2\nline3");
 *   pipeline_add_stage(p, PIPE_FILTER, "pattern");
 *   pipeline_add_stage(p, PIPE_SORT, NULL);
 *   pipeline_add_stage(p, PIPE_UNIQ, NULL);
 *   char *result = pipeline_execute(p);
 *   pipeline_free(p);
 */

#define PIPE_MAX_STAGES     16
#define PIPE_MAX_LINES      65536
#define PIPE_MAX_LINE_LEN   4096
#define PIPE_MAX_OUTPUT      (256 * 1024)

typedef enum {
    PIPE_FILTER,        /* grep: keep lines matching pattern */
    PIPE_FILTER_V,      /* grep -v: remove lines matching pattern */
    PIPE_MAP,           /* sed: s/pattern/replacement/ on each line */
    PIPE_SORT,          /* sort lines */
    PIPE_SORT_N,        /* sort numerically */
    PIPE_SORT_R,        /* sort reverse */
    PIPE_UNIQ,          /* deduplicate adjacent lines */
    PIPE_UNIQ_C,        /* uniq -c: count occurrences */
    PIPE_HEAD,          /* first N lines */
    PIPE_TAIL,          /* last N lines */
    PIPE_REVERSE,       /* reverse line order */
    PIPE_COUNT,         /* count lines */
    PIPE_TRIM,          /* trim whitespace from each line */
    PIPE_UPPER,         /* uppercase */
    PIPE_LOWER,         /* lowercase */
    PIPE_PREFIX,        /* add prefix to each line */
    PIPE_SUFFIX,        /* add suffix to each line */
    PIPE_NUMBER,        /* number lines */
    PIPE_JOIN,          /* join all lines with delimiter */
    PIPE_SPLIT,         /* split each line by delimiter */
    PIPE_CUT,           /* extract field by delimiter + index */
    PIPE_REGEX,         /* extract regex matches */
    PIPE_REPLACE,       /* string replacement */
    PIPE_TAKE_WHILE,    /* take lines while pattern matches */
    PIPE_DROP_WHILE,    /* drop lines while pattern matches */
    PIPE_FLATTEN,       /* flatten nested lines (strip leading whitespace) */
    PIPE_BLANK_REMOVE,  /* remove blank lines */
    PIPE_LENGTH,        /* replace each line with its length */
    PIPE_HASH,          /* replace each line with its SHA-256 hash */
    PIPE_JSON_EXTRACT,  /* extract a JSON field from each line */
    PIPE_CSV_COLUMN,    /* extract CSV column by index */
    PIPE_STATS,         /* emit line count, word count, char count, etc. */
} pipe_stage_type_t;

typedef struct {
    pipe_stage_type_t type;
    char              arg[PIPE_MAX_LINE_LEN]; /* stage-specific argument */
    int               int_arg;                /* numeric argument */
} pipe_stage_t;

typedef struct {
    char          **lines;
    int             line_count;
    int             line_cap;
    pipe_stage_t    stages[PIPE_MAX_STAGES];
    int             stage_count;
    char           *input;   /* owned copy of input */
} pipeline_t;

/* Create & destroy */
pipeline_t *pipeline_create(const char *input);
void        pipeline_free(pipeline_t *p);

/* Add stages */
void pipeline_add_stage(pipeline_t *p, pipe_stage_type_t type,
                        const char *arg);
void pipeline_add_stage_n(pipeline_t *p, pipe_stage_type_t type,
                          int n);

/* Execute the full pipeline, returns result (caller frees) */
char *pipeline_execute(pipeline_t *p);

/* Parse a pipeline spec string like "filter:error|sort|uniq|head:10" */
pipeline_t *pipeline_parse(const char *input, const char *spec);

/* Convenience: run a pipeline spec on input, returns result */
char *pipeline_run(const char *input, const char *spec);

#endif
