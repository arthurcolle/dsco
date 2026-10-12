#ifndef DSCO_WORKSPACE_INTAKE_H
#define DSCO_WORKSPACE_INTAKE_H

#include <stdbool.h>
#include <stddef.h>

#define WORKSPACE_INTAKE_MAX_INPUT (128u * 1024u)
#define WORKSPACE_INTAKE_MAX_OUTPUT (128u * 1024u)
#define WORKSPACE_INTAKE_ERROR_CAP 256u

/* Pure, deterministic, locale-independent analysis; no I/O or execution.
 * All inputs are NUL-terminated UTF-8 strings. Embedded NUL bytes cannot be
 * detected by this signature: a length-aware caller MUST reject them before
 * calling. Inputs and result must not overlap. project/source are labels, not
 * paths to open, and are limited to 4096 bytes each. Content may be empty.
 *
 * Success: complete dsco.workspace.intake.v1 JSON, at most MAX_OUTPUT bytes
 * excluding its NUL. Use MAX_OUTPUT + 1 bytes for result. Failure: false and
 * complete JSON {schema,ok:false,error:{code,message}} if it fits; otherwise
 * result is empty (when non-NULL and cap > 0). Never returns partial JSON.
 * ERROR_CAP always fits diagnostic JSON. NULL result/zero cap returns false.
 *
 * Detection: .json/.csv suffixes (ASCII case-insensitive) are authoritative;
 * leading '{'/'[' also selects strict JSON. Otherwise strict rectangular CSV
 * with >=2 records and >=2 columns is recognized, then Markdown/text. JSON
 * scalars require .json. CSV records include the first record: no header is
 * assumed. Ragged explicit CSV is profiled, not silently padded or discarded.
 * Markdown: ATX/setext headings, list checkboxes, uppercase TODO tokens;
 * fenced/indented code is excluded from goal/heading extraction. Association
 * nodes are lexical ASCII email/domain occurrences, not identity/business
 * claims; edges only record containment or an observed email domain.
 *
 * Additional explicit resource limits: 2048 nodes, JSON depth 64, CSV 4096
 * records/256 columns. Exhausting any bound fails the whole operation.
 * Source-derived Markdown punctuation/HTML is entity-escaped, never executed.
 * Structured strings remain data and must be escaped by non-Markdown consumers.
 */
bool workspace_intake_analyze(const char *project, const char *source,
                              const char *content, char *result, size_t cap);

#endif
