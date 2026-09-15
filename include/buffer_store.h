#ifndef DSCO_BUFFER_STORE_H
#define DSCO_BUFFER_STORE_H
#include <stdbool.h>
#include <stddef.h>

#define BUFFER_DESCRIPTION \
    "Persistent named UTF-8 buffers independent of terminal views; never launches a process. " \
    "Kinds: scratch, file (imports source_path), log. Use buffer_id or name to select an existing buffer; " \
    "names are unique within workspace. Each response includes the SHA256 revision of the actual " \
    "owned content file, including external editor changes. Write requires expected_revision; append " \
    "accepts optional expected_revision. Create/fork/write/append accept request_id for exact retry " \
    "deduplication. Read returns bounded text plus lossless base64 and byte offsets. Close retains " \
    "content; reopen restores editing. Save writes path, or the remembered source_path when omitted, after checking " \
    "the imported/saved source revision (or expected_source_revision for an existing new target). " \
    "Use new_name for rename/fork. Create accepts sensitive=true, retained on forks. " \
    "Content is limited to 1 MiB; read pages to 32768 bytes; a workspace retains up to 256 buffers " \
    "and 4096 mutation retry records. No keychain management or arbitrary deletion."

#define BUFFER_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"list\",\"inspect\",\"read\",\"write\",\"append\",\"rename\",\"fork\",\"close\",\"reopen\",\"save\"]}," \
    "\"workspace\":{\"type\":\"string\",\"maxLength\":63}," \
    "\"buffer_id\":{\"type\":\"string\",\"maxLength\":36}," \
    "\"name\":{\"type\":\"string\",\"maxLength\":160}," \
    "\"new_name\":{\"type\":\"string\",\"maxLength\":160}," \
    "\"kind\":{\"type\":\"string\",\"enum\":[\"scratch\",\"file\",\"log\"]}," \
    "\"content\":{\"type\":\"string\",\"maxLength\":1048576}," \
    "\"source_path\":{\"type\":\"string\",\"maxLength\":4095}," \
    "\"path\":{\"type\":\"string\",\"maxLength\":4095}," \
    "\"expected_revision\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}," \
    "\"expected_source_revision\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64}," \
    "\"request_id\":{\"type\":\"string\",\"maxLength\":80}," \
    "\"sensitive\":{\"type\":\"boolean\"}," \
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1048576}," \
    "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768}" \
    "},\"required\":[\"action\"]}"

bool tool_buffer(const char *input_json, char *result, size_t result_len);
/* Read-only sensitivity lookup. Returns true on malformed/inaccessible metadata
 * so a damaged registry cannot lower a required secrets capability. */
bool buffer_store_sensitive(const char *input_json);
#endif
