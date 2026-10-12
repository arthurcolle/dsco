/* Oversized-result spill. See include/result_spill.h. */

#include "result_spill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "context_fabric.h"
#include "context_archive.h"

static bool spill_enabled(void) {
    const char *v = getenv("DSCO_RESULT_SPILL");
    if (!v || !*v)
        return true; /* on by default: losing bytes is the worse failure */
    return strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 && strcasecmp(v, "off") != 0;
}

/* Cut a preview at a UTF-8 boundary so we never emit a torn multibyte char. */
static size_t utf8_floor(const char *s, size_t want) {
    while (want > 0 && ((unsigned char)s[want] & 0xC0) == 0x80)
        want--;
    return want;
}

static size_t utf8_ceil_start(const char *s, size_t len, size_t from) {
    while (from < len && ((unsigned char)s[from] & 0xC0) == 0x80)
        from++;
    return from;
}

bool result_spill(char *result, size_t cap, const char *label, size_t threshold) {
    /* Retrieval already supplies an exact bounded page and continuation key.
     * Re-spilling a page would invalidate those offsets and create a recall loop. */
    if(label && (!strcmp(label,"context_recall") || !strcmp(label,"ctx_get")))return false;
    if (!result || cap == 0 || !spill_enabled())
        return false;
    if (threshold == 0)
        threshold = RESULT_SPILL_THRESHOLD;
    size_t n = strlen(result);
    if (n <= threshold)
        return false;
    /* Preview layout must fit comfortably; give up (leaving the original
     * intact) rather than produce a preview larger than the input. */
    if (cap < RESULT_SPILL_HEAD + RESULT_SPILL_TAIL + 512)
        return false;

    ctx_put_opts_t opts = {0};
    opts.kind = CTX_KIND_BLOB;
    opts.source = label && label[0] ? label : "tool_result";
    opts.tags = "tool_result,spill";
    opts.importance = 0.4;
    opts.embed = -1; /* auto: index text so ctx_search can find it later */

    ctxkey_t key;
    if (!context_archive_store_verified(result,n,&opts,&key))
        return false;

    char keystr[128];
    if (ctxkey_format(&key, keystr, sizeof(keystr)) < 0)
        return false;

    size_t head = utf8_floor(result, RESULT_SPILL_HEAD);
    size_t tail_start = utf8_ceil_start(result, n, n - RESULT_SPILL_TAIL);
    size_t tail_len = n - tail_start;

    /* Assemble preview into a scratch buffer, then copy over the original. */
    char *out = malloc(head + tail_len + 512);
    if (!out)
        return false;
    size_t w = 0;
    memcpy(out + w, result, head);
    w += head;
    int header = snprintf(out + w, 512,
                          "\n\n[spilled: %zu bytes total from %s; showing first %zu + last %zu. "
                          "Full content: context_recall key=%s — supports #b:START-END for ranges]\n\n",
                          n, opts.source, head, tail_len, keystr);
    if (header < 0 || header >= 512) { free(out); return false; }
    w += (size_t)header;
    memcpy(out + w, result + tail_start, tail_len);
    w += tail_len;
    out[w] = '\0';

    if (w >= cap) { /* paranoid: preview somehow larger than cap */
        free(out);
        return false;
    }
    memcpy(result, out, w + 1);
    free(out);
    return true;
}
