#include "swarm_scale.h"
#include "json_util.h"
#include "crypto.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE_LOGICAL_MAX 4096
#define SCALE_INPUT_MAX (8U * 1024U * 1024U)

static bool fail(char *out, size_t cap, const char *reason) {
    if (out && cap) snprintf(out, cap, "{\"error\":\"%s\",\"submitted\":false}", reason);
    return false;
}

/* Reject duplicate keys and embedded NULs before another parser sees the request.
 * Bounds make validation independent of pathological user object depth/width. */
static bool valid(yyjson_val *v, int depth) {
    if (depth > 32) return false;
    if (yyjson_is_str(v)) return strlen(yyjson_get_str(v)) == yyjson_get_len(v);
    size_t i, n; yyjson_val *k, *x;
    if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 128) return false;
        yyjson_obj_foreach(v, i, n, k, x) {
            if (!valid(k, depth+1) || !valid(x, depth+1)) return false;
            size_t j, m; yyjson_val *other, *value;
            yyjson_obj_foreach(v, j, m, other, value) {
                (void)value;
                if (j >= i) break;
                if (!strcmp(yyjson_get_str(k), yyjson_get_str(other))) return false;
            }
        }
    } else if (yyjson_is_arr(v)) {
        if (yyjson_arr_size(v) > SCALE_LOGICAL_MAX) return false;
        yyjson_arr_foreach(v, i, n, x) if (!valid(x, depth+1)) return false;
    }
    return true;
}
static bool native_executor(yyjson_val *v) {
    yyjson_val *e = yyjson_obj_get(v, "executor");
    return !e || (yyjson_is_str(e) && !strcmp(yyjson_get_str(e), "dsco"));
}
static uint64_t fingerprint(const char *s) {
    unsigned char digest[32];
    sha256_ctx_t ctx; sha256_init(&ctx);
    sha256_update(&ctx,(const unsigned char *)s,strlen(s)); sha256_final(&ctx,digest);
    uint64_t value; memcpy(&value, digest, sizeof(value)); return value;
}

bool swarm_scale_execute(const char *input, const char *tier, int physical_limit,
                         char *out, size_t cap) {
    if (!out || !cap) return false;
    if (!input || !tier || !*tier || physical_limit < 1 ||
        strlen(input) > SCALE_INPUT_MAX) return fail(out,cap,"invalid scale request or authority context");
    yyjson_doc *doc=yyjson_read(input,strlen(input),0);
    yyjson_val *root=doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root) || !valid(root,0)) {
        if (doc) yyjson_doc_free(doc);
        return fail(out,cap,"invalid JSON, duplicate keys, NUL, or request bounds");
    }
    yyjson_val *pure=yyjson_obj_get(root,"pure_tasks"), *dry=yyjson_obj_get(root,"dry_run");
    yyjson_val *tasks=yyjson_obj_get(root,"tasks");
    size_t count=yyjson_arr_size(tasks);
    if (!yyjson_is_true(pure) || (dry && !yyjson_is_bool(dry)) ||
        !yyjson_is_arr(tasks) || !count || count>SCALE_LOGICAL_MAX || !native_executor(root) ||
        yyjson_obj_get(root,"coordinator") || yyjson_obj_get(root,"operation")) {
        yyjson_doc_free(doc);
        return fail(out,cap,"scale requires pure_tasks:true and 1..4096 native tasks; supports create only");
    }
    size_t slots=1; while (slots<count*2) slots*=2;
    size_t *table=calloc(slots,sizeof(*table)), *aliases=calloc(count,sizeof(*aliases));
    char **keys=calloc(count,sizeof(*keys));
    yyjson_mut_doc *request=yyjson_doc_mut_copy(doc,NULL);
    if (!table || !aliases || !keys || !request) {
        free(table);free(aliases);free(keys);if(request)yyjson_mut_doc_free(request);yyjson_doc_free(doc);
        return fail(out,cap,"scale allocation failed");
    }
    yyjson_mut_val *req=yyjson_mut_doc_get_root(request), *unique=yyjson_mut_arr(request);
    size_t used=0; bool ok=false; const char *error=NULL;
    size_t i,n; yyjson_val *item;
    yyjson_arr_foreach(tasks,i,n,item) {
        yyjson_val *task=yyjson_is_obj(item) ? yyjson_obj_get(item,"task") : item;
        if (!yyjson_is_str(task) || !yyjson_get_len(task) ||
            (yyjson_is_obj(item) && !native_executor(item))) { error="invalid task or non-native executor"; goto done; }
        char *key=yyjson_val_write(item,0,NULL);
        if (!key) { error="scale allocation failed"; goto done; }
        size_t slot=(size_t)fingerprint(key)&(slots-1);
        while (table[slot] && strcmp(keys[table[slot]-1],key)) slot=(slot+1)&(slots-1);
        if (table[slot]) { aliases[i]=table[slot]-1; free(key); }
        else {
            if (used >= (size_t)physical_limit) { free(key); error="unique tasks exceed physical worker limit; no workers submitted"; goto done; }
            keys[used]=key; table[slot]=used+1; aliases[i]=used++;
            yyjson_mut_val *copy=yyjson_val_mut_copy(request,item);
            if (!copy || !unique || !yyjson_mut_arr_append(unique,copy)) { error="scale allocation failed"; goto done; }
        }
    }
    yyjson_mut_obj_remove_key(req,"action"); yyjson_mut_obj_remove_key(req,"tasks");
    yyjson_mut_obj_remove_key(req,"pure_tasks"); yyjson_mut_obj_remove_key(req,"dry_run");
    if (!yyjson_mut_obj_add_str(request,req,"action","create") ||
        !yyjson_mut_obj_add_val(request,req,"tasks",unique)) { error="scale allocation failed"; goto done; }
    char *wire=yyjson_mut_write(request,0,NULL);
    if (!wire) { error="scale allocation failed"; goto done; }
    jbuf_t meta; jbuf_init(&meta,1024);
    jbuf_appendf(&meta,"{\"schema\":\"dsco.swarm_scale.v1\",\"logical_tasks\":%zu,\"unique_tasks\":%zu,\"reused_tasks\":%zu,\"work_ratio\":%.8f,\"logical_to_unique_task\":[",count,used,count-used,(double)used/count);
    for (i=0;i<count;i++) jbuf_appendf(&meta,"%s%zu",i ? "," : "",aliases[i]);
    jbuf_append(&meta,"],\"cost_basis\":\"unmeasured; dispatch count is not billed cost\",\"reuse_scope\":\"this request only; successful unique results may be shared, failures remain failures\"");
    if (yyjson_is_true(dry)) {
        jbuf_append(&meta,",\"submitted\":false,\"planned_request\":");jbuf_append(&meta,wire);jbuf_append(&meta,"}");
        if (meta.len>=cap) error="scale plan exceeds output capacity; no workers submitted";
        else { memcpy(out,meta.data,meta.len+1);ok=true; }
    } else if (meta.len+count*12+2048>=cap) error="scale receipt exceeds output capacity; no workers submitted";
    else {
        char *child=calloc(1,cap);
        if (!child) error="scale allocation failed";
        else {
            /* Re-enter normal trust/approval/budget gates. Never call tool bodies. */
            bool accepted=tools_execute_for_tier("swarm",wire,tier,child,cap);
            yyjson_doc *reply=yyjson_read(child,strlen(child),0);
            yyjson_val *body=reply ? yyjson_doc_get_root(reply) : NULL;
            yyjson_val *ids=yyjson_obj_get(body,"agent_ids");
            bool mapped=accepted && yyjson_is_arr(ids) && yyjson_arr_size(ids)==used;
            for (size_t j=0;mapped && j<used;j++)
                mapped=yyjson_is_int(yyjson_arr_get(ids,j)) && yyjson_get_sint(yyjson_arr_get(ids,j))>=0;
            jbuf_append(&meta,",\"logical_worker_ids\":");
            if (mapped) {
                jbuf_append(&meta,"[");
                for (size_t j=0;j<count;j++) jbuf_appendf(&meta,"%s%lld",j?",":"",(long long)yyjson_get_sint(yyjson_arr_get(ids,aliases[j])));
                jbuf_append(&meta,"]");
            } else jbuf_append(&meta,"null");
            jbuf_appendf(&meta,",\"result_reconciliation_required\":%s",mapped ? "false" : "true");
            jbuf_append(&meta,",\"reference_scope\":\"live group; collect and retain results before worker IDs are reused; failure is shared, not success\"");
            size_t prefix=meta.len;
            jbuf_appendf(&meta,",\"delegated\":true,\"dispatch_accepted\":%s,\"execution\":",accepted ? "true" : "false");
            if (body) jbuf_append(&meta,child); else jbuf_append_json_str(&meta,child);
            jbuf_append(&meta,"}");
            if (meta.len>=cap) {
                /* A spawn may already have happened: preserve a collectable handle
                 * rather than returning a retryable size error after side effects. */
                meta.len=prefix; meta.data[prefix]='\0';
                yyjson_val *gid=yyjson_obj_get(body,"group_id");
                jbuf_appendf(&meta,",\"delegated\":true,\"dispatch_accepted\":%s,\"execution_response_omitted\":true,\"reconcile_before_retry\":true,\"group_id\":",accepted ? "true" : "false");
                if (yyjson_is_int(gid)) jbuf_appendf(&meta,"%lld",(long long)yyjson_get_sint(gid));
                else jbuf_append(&meta,"null");
                jbuf_append(&meta,"}");
            }
            if (reply) yyjson_doc_free(reply);
            if (meta.len<cap) memcpy(out,meta.data,meta.len+1);
            else snprintf(out,cap,"{\"delegated\":true,\"dispatch_accepted\":%s,\"reconcile_before_retry\":true}",accepted?"true":"false");
            ok=accepted;free(child);
        }
    }
    jbuf_free(&meta);free(wire);
done:
    for (i=0;i<used;i++) free(keys[i]);
    free(keys);free(table);free(aliases);yyjson_mut_doc_free(request);yyjson_doc_free(doc);
    if (error) return fail(out,cap,error);
    return ok;
}
