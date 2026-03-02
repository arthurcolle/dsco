#include "semantic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * STOP WORDS — filtered during tokenization
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *STOP_WORDS[] = {
    "a", "an", "the", "is", "it", "in", "on", "at", "to", "for",
    "of", "and", "or", "but", "not", "with", "this", "that", "from",
    "by", "as", "be", "are", "was", "were", "been", "has", "have",
    "had", "do", "does", "did", "will", "would", "could", "should",
    "can", "may", "might", "if", "then", "else", "when", "where",
    "what", "which", "who", "how", "all", "each", "every", "any",
    "no", "so", "up", "out", "just", "than", "them", "its", "my",
    "your", "our", "their", "he", "she", "we", "they", "me", "him",
    "her", "us", "i", "you", NULL
};

static bool is_stop_word(const char *word) {
    for (int i = 0; STOP_WORDS[i]; i++) {
        if (strcmp(word, STOP_WORDS[i]) == 0) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOKENIZER — whitespace + punctuation split, lowercase, stop word removal
 * ═══════════════════════════════════════════════════════════════════════════ */

void sem_tokenize(const char *text, token_list_t *out) {
    out->count = 0;
    if (!text) return;

    char buf[SEM_MAX_TOKEN_LEN];
    int blen = 0;

    for (const char *p = text; ; p++) {
        bool is_sep = (*p == '\0' || isspace((unsigned char)*p) ||
                       *p == ',' || *p == '.' || *p == ':' || *p == ';' ||
                       *p == '(' || *p == ')' || *p == '[' || *p == ']' ||
                       *p == '{' || *p == '}' || *p == '"' || *p == '\'' ||
                       *p == '/' || *p == '|' || *p == '!' || *p == '?');

        if (is_sep) {
            if (blen > 1 && blen < SEM_MAX_TOKEN_LEN - 1 && out->count < SEM_MAX_TOKENS) {
                buf[blen] = '\0';
                if (!is_stop_word(buf)) {
                    memcpy(out->tokens[out->count], buf, blen + 1);
                    out->count++;
                }
            }
            blen = 0;
            if (*p == '\0') break;
        } else {
            if (blen < SEM_MAX_TOKEN_LEN - 2) {
                buf[blen++] = (char)tolower((unsigned char)*p);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TF-IDF ENGINE
 * ═══════════════════════════════════════════════════════════════════════════ */

void sem_tfidf_init(tfidf_index_t *idx) {
    memset(idx, 0, sizeof(*idx));
}

/* Find or create a vocab entry. Returns vocab_id. */
static int vocab_get_or_add(tfidf_index_t *idx, const char *term) {
    /* Linear scan — fine for <4096 terms */
    for (int i = 0; i < idx->vocab_count; i++) {
        if (strcmp(idx->vocab[i].term, term) == 0) return i;
    }
    if (idx->vocab_count >= SEM_VOCAB_SIZE) return -1;
    int id = idx->vocab_count++;
    strncpy(idx->vocab[id].term, term, SEM_MAX_TOKEN_LEN - 1);
    idx->vocab[id].vocab_id = id;
    idx->vocab[id].df = 0;
    idx->vocab[id].idf = 0.0;
    return id;
}

static int vocab_find(tfidf_index_t *idx, const char *term) {
    for (int i = 0; i < idx->vocab_count; i++) {
        if (strcmp(idx->vocab[i].term, term) == 0) return i;
    }
    return -1;
}

/* Track which terms appear in each document for DF computation */
typedef struct {
    int    doc_terms[SEM_VOCAB_SIZE];  /* vocab IDs seen in this doc */
    int    doc_tfs[SEM_VOCAB_SIZE];    /* term frequency per vocab ID */
    int    term_count;
    int    total_tokens;
} doc_stats_t;

static doc_stats_t s_doc_stats[SEM_MAX_DOCS];

int sem_tfidf_add_doc(tfidf_index_t *idx, const char *text) {
    if (idx->doc_count >= SEM_MAX_DOCS) return -1;
    int doc_id = idx->doc_count++;

    token_list_t tokens;
    sem_tokenize(text, &tokens);

    doc_stats_t *ds = &s_doc_stats[doc_id];
    memset(ds, 0, sizeof(*ds));
    ds->total_tokens = tokens.count;

    /* Count term frequencies for this document */
    for (int i = 0; i < tokens.count; i++) {
        int vid = vocab_get_or_add(idx, tokens.tokens[i]);
        if (vid < 0) continue;

        /* Check if we already saw this term in this doc */
        bool found = false;
        for (int j = 0; j < ds->term_count; j++) {
            if (ds->doc_terms[j] == vid) {
                ds->doc_tfs[j]++;
                found = true;
                break;
            }
        }
        if (!found && ds->term_count < SEM_VOCAB_SIZE) {
            ds->doc_terms[ds->term_count] = vid;
            ds->doc_tfs[ds->term_count] = 1;
            ds->term_count++;
        }
    }

    /* Update document frequencies */
    for (int j = 0; j < ds->term_count; j++) {
        idx->vocab[ds->doc_terms[j]].df++;
    }

    return doc_id;
}

void sem_tfidf_finalize(tfidf_index_t *idx) {
    int N = idx->doc_count;
    if (N == 0) return;

    /* Compute IDF for each term: log((N + 1) / (df + 1)) + 1 (smoothed) */
    for (int i = 0; i < idx->vocab_count; i++) {
        int df = idx->vocab[i].df;
        idx->vocab[i].idf = log((double)(N + 1) / (double)(df + 1)) + 1.0;
    }

    /* Build TF-IDF vectors for all documents */
    for (int d = 0; d < N; d++) {
        tfidf_vec_t *vec = &idx->doc_vecs[d];
        memset(vec, 0, sizeof(*vec));
        doc_stats_t *ds = &s_doc_stats[d];

        for (int j = 0; j < ds->term_count; j++) {
            int vid = ds->doc_terms[j];
            double tf = (double)ds->doc_tfs[j] / (double)(ds->total_tokens > 0 ? ds->total_tokens : 1);
            double tfidf = tf * idx->vocab[vid].idf;
            vec->values[vid] = tfidf;
            vec->nonzero[vec->nnz++] = vid;
        }
    }

    idx->doc_vec_count = N;
}

void sem_tfidf_vectorize(tfidf_index_t *idx, const char *text, tfidf_vec_t *out) {
    memset(out, 0, sizeof(*out));

    token_list_t tokens;
    sem_tokenize(text, &tokens);
    if (tokens.count == 0) return;

    /* Count term frequencies */
    int tfs[SEM_VOCAB_SIZE] = {0};
    int seen[SEM_MAX_TOKENS];
    int seen_count = 0;

    for (int i = 0; i < tokens.count; i++) {
        int vid = vocab_find(idx, tokens.tokens[i]);
        if (vid < 0) continue;

        if (tfs[vid] == 0) {
            seen[seen_count++] = vid;
        }
        tfs[vid]++;
    }

    /* Build TF-IDF vector */
    for (int i = 0; i < seen_count; i++) {
        int vid = seen[i];
        double tf = (double)tfs[vid] / (double)tokens.count;
        double tfidf = tf * idx->vocab[vid].idf;
        out->values[vid] = tfidf;
        out->nonzero[out->nnz++] = vid;
    }
}

double sem_cosine_sim(const tfidf_vec_t *a, const tfidf_vec_t *b) {
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;

    for (int i = 0; i < a->nnz; i++) {
        int vid = a->nonzero[i];
        dot += a->values[vid] * b->values[vid];
        norm_a += a->values[vid] * a->values[vid];
    }
    for (int i = 0; i < b->nnz; i++) {
        int vid = b->nonzero[i];
        norm_b += b->values[vid] * b->values[vid];
    }

    double denom = sqrt(norm_a) * sqrt(norm_b);
    if (denom < 1e-10) return 0.0;
    return dot / denom;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BM25 RANKING
 * ═══════════════════════════════════════════════════════════════════════════ */

static int bm25_cmp(const void *a, const void *b) {
    double sa = ((const bm25_result_t *)a)->score;
    double sb = ((const bm25_result_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

int sem_bm25_rank(tfidf_index_t *idx, const char *query,
                  bm25_result_t *results, int max_results) {
    token_list_t qtoks;
    sem_tokenize(query, &qtoks);
    if (qtoks.count == 0 || idx->doc_count == 0) return 0;

    /* Compute average document length */
    double avg_dl = 0;
    for (int d = 0; d < idx->doc_count; d++) {
        avg_dl += s_doc_stats[d].total_tokens;
    }
    avg_dl /= idx->doc_count;
    if (avg_dl < 1.0) avg_dl = 1.0;

    int result_count = 0;
    for (int d = 0; d < idx->doc_count && result_count < max_results; d++) {
        double score = 0.0;
        doc_stats_t *ds = &s_doc_stats[d];
        double dl = (double)ds->total_tokens;

        for (int qi = 0; qi < qtoks.count; qi++) {
            int vid = vocab_find(idx, qtoks.tokens[qi]);
            if (vid < 0) continue;

            /* Find term frequency in this document */
            int tf = 0;
            for (int j = 0; j < ds->term_count; j++) {
                if (ds->doc_terms[j] == vid) {
                    tf = ds->doc_tfs[j];
                    break;
                }
            }
            if (tf == 0) continue;

            double idf = idx->vocab[vid].idf;
            double tf_norm = ((double)tf * (BM25_K1 + 1.0)) /
                             ((double)tf + BM25_K1 * (1.0 - BM25_B + BM25_B * dl / avg_dl));
            score += idf * tf_norm;
        }

        if (score > 0.001) {
            results[result_count].doc_id = d;
            results[result_count].score = score;
            result_count++;
        }
    }

    qsort(results, result_count, sizeof(bm25_result_t), bm25_cmp);
    return result_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOOL RELEVANCE SCORING
 * ═══════════════════════════════════════════════════════════════════════════ */

void sem_tools_index_build(tfidf_index_t *idx,
                           const char **names, const char **descriptions,
                           int tool_count) {
    sem_tfidf_init(idx);

    /* Each tool = one document: "name name name description" for emphasis */
    for (int i = 0; i < tool_count && i < SEM_MAX_DOCS; i++) {
        char combined[2048];
        /* Repeat name for extra weight, include description */
        snprintf(combined, sizeof(combined), "%s %s %s %s",
                 names[i], names[i], names[i], descriptions[i]);
        sem_tfidf_add_doc(idx, combined);
    }
    sem_tfidf_finalize(idx);
}

static int tool_score_cmp(const void *a, const void *b) {
    double sa = ((const tool_score_t *)a)->score;
    double sb = ((const tool_score_t *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

int sem_tools_rank(tfidf_index_t *idx, const char *query,
                   tool_score_t *results, int max_results, int tool_count) {
    /* Get BM25 scores */
    bm25_result_t bm25_results[SEM_MAX_DOCS];
    int bm25_count = sem_bm25_rank(idx, query, bm25_results, tool_count);

    /* Also compute cosine similarity for blending */
    tfidf_vec_t query_vec;
    sem_tfidf_vectorize(idx, query, &query_vec);

    /* Build combined scores */
    double scores[SEM_MAX_DOCS];
    memset(scores, 0, sizeof(double) * tool_count);

    /* BM25 scores (normalized to 0-1 range) */
    double max_bm25 = 0.001;
    for (int i = 0; i < bm25_count; i++) {
        if (bm25_results[i].score > max_bm25) max_bm25 = bm25_results[i].score;
    }
    for (int i = 0; i < bm25_count; i++) {
        scores[bm25_results[i].doc_id] = bm25_results[i].score / max_bm25;
    }

    /* Blend with cosine similarity (30% cosine, 70% BM25) */
    for (int d = 0; d < tool_count && d < idx->doc_vec_count; d++) {
        double cos_score = sem_cosine_sim(&query_vec, &idx->doc_vecs[d]);
        scores[d] = 0.7 * scores[d] + 0.3 * cos_score;
    }

    /* Build result array */
    int count = 0;
    for (int i = 0; i < tool_count && count < max_results; i++) {
        results[count].tool_index = i;
        results[count].score = scores[i];
        results[count].always_include = false;
        count++;
    }

    qsort(results, count, sizeof(tool_score_t), tool_score_cmp);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * QUERY CLASSIFICATION
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    query_category_t cat;
    const char      *name;
    const char      *keywords[SEM_CATEGORY_KEYWORDS];
} category_def_t;

static const category_def_t CATEGORIES[] = {
    { QCAT_FILE_IO, "file_io", {
        "file", "read", "write", "edit", "create", "delete", "copy", "move",
        "directory", "folder", "path", "list", "find", "search", "grep",
        "content", "append", "mkdir", "tree", "permission", "chmod", NULL }},
    { QCAT_GIT, "git", {
        "git", "commit", "branch", "merge", "push", "pull", "clone", "diff",
        "status", "log", "rebase", "stash", "checkout", "remote", "tag", NULL }},
    { QCAT_NETWORK, "network", {
        "http", "https", "url", "api", "request", "curl", "download", "upload",
        "dns", "ping", "port", "socket", "websocket", "fetch", "web",
        "server", "endpoint", "rest", "json", "weather", "ip", NULL }},
    { QCAT_SHELL, "shell", {
        "bash", "shell", "command", "run", "execute", "terminal", "process",
        "script", "pipe", "sudo", "install", "brew", "apt", "npm", "make",
        "compile", "build", "docker", "container", NULL }},
    { QCAT_CODE, "code", {
        "code", "function", "class", "variable", "compile", "debug", "error",
        "bug", "fix", "refactor", "implement", "program", "source", "header",
        "syntax", "type", "struct", "define", "macro", NULL }},
    { QCAT_CRYPTO, "crypto", {
        "hash", "sha", "md5", "hmac", "encrypt", "decrypt", "key", "token",
        "jwt", "uuid", "random", "base64", "sign", "verify", "certificate",
        "password", "secret", "hkdf", "crypto", NULL }},
    { QCAT_SWARM, "swarm", {
        "swarm", "agent", "spawn", "parallel", "concurrent", "distribute",
        "worker", "task", "delegate", "orchestrate", "fan", "sub-agent",
        "multi", "batch", NULL }},
    { QCAT_AST, "ast", {
        "ast", "parse", "inspect", "introspect", "analyze", "complexity",
        "call_graph", "dependency", "function", "struct", "symbol", NULL }},
    { QCAT_PIPELINE, "pipeline", {
        "pipeline", "filter", "sort", "map", "reduce", "transform", "stream",
        "csv", "json", "column", "stats", "uniq", "count", "regex", NULL }},
    { QCAT_MATH, "math", {
        "calculate", "math", "eval", "expression", "formula", "number",
        "factorial", "fibonacci", "prime", "sqrt", "sin", "cos", "log",
        "convert", "hex", "binary", "octal", NULL }},
    { QCAT_SEARCH, "search", {
        "search", "find", "look", "where", "locate", "which", "grep",
        "pattern", "match", "query", "index", NULL }},
    { QCAT_GENERAL, "general", { NULL }},
};

static const int CATEGORY_COUNT = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

static int classify_cmp(const void *a, const void *b) {
    double ca = ((const classification_t *)a)->confidence;
    double cb = ((const classification_t *)b)->confidence;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

int sem_classify(const char *query, classification_t *results, int max_results) {
    token_list_t tokens;
    sem_tokenize(query, &tokens);
    if (tokens.count == 0) {
        if (max_results > 0) {
            results[0].category = QCAT_GENERAL;
            results[0].confidence = 1.0;
            return 1;
        }
        return 0;
    }

    double scores[QCAT_COUNT];
    memset(scores, 0, sizeof(scores));

    for (int ti = 0; ti < tokens.count; ti++) {
        for (int ci = 0; ci < CATEGORY_COUNT - 1; ci++) {  /* skip GENERAL */
            for (int ki = 0; CATEGORIES[ci].keywords[ki]; ki++) {
                /* Exact match */
                if (strcmp(tokens.tokens[ti], CATEGORIES[ci].keywords[ki]) == 0) {
                    scores[CATEGORIES[ci].cat] += 2.0;
                }
                /* Substring match (e.g., "files" matches "file") */
                else if (strstr(tokens.tokens[ti], CATEGORIES[ci].keywords[ki]) ||
                         strstr(CATEGORIES[ci].keywords[ki], tokens.tokens[ti])) {
                    scores[CATEGORIES[ci].cat] += 0.5;
                }
            }
        }
    }

    /* Normalize by token count */
    for (int i = 0; i < QCAT_COUNT; i++) {
        scores[i] /= (double)tokens.count;
    }

    /* If no category scored, default to GENERAL */
    double max_score = 0;
    for (int i = 0; i < QCAT_COUNT - 1; i++) {
        if (scores[i] > max_score) max_score = scores[i];
    }
    if (max_score < 0.1) {
        scores[QCAT_GENERAL] = 1.0;
    }

    /* Build results */
    int count = 0;
    for (int i = 0; i < QCAT_COUNT && count < max_results; i++) {
        if (scores[i] > 0.01) {
            results[count].category = (query_category_t)i;
            results[count].confidence = scores[i];
            count++;
        }
    }

    qsort(results, count, sizeof(classification_t), classify_cmp);
    return count;
}

const char *sem_category_name(query_category_t cat) {
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        if (CATEGORIES[i].cat == cat) return CATEGORIES[i].name;
    }
    return "unknown";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CONTEXT RELEVANCE SCORING
 * ═══════════════════════════════════════════════════════════════════════════ */

static int msg_score_cmp(const void *a, const void *b) {
    double sa = ((const msg_score_t *)a)->final_score;
    double sb = ((const msg_score_t *)b)->final_score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

int sem_score_messages(tfidf_index_t *idx, const char *query,
                       const char **msg_texts, int msg_count,
                       msg_score_t *results, int max_results) {
    /* Vectorize query */
    tfidf_vec_t query_vec;
    sem_tfidf_vectorize(idx, query, &query_vec);

    /* Score each message */
    int count = 0;
    for (int i = 0; i < msg_count && count < max_results; i++) {
        tfidf_vec_t msg_vec;
        sem_tfidf_vectorize(idx, msg_texts[i], &msg_vec);

        msg_score_t *ms = &results[count];
        ms->msg_index = i;
        ms->relevance = sem_cosine_sim(&query_vec, &msg_vec);
        ms->is_recent = (i >= msg_count - 6);  /* last 3 turns (user+assistant pairs) */
        ms->has_tool_result = (strstr(msg_texts[i], "tool_result") != NULL ||
                               strstr(msg_texts[i], "tool_use") != NULL);

        /* Combined score: recency bonus + relevance + tool content bonus */
        ms->final_score = ms->relevance;
        if (ms->is_recent)       ms->final_score += 0.5;
        if (ms->has_tool_result) ms->final_score += 0.2;
        /* First message (initial context) gets a bonus */
        if (i == 0)              ms->final_score += 0.3;

        count++;
    }

    qsort(results, count, sizeof(msg_score_t), msg_score_cmp);
    return count;
}
