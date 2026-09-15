#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "skill_trace.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRACE_LIMIT (64u * 1024u * 1024u)
#define FRAME_LIMIT (16u * 1024u * 1024u)
#define SELECT_LIMIT 256u
static uint32_t u32le(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t frame_crc(const unsigned char *p, size_t len) {
    uint32_t crc = ~0u;
    for (size_t i=0; i<len; i++) {
        crc ^= p[i];
        for (int b=0; b<8; b++) crc = (crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static const char *safe_status(yyjson_val *value) {
    const char *values[] = {"ok", "succeeded", "failed", "error", "timeout", "denied", "cancelled"};
    for (size_t i=0; i<sizeof(values)/sizeof(*values); i++)
        if (yyjson_equals_str(value,values[i])) return values[i];
    return "unknown";
}
static int trace_report(const char *path, char **report) {
    *report = NULL;
    int fd = open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if (fd<0) { fputs("dsco learn: cannot open trace\n",stderr); return 1; }
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>TRACE_LIMIT) {
        close(fd); fputs("dsco learn: trace must be a nonempty regular file <=64 MiB\n",stderr); return 1;
    }
    size_t len=(size_t)st.st_size, got=0;
    unsigned char *data=malloc(len);
    if (!data) { close(fd); return 1; }
    while (got<len) {
        ssize_t n=read(fd,data+got,len-got);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        got+=(size_t)n;
    }
    close(fd);
    if (got!=len) { free(data); fputs("dsco learn: incomplete trace read\n",stderr); return 1; }
    char source_hash[65]; sha256_hex(data,len,source_hash);
    jbuf_t selected; jbuf_init(&selected,1024);
    size_t offset=0, frames=0, tools=0;
    bool valid=true;
    while (offset<len) {
        if (len-offset<8) { valid=false; break; }
        uint32_t n=u32le(data+offset), crc=u32le(data+offset+4);
        if (!n || n>FRAME_LIMIT || (size_t)n>len-offset-8 || frame_crc(data+offset+8,n)!=crc) { valid=false; break; }
        yyjson_doc *doc=yyjson_read((const char *)data+offset+8,n,0);
        yyjson_val *root=doc ? yyjson_doc_get_root(doc):NULL;
        yyjson_val *seq=yyjson_obj_get(root,"seq");
        if (!yyjson_is_obj(root) || !yyjson_equals_str(yyjson_obj_get(root,"type"),"rl.trajectory.event")) {
            /* Non-trajectory frames still require valid JSON and a v1 envelope. */
            if (!yyjson_is_obj(root)) valid=false;
        }
        yyjson_val *version=yyjson_obj_get(root,"v");
        if (!yyjson_is_uint(version) || yyjson_get_uint(version)!=1 || !yyjson_is_uint(seq) ||
            !yyjson_is_str(yyjson_obj_get(root,"type")) || !yyjson_is_str(yyjson_obj_get(root,"run_id"))) valid=false;
        if (!valid) { yyjson_doc_free(doc); break; }
        yyjson_val *payload=yyjson_obj_get(root,"payload");
        if (yyjson_equals_str(yyjson_obj_get(root,"type"),"rl.trajectory.event") &&
            yyjson_equals_str(yyjson_obj_get(payload,"category"),"tool")) {
            if (tools<SELECT_LIMIT) {
                char hash[65]; sha256_hex(data+offset+8,n,hash);
                if (tools) jbuf_append(&selected,",");
                jbuf_appendf(&selected,"{\"frame\":%zu,\"offset_bytes\":%zu,\"sequence\":%llu,\"frame_sha256\":",
                             frames,offset,(unsigned long long)yyjson_get_uint(seq));
                jbuf_append_json_str(&selected,hash);
                jbuf_appendf(&selected,",\"evidence_ref\":\"chronicle:%s:%s\"",source_hash,hash);
                jbuf_append(&selected,",\"status\":");
                jbuf_append_json_str(&selected,safe_status(yyjson_obj_get(payload,"status")));
                jbuf_append(&selected,",\"payload_exported\":false}");
            }
            tools++;
        }
        frames++; offset+=8+(size_t)n; yyjson_doc_free(doc);
    }
    free(data);
    if (!valid) {
        jbuf_free(&selected); fputs("dsco learn: invalid, corrupt, unsupported or truncated journal; no evidence exported\n",stderr); return 1;
    }
    jbuf_t output; jbuf_init(&output, selected.len + 1024);
    jbuf_appendf(&output, "{\"schema\":\"dsco.skill_trace.v1\",\"source_sha256\":\"%s\",\"snapshot_bytes\":%zu,\"journal_frames\":%zu,\"tool_events\":%zu,\"selection_truncated\":%s,\"events\":[%s],\"redaction\":\"omit_all_payloads_and_free_text\",\"extraction_ready\":false,\"reason\":\"observable_payload_review_and_acceptance_evidence_required\",\"promotion_eligible\":false}\n",
           source_hash,len,frames,tools,tools>SELECT_LIMIT ? "true":"false",selected.data);
    *report = strdup(output.data);
    jbuf_free(&output); jbuf_free(&selected); return *report ? 0 : 1;
}

int skill_trace_cli(const char *path) {
    char *report = NULL;
    int rc = trace_report(path, &report);
    if (!rc) fputs(report, stdout);
    free(report);
    return rc;
}

bool skill_trace_resolve_evidence(const char *path, const char *const *refs, size_t count) {
    if (!refs || !count || count > SELECT_LIMIT) return false;
    /* Validate references before touching the source. Never interpret a URI as
     * a network destination; it is only two lowercase content hashes. */
    for (size_t i = 0; i < count; i++) {
        const char *r = refs[i];
        if (!r || strlen(r) != 139 || strncmp(r, "chronicle:", 10) || r[74] != ':') return false;
        for (size_t j = 10; j < 139; j++) {
            if (j == 74) continue;
            if (!((r[j] >= '0' && r[j] <= '9') || (r[j] >= 'a' && r[j] <= 'f'))) return false;
        }
    }
    char *report = NULL;
    if (trace_report(path, &report)) return false;
    yyjson_doc *doc = yyjson_read(report, strlen(report), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *source = yyjson_get_str(yyjson_obj_get(root, "source_sha256"));
    yyjson_val *events = yyjson_obj_get(root, "events");
    bool ok = source && yyjson_is_arr(events);
    for (size_t i = 0; ok && i < count; i++) {
        if (strncmp(refs[i] + 10, source, 64)) { ok = false; break; }
        bool found = false;
        size_t j, n; yyjson_val *event;
        yyjson_arr_foreach(events, j, n, event)
            if (yyjson_equals_str(yyjson_obj_get(event, "frame_sha256"), refs[i] + 75)) found = true;
        if (!found) ok = false;
    }
    yyjson_doc_free(doc); free(report);
    return ok;
}
