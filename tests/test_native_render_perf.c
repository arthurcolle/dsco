/* Populated native raster benchmark; fixed UI clock, real duration clock. */
#include <time.h>
static double wall_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static int fixture_clock_gettime(clockid_t clock, struct timespec *t) {
    if (clock == CLOCK_MONOTONIC) {
        t->tv_sec = 1000;
        t->tv_nsec = 250000000;
        return 0;
    }
    return clock_gettime(clock, t);
}
#define clock_gettime fixture_clock_gettime
#define main transcript_review_main
#include "test_native_transcript_review.c"
#undef main
#undef clock_gettime

static uint64_t pixel_hash(const px_canvas_t *c) {
    const uint8_t *p = (const uint8_t *)c->pixels;
    size_t bytes = (size_t)c->pixel_width * c->pixel_height * 3;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
}

static px_canvas_t *render_populated(void) {
    return render_session_frame(1476, 830, 1.3, 1920, 1080, "gpt-6-astra", PIXEL_TUI_RESPONDING);
}

static uint64_t render_hash(void) {
    px_canvas_t *c = render_populated();
    CHECK(c != NULL);
    if (!c) return 0;
    uint64_t hash = pixel_hash(c);
    free_canvas(c);
    return hash;
}

static void check_fresh_equivalence(uint64_t prior) {
    uint64_t retained = render_hash();
    visual_cache_invalidate();
    uint64_t rebuilt = render_hash();
    CHECK(retained == rebuilt);
    CHECK(retained != prior);
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void) {
    render_coding_review();
    const char *sentence =
        "The completion transaction must remain atomic when concurrent workers submit the same receipt. "
        "The durable event is written once, while retries return the existing result without another side effect. "
        "This keeps **lease renewal and recovery** consistent with the worker’s original request. ";
    char paragraph[8192] = {0};
    for (int i = 0; i < 18; ++i)
        strcat(paragraph, sentence);
    strcat(paragraph, "Final receipt: AAAA.");
    pixel_message_t *latest = coding_message("ASSISTANT", paragraph);
    CHECK(latest != NULL);
    visual_cache_invalidate();
    double start = wall_ms();
    px_canvas_t *first = render_populated();
    double first_ms = wall_ms() - start;
    CHECK(first != NULL);
    if (!first) return 1;
    uint64_t reference = pixel_hash(first);
    const char *artifact = getenv("DSCO_RENDER_PERF_ARTIFACT");
    if (artifact && *artifact) CHECK(canvas_write_ppm(artifact, first));
    free_canvas(first);
    double samples[100];
    for (int i = 0; i < 100; ++i) {
        start = wall_ms();
        px_canvas_t *c = render_populated();
        samples[i] = wall_ms() - start;
        CHECK(c != NULL);
        if (c) {
            CHECK(pixel_hash(c) == reference);
            free_canvas(c);
        }
    }
    qsort(samples, 100, sizeof(samples[0]), compare_double);
    printf("{\"physical_width\":1920,\"physical_height\":1080,\"scale\":1.3,"
           "\"newest_text_bytes\":%zu,\"frames\":100,\"first_uncached_ms\":%.3f,"
           "\"median_ms\":%.3f,\"p95_ms\":%.3f,\"max_ms\":%.3f,"
           "\"pixel_hash\":\"%016llx\"}\n", latest->text_len, first_ms,
           (samples[49] + samples[50]) * 0.5, samples[94], samples[99],
           (unsigned long long)reference);

    /* The newest row cache must track content and presentation, not length
     * alone. Compare retained rendering to an explicitly rebuilt reference. */
    size_t bytes = latest->text_len;
    char *end = strstr(paragraph, "AAAA");
    CHECK(end != NULL);
    if (end) memcpy(end, "BBBB", 4);
    CHECK(message_text_set_plain(latest, paragraph) && latest->text_len == bytes);
    check_fresh_equivalence(reference);
    reference = render_hash();
    latest->streaming = true;
    check_fresh_equivalence(reference);

    coding_tool("python3 -m unittest", "test_duplicate_completion ... FAIL", false, 17);
    latest = &g_session.messages[g_session.current_message];
    reference = render_hash();
    latest->tool_status = 1;
    check_fresh_equivalence(reference);
    reset_session();
    printf("native populated render: %d failures\n", failures);
    return failures != 0;
}
