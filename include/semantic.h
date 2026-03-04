#ifndef DSCO_SEMANTIC_H
#define DSCO_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>

/* ── Tokenizer ─────────────────────────────────────────────────────────── */

#define SEM_MAX_TOKENS     256
#define SEM_MAX_TOKEN_LEN  64
#define SEM_MAX_DOCS       512
#define SEM_VOCAB_SIZE     4096
#define SEM_MAX_CATEGORIES 16
#define SEM_CATEGORY_KEYWORDS 32

typedef struct {
    char tokens[SEM_MAX_TOKENS][SEM_MAX_TOKEN_LEN];
    int  count;
} token_list_t;

void sem_tokenize(const char *text, token_list_t *out);

/* ── TF-IDF Engine ─────────────────────────────────────────────────────── */

typedef struct {
    char   term[SEM_MAX_TOKEN_LEN];
    double idf;            /* log(N / df) */
    int    df;             /* document frequency */
    int    vocab_id;
} vocab_entry_t;

typedef struct {
    double values[SEM_VOCAB_SIZE]; /* sparse-ish: indexed by vocab_id */
    int    nonzero[SEM_VOCAB_SIZE];
    int    nnz;
} tfidf_vec_t;

typedef struct {
    vocab_entry_t vocab[SEM_VOCAB_SIZE];
    int           vocab_count;
    int           doc_count;

    /* Pre-computed document vectors (for tools, messages, etc.) */
    tfidf_vec_t   doc_vecs[SEM_MAX_DOCS];
    int           doc_vec_count;
} tfidf_index_t;

void   sem_tfidf_init(tfidf_index_t *idx);
int    sem_tfidf_add_doc(tfidf_index_t *idx, const char *text);
void   sem_tfidf_finalize(tfidf_index_t *idx);  /* compute IDFs after all docs added */
void   sem_tfidf_vectorize(tfidf_index_t *idx, const char *text, tfidf_vec_t *out);
double sem_cosine_sim(const tfidf_vec_t *a, const tfidf_vec_t *b);

/* ── BM25 Scoring ──────────────────────────────────────────────────────── */

#define BM25_K1  1.2
#define BM25_B   0.75

typedef struct {
    int doc_id;
    double score;
} bm25_result_t;

/* Score a query against all documents. Results sorted descending. */
int sem_bm25_rank(tfidf_index_t *idx, const char *query,
                  bm25_result_t *results, int max_results);

/* ── Tool Relevance Scoring ────────────────────────────────────────────── */

typedef struct {
    int   tool_index;
    double score;          /* combined relevance score */
    bool  always_include;  /* essential tools always sent */
} tool_score_t;

/* Build tool index from tool names+descriptions. Call once at startup. */
void sem_tools_index_build(tfidf_index_t *idx,
                           const char **names, const char **descriptions,
                           int tool_count);

/* Rank tools by relevance to a query. Returns count of results. */
int sem_tools_rank(tfidf_index_t *idx, const char *query,
                   tool_score_t *results, int max_results, int tool_count);

/* ── Query Classification ──────────────────────────────────────────────── */

typedef enum {
    QCAT_FILE_IO,
    QCAT_GIT,
    QCAT_NETWORK,
    QCAT_SHELL,
    QCAT_CODE,
    QCAT_CRYPTO,
    QCAT_SWARM,
    QCAT_AST,
    QCAT_PIPELINE,
    QCAT_MATH,
    QCAT_SEARCH,
    QCAT_GENERAL,
    QCAT_COUNT
} query_category_t;

typedef struct {
    query_category_t category;
    double           confidence;
} classification_t;

/* Classify a query into one or more categories.
   Returns count of classifications (top N). */
int sem_classify(const char *query, classification_t *results, int max_results);

/* Get category name string */
const char *sem_category_name(query_category_t cat);

/* ── Context Relevance Scoring ─────────────────────────────────────────── */

typedef struct {
    int    msg_index;
    double relevance;       /* semantic similarity to current query */
    bool   is_recent;       /* within last N messages */
    bool   has_tool_result; /* contains tool output (high value) */
    double final_score;     /* combined score for retention */
} msg_score_t;

/* Score conversation messages for relevance to current query.
   Returns indices sorted by final_score descending. */
int sem_score_messages(tfidf_index_t *idx, const char *query,
                       const char **msg_texts, int msg_count,
                       msg_score_t *results, int max_results);

#endif
