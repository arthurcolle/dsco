#ifndef DSCO_PROCESS_CAPTURE_H
#define DSCO_PROCESS_CAPTURE_H
#include "json_util.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char *output;
    size_t length;
    int exit_code;
    int spawn_error;
    bool timed_out;
    bool truncated;
} process_capture_t;
/* Direct argv, isolated stdin, private process group, bounded concurrent drain.
 * Timeout starts TERM/KILL cleanup while retaining bounded final diagnostics.
 * Descendants in the private group are cleaned up even after normal root exit.
 * Signal exits use 128 + signal; -1 means no observed exit status. */
bool process_capture(const char *path, char *const argv[], int timeout_ms,
                     size_t max_output, process_capture_t *out);
/* Like process_capture, with bounded input delivered via an anonymous file;
 * request bytes never appear in argv or a named on-disk artifact. */
bool process_capture_input(const char *path, char *const argv[], const char *input,
                           size_t input_len, int timeout_ms, size_t max_output,
                           process_capture_t *out);
void process_capture_free(process_capture_t *out);
/* Append capture output as valid JSON text. Invalid UTF-8 and embedded NULs are
 * replaced in `output`; an exact `output_base64` sibling is appended when that
 * happens. The caller writes the `"output":` key before calling this helper. */
void process_capture_append_json_output(jbuf_t *buffer,
                                        const process_capture_t *capture);
bool process_capture_json(const char *path, char *const argv[], int timeout_ms,
                          const char *command, char *result, size_t result_len);
#endif
