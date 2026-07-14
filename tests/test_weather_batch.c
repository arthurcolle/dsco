/* Focused native weather-batch tests. Build with:
 * cc -std=c2y -Iinclude tests/test_weather_batch.c src/weather_batch.c
 *    src/json_util.c src/http_pool.c -lcurl -o /tmp/test_weather_batch
 */
#include "weather_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(bool condition, const char *message) {
    if (condition)
        return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    char result[512] = {0};
    int failures = 0;

    /* Validation deliberately precedes credential lookup: malformed input must
     * fail locally and never create a network request. */
    failures += check(!tool_weather_batch("{\"locations\":[]}", result, sizeof(result)),
                      "empty batch must fail");
    failures += check(strstr(result, "requires locations") != NULL,
                      "empty batch must have actionable error");

    memset(result, 0, sizeof(result));
    failures += check(!tool_weather_batch("{\"locations\":[17]}", result, sizeof(result)),
                      "non-string location must fail");
    failures += check(strstr(result, "requires locations") != NULL,
                      "non-string location must have actionable error");

    unsetenv("OPENWEATHERMAP_API_KEY");
    memset(result, 0, sizeof(result));
    failures += check(!tool_weather_batch("{\"locations\":[\"20002, US\"]}", result,
                                          sizeof(result)),
                      "valid batch without key must fail");
    failures += check(strstr(result, "OPENWEATHERMAP_API_KEY") != NULL,
                      "missing key must name required environment variable");

    if (failures)
        return 1;
    puts("PASS: weather_batch native validation");
    return 0;
}
