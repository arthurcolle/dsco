#include "lingo_graphsub_world.h"
#include "crypto.h"
#include "http_pool.h"
#include "service_boundary.h"
#include "../vendor/yyjson.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define BODY_LIMIT (128u * 1024u)
#define INPUT_LIMIT (128u * 1024u)
#define SNAPSHOT_LIMIT 49152u
#define SAFE_INTEGER 9007199254740991LL
const char lingo_graphsub_world_schema[] = "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"publish\",\"read\"]},\"connection\":{\"enum\":[\"default\"],\"default\":\"default\"},\"shard_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2147483647,\"default\":0},\"snapshot\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":49152,\"description\":\"Exact JSON text of lingo.world/1; additionally limited to 49152 UTF-8 bytes.\"},\"node_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":256,\"pattern\":\"^[A-Za-z0-9_-]+$\",\"not\":{\"pattern\":\"[^A-Za-z0-9_-]\"}},\"sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[a-f0-9]{64}$\"},\"world_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":128}},\"required\":[\"action\"],\"additionalProperties\":false,\"oneOf\":[{\"properties\":{\"action\":{\"const\":\"publish\"}},\"required\":[\"snapshot\"],\"not\":{\"anyOf\":[{\"required\":[\"node_id\"]},{\"required\":[\"sha256\"]},{\"required\":[\"world_id\"]}]}},{\"properties\":{\"action\":{\"const\":\"read\"}},\"required\":[\"node_id\",\"sha256\",\"world_id\"],\"not\":{\"required\":[\"snapshot\"]}}]}";
const char lingo_graphsub_world_output_schema[] = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"profile\":{\"const\":\"world_artifact\"},\"action\":{\"enum\":[\"publish\",\"read\"]},\"consistency\":{\"const\":\"content_verified\"},\"persistence\":{\"const\":\"server_reported\"},\"artifact\":{\"type\":\"object\",\"properties\":{\"connection\":{\"enum\":[\"default\"],\"default\":\"default\"},\"shard_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2147483647,\"default\":0},\"node_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":256,\"pattern\":\"^[A-Za-z0-9_-]+$\",\"not\":{\"pattern\":\"[^A-Za-z0-9_-]\"}},\"sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[a-f0-9]{64}$\"},\"world_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":128}},\"required\":[\"connection\",\"shard_id\",\"node_id\",\"sha256\",\"world_id\"],\"additionalProperties\":false},\"snapshot\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":49152,\"description\":\"Exact JSON text of lingo.world/1; additionally limited to 49152 UTF-8 bytes.\"},\"before\":{\"type\":\"object\"},\"after\":{\"type\":\"object\"},\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"effect\":{\"enum\":[\"none\",\"unknown\"]},\"http_status\":{\"type\":\"integer\"},\"artifact\":{\"type\":\"object\",\"properties\":{\"connection\":{\"enum\":[\"default\"],\"default\":\"default\"},\"shard_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2147483647,\"default\":0},\"node_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":256,\"pattern\":\"^[A-Za-z0-9_-]+$\",\"not\":{\"pattern\":\"[^A-Za-z0-9_-]\"}},\"sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[a-f0-9]{64}$\"},\"world_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":128}},\"required\":[\"connection\",\"shard_id\",\"node_id\",\"sha256\",\"world_id\"],\"additionalProperties\":false}},\"required\":[\"code\",\"message\",\"effect\"],\"additionalProperties\":false}},\"required\":[\"ok\"],\"additionalProperties\":false,\"oneOf\":[{\"properties\":{\"ok\":{\"const\":true}},\"required\":[\"profile\",\"action\",\"consistency\",\"persistence\",\"artifact\",\"snapshot\",\"before\",\"after\"],\"not\":{\"required\":[\"error\"]}},{\"properties\":{\"ok\":{\"const\":false}},\"required\":[\"error\"],\"not\":{\"required\":[\"snapshot\"]}}]}";

static bool text_is(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
           !memcmp(yyjson_get_str(v), s, strlen(s));
}

static bool uint_arg(yyjson_val *v, uint64_t min, uint64_t max, unsigned *out) {
    if (!v)
        return true;
    double n = yyjson_get_num(v);
    if (!yyjson_is_num(v) || !isfinite(n) || floor(n) != n || n < (double)min || n > (double)max)
        return false;
    *out = (unsigned)n;
    return true;
}

static bool native_id(yyjson_val *v) {
    if (!yyjson_is_str(v) || !yyjson_get_len(v) || yyjson_get_len(v) > 256)
        return false;
    const unsigned char *s = (const unsigned char *)yyjson_get_str(v);
    for (size_t i = 0; i < yyjson_get_len(v); ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}

typedef struct {
    char *data;
    size_t length;
    bool too_large;
} body_t;

static size_t receive(void *data, size_t size, size_t count, void *opaque) {
    body_t *body = opaque;
    if ((size && count > BODY_LIMIT / size) || size * count > BODY_LIMIT - body->length) {
        body->too_large = true;
        return 0;
    }
    size_t n = size * count;
    memcpy(body->data + body->length, data, n);
    body->length += n;
    body->data[body->length] = '\0';
    return n;
}

static bool loopback(const char *host) {
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, host, &v4) == 1)
        return (ntohl(v4.s_addr) >> 24) == 127;
    if (!strcmp(host, "[::1]") || !strcmp(host, "::1"))
        return true;
    return inet_pton(AF_INET6, host, &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}

/* Only an origin is configurable. No userinfo, base path, query or fragment.
 * localhost over HTTP is pinned numerically so DNS cannot move it off-host. */
static CURLU *configured_origin(const char *action) {
    const char *base = getenv("GRAPHSUB_HOST");
    if (!base)
        base = "http://127.0.0.1:7879";
    size_t length = strnlen(base, 2049);
    if (!length || length > 2048 || strchr(base, '?') || strchr(base, '#') ||
        (strncmp(base, "http://", 7) && strncmp(base, "https://", 8)))
        return NULL;
    const char *raw_path = strchr(base + (!strncmp(base, "https://", 8) ? 8 : 7), '/');
    if (raw_path && strcmp(raw_path, "/"))
        return NULL;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)base[i] <= 32 || (unsigned char)base[i] == 127)
            return NULL;
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL, *path = NULL;
    bool valid = url && curl_url_set(url, CURLUPART_URL, base, CURLU_DISALLOW_USER) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && !strcmp(path, "/");
    if (valid && !strcmp(scheme, "http")) {
        if (!strcasecmp(host, "localhost"))
            valid = curl_url_set(url, CURLUPART_HOST, "127.0.0.1", 0) == CURLUE_OK;
        else
            valid = loopback(host);
    } else if (valid)
        valid = !strcmp(scheme, "https");
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    if (!valid) {
        curl_url_cleanup(url);
        return NULL;
    }
    if (!service_action_destination_allowed("graphsub_world", action, base)) {
        curl_url_cleanup(url);
        return NULL;
    }
    return url;
}

/* Reject values that LuaJIT/JSON consumers cannot carry without changing an
 * integer. BIGNUM_AS_RAW prevents yyjson converting oversized integers first. */
typedef enum { RESPONSE_VALID, RESPONSE_MALFORMED, RESPONSE_PRECISION } response_check;

static response_check validate_json(yyjson_val *v, unsigned depth) {
    if (depth > 64)
        return RESPONSE_MALFORMED;
    if (yyjson_is_raw(v))
        return RESPONSE_PRECISION;
    if (yyjson_is_uint(v))
        return yyjson_get_uint(v) <= (uint64_t)SAFE_INTEGER ? RESPONSE_VALID : RESPONSE_PRECISION;
    if (yyjson_is_sint(v))
        return yyjson_get_sint(v) >= -SAFE_INTEGER && yyjson_get_sint(v) <= SAFE_INTEGER
                   ? RESPONSE_VALID : RESPONSE_PRECISION;
    if (yyjson_is_real(v))
        return isfinite(yyjson_get_real(v)) && fabs(yyjson_get_real(v)) <= (double)SAFE_INTEGER
                   ? RESPONSE_VALID : RESPONSE_PRECISION;
    size_t i, count;
    yyjson_val *key, *value;
    if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v, i, count, value) {
            response_check check = validate_json(value, depth + 1);
            if (check != RESPONSE_VALID)
                return check;
        }
    } else if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 4096)
            return RESPONSE_MALFORMED;
        yyjson_obj_foreach(v, i, count, key, value) {
            const char *name = yyjson_get_str(key);
            if (strlen(name) != yyjson_get_len(key) || yyjson_obj_get(v, name) != value)
                return RESPONSE_MALFORMED;
            response_check check = validate_json(value, depth + 1);
            if (check != RESPONSE_VALID)
                return check;
        }
    }
    return RESPONSE_VALID;
}

static bool response_text(yyjson_val *v) {
    return yyjson_is_str(v) && yyjson_get_len(v) > 0 &&
           strlen(yyjson_get_str(v)) == yyjson_get_len(v);
}

static bool response_uint(yyjson_val *v) {
    return yyjson_is_num(v) && yyjson_get_num(v) >= 0 &&
           floor(yyjson_get_num(v)) == yyjson_get_num(v);
}


typedef struct {
    CURLU *url;
    struct curl_slist *headers;
    const char *code, *message;
    long status;
    bool may_have_published;
} transport_t;

static yyjson_doc *request_json(transport_t *t, const char *path, const char *post) {
    yyjson_doc *doc = NULL;
    CURL *easy = curl_easy_init();
    body_t body = {0};
    body.data = malloc(BODY_LIMIT + 1);
    t->status = 0;
    t->code = "transport_error"; t->message = "GraphSub request failed";
    if (!easy || !body.data) { t->code = "allocation_failed"; t->message = "Cannot allocate transport"; goto done; }
    body.data[0] = '\0'; dsco_http_pool_apply(easy);
    if (curl_url_set(t->url, CURLUPART_PATH, path, 0) != CURLUE_OK) goto done;
#define SET(option, value) do { if (curl_easy_setopt(easy, option, value) != CURLE_OK) goto done; } while (0)
    SET(CURLOPT_CURLU, t->url); SET(CURLOPT_HTTPHEADER, t->headers);
    SET(CURLOPT_WRITEFUNCTION, receive); SET(CURLOPT_WRITEDATA, &body);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 3000L); SET(CURLOPT_TIMEOUT_MS, 10000L);
    SET(CURLOPT_NOSIGNAL, 1L); SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_PROXY, ""); SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    SET(CURLOPT_PATH_AS_IS, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    SET(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    if (post) { SET(CURLOPT_POST, 1L); SET(CURLOPT_POSTFIELDS, post); SET(CURLOPT_POSTFIELDSIZE, (long)strlen(post)); }
    else SET(CURLOPT_HTTPGET, 1L);
#undef SET
    if (post) t->may_have_published = true;
    CURLcode rc = curl_easy_perform(easy);
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &t->status);
    if (body.too_large) { t->code = "response_too_large"; t->message = "GraphSub response exceeds 128 KiB"; goto done; }
    if (rc != CURLE_OK) {
        if (rc == CURLE_OPERATION_TIMEDOUT) { t->code = "timeout"; t->message = "GraphSub request timed out"; }
        goto done;
    }
    if (t->status < 200 || t->status >= 300) { t->code = "http_error"; t->message = "GraphSub returned an unsuccessful HTTP status"; goto done; }
    doc = yyjson_read(body.data, body.length, YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_val *value = doc ? yyjson_doc_get_root(doc) : NULL;
    response_check check = validate_json(value, 0);
    if (!yyjson_is_obj(value) || check != RESPONSE_VALID) {
        t->code = check == RESPONSE_PRECISION ? "precision_unsupported" : "malformed_response";
        t->message = "GraphSub returned invalid or inexact JSON";
        yyjson_doc_free(doc); doc = NULL;
    }
done:
    curl_easy_cleanup(easy); free(body.data); return doc;
}

static bool hash_value(yyjson_val *v) {
    if (!yyjson_is_str(v) || yyjson_get_len(v) != 64) return false;
    const char *p = yyjson_get_str(v);
    for (size_t i = 0; i < 64; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}

static bool bounded_text(yyjson_val *v, size_t limit) {
    if (!response_text(v) || yyjson_get_len(v) > limit) return false;
    const unsigned char *p = (const unsigned char *)yyjson_get_str(v);
    for (size_t i = 0; i < yyjson_get_len(v); i++) if (p[i] < 32 || p[i] == 127) return false;
    return true;
}

static bool snapshot_valid(yyjson_val *v, const char *world_id) {
    yyjson_val *runtime = yyjson_obj_get(v, "runtime");
    yyjson_val *objects = yyjson_obj_get(v, "objects");
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) != 5 ||
        !text_is(yyjson_obj_get(v, "format"), "lingo.world/1") ||
        !bounded_text(yyjson_obj_get(v, "world_id"), 128) ||
        (world_id && !text_is(yyjson_obj_get(v, "world_id"), world_id)) ||
        !yyjson_is_obj(runtime) || (yyjson_obj_size(runtime) != 3 && yyjson_obj_size(runtime) != 4) ||
        (yyjson_obj_size(runtime) == 4 && !bounded_text(yyjson_obj_get(runtime, "compiler"), 128)) ||
        !text_is(yyjson_obj_get(runtime, "api"), "0.2") ||
        !hash_value(yyjson_obj_get(runtime, "runtime_sha256")) ||
        !hash_value(yyjson_obj_get(runtime, "world_io_sha256")) ||
        !yyjson_is_obj(yyjson_obj_get(v, "classes")) || !yyjson_is_arr(objects) ||
        validate_json(v, 0) != RESPONSE_VALID) return false;
    size_t i, count; yyjson_val *object;
    yyjson_arr_foreach(objects, i, count, object) {
        if (!yyjson_is_obj(object) || yyjson_obj_size(object) != 4 ||
            !bounded_text(yyjson_obj_get(object, "id"), 1024) ||
            !bounded_text(yyjson_obj_get(object, "class"), 1024) ||
            !response_uint(yyjson_obj_get(object, "revision")) || yyjson_get_num(yyjson_obj_get(object, "revision")) < 1 ||
            !yyjson_is_obj(yyjson_obj_get(object, "values"))) return false;
    }
    return true;
}

static bool identity_matches(yyjson_val *object, unsigned shard, const char *expected) {
    yyjson_val *id = yyjson_obj_get(object, "id"), *number = yyjson_obj_get(object, "node_id");
    if (!native_id(id) || !response_uint(number) || yyjson_get_num(number) > UINT32_MAX ||
        (expected && !text_is(id, expected))) return false;
    char plain[80], encoded[128];
    int length = snprintf(plain, sizeof(plain), "s%un%u", shard, (unsigned)yyjson_get_num(number));
    size_t n = base64url_encode((const uint8_t *)plain, (size_t)length, encoded, sizeof(encoded));
    return n && text_is(id, encoded);
}

static bool persistence_valid(yyjson_val *v, unsigned shard) {
    yyjson_val *manifest = yyjson_obj_get(v, "primary_manifest_integrity_mode");
    return response_uint(yyjson_obj_get(v, "shard_id")) && yyjson_get_num(yyjson_obj_get(v, "shard_id")) == shard &&
        yyjson_is_true(yyjson_obj_get(v, "persistent_request_path_enabled")) &&
        (text_is(manifest, "full_checksum") || text_is(manifest, "wal_backed_metadata")) &&
        response_uint(yyjson_obj_get(v, "latest_committed_tx_id")) &&
        response_uint(yyjson_obj_get(v, "latest_checkpoint_tx_id")) &&
        yyjson_is_obj(yyjson_obj_get(v, "wal")) &&
        response_uint(yyjson_obj_get(yyjson_obj_get(v, "wal"), "segment_count")) &&
        response_uint(yyjson_obj_get(yyjson_obj_get(v, "wal"), "total_bytes")) &&
        response_uint(yyjson_obj_get(yyjson_obj_get(v, "timings"), "commit_count"));
}

static yyjson_mut_val *persistence_summary(yyjson_mut_doc *d, yyjson_val *v) {
    yyjson_mut_val *out = yyjson_mut_obj(d);
    const char *names[] = {"shard_id", "persistent_request_path_enabled", "primary_manifest_integrity_mode", "latest_committed_tx_id", "latest_checkpoint_tx_id", "wal"};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); i++)
        yyjson_mut_obj_add_val(d, out, names[i], yyjson_val_mut_copy(d, yyjson_obj_get(v, names[i])));
    yyjson_mut_obj_add_val(d, out, "commit_count", yyjson_val_mut_copy(d, yyjson_obj_get(yyjson_obj_get(v,"timings"),"commit_count")));
    return out;
}

static yyjson_mut_val *artifact(yyjson_mut_doc *d, unsigned shard, const char *node, const char *hash, const char *world) {
    yyjson_mut_val *a = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d,a,"connection","default"); yyjson_mut_obj_add_uint(d,a,"shard_id",shard);
    yyjson_mut_obj_add_strcpy(d,a,"node_id",node); yyjson_mut_obj_add_strcpy(d,a,"sha256",hash);
    yyjson_mut_obj_add_strcpy(d,a,"world_id",world); return a;
}

static bool write_error(char *result, size_t cap, transport_t *t, unsigned shard, const char *node, const char *hash, const char *world) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);
    if (!d) { if (cap) result[0]='\0'; return false; }
    yyjson_mut_val *r=yyjson_mut_obj(d), *e=yyjson_mut_obj(d); yyjson_mut_doc_set_root(d,r);
    yyjson_mut_obj_add_bool(d,r,"ok",false); yyjson_mut_obj_add_val(d,r,"error",e);
    yyjson_mut_obj_add_str(d,e,"code",t->code); yyjson_mut_obj_add_str(d,e,"message",t->message);
    yyjson_mut_obj_add_str(d,e,"effect",t->may_have_published?"unknown":"none");
    if (t->status>0) yyjson_mut_obj_add_int(d,e,"http_status",t->status);
    if (node && hash && world) yyjson_mut_obj_add_val(d,e,"artifact",artifact(d,shard,node,hash,world));
    size_t length=0; char *encoded=yyjson_mut_write(d,0,&length);
    if (encoded && length<cap) memcpy(result,encoded,length+1);
    else if(cap) result[0]='\0';
    free(encoded); yyjson_mut_doc_free(d); return false;
}

bool lingo_graphsub_world_execute(const char *input, char *result, size_t capacity) {
    transport_t t={.code="invalid_request",.message="Invalid world artifact request"};
    unsigned shard=0; char hash[65]={0}, node[257]={0}; const char *world=NULL, *snapshot=NULL;
    yyjson_doc *args=NULL,*image=NULL,*before=NULL,*created=NULL,*fetched=NULL,*after=NULL;
    yyjson_mut_doc *wire=NULL,*output=NULL; char *post=NULL,*encoded=NULL; bool success=false;
    if (!result || !capacity) return false;
    result[0]='\0';
    size_t length=input?strnlen(input,INPUT_LIMIT+1):0;
    if (!length || length>INPUT_LIMIT) goto done;
    args=yyjson_read(input,length,YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_val *a=args?yyjson_doc_get_root(args):NULL;
    if (!yyjson_is_obj(a) || validate_json(a,0)!=RESPONSE_VALID) goto done;
    bool publish=text_is(yyjson_obj_get(a,"action"),"publish");
    if (!publish && !text_is(yyjson_obj_get(a,"action"),"read")) goto done;
    size_t i,count; yyjson_val *key,*value;
    yyjson_obj_foreach(a,i,count,key,value) {
        const char *k=yyjson_get_str(key);
        if (strcmp(k,"action") && strcmp(k,"connection") && strcmp(k,"shard_id") &&
            (publish?strcmp(k,"snapshot"):(strcmp(k,"node_id") && strcmp(k,"sha256") && strcmp(k,"world_id")))) goto done;
    }
    if ((yyjson_obj_get(a,"connection") && !text_is(yyjson_obj_get(a,"connection"),"default")) ||
        !uint_arg(yyjson_obj_get(a,"shard_id"),0,2147483647,&shard)) goto done;
    if (publish) {
        yyjson_val *s=yyjson_obj_get(a,"snapshot");
        if (!response_text(s) || yyjson_get_len(s)>SNAPSHOT_LIMIT) goto done;
        snapshot=yyjson_get_str(s);
        image=yyjson_read(snapshot,yyjson_get_len(s),YYJSON_READ_BIGNUM_AS_RAW);
        if (!image || !snapshot_valid(yyjson_doc_get_root(image),NULL)) goto done;
        world=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(image),"world_id"));
        sha256_hex((const uint8_t *)snapshot,strlen(snapshot),hash);
    } else {
        if (!native_id(yyjson_obj_get(a,"node_id")) || !hash_value(yyjson_obj_get(a,"sha256")) ||
            !bounded_text(yyjson_obj_get(a,"world_id"),128)) goto done;
        strcpy(node,yyjson_get_str(yyjson_obj_get(a,"node_id")));
        strcpy(hash,yyjson_get_str(yyjson_obj_get(a,"sha256")));
        world=yyjson_get_str(yyjson_obj_get(a,"world_id"));
    }
    dsco_http_global_init();
    t.code="bad_configuration"; t.message="Invalid GraphSub connection origin";
    t.url=configured_origin(publish?"publish":"read"); if (!t.url) goto done;
    const char *token=getenv("GRAPHSUB_API_KEY");
    if (token && *token) {
        size_t n=strnlen(token,4097); if (n>4096) goto done;
        for (size_t j=0;j<n;j++) if ((unsigned char)token[j]<33 || (unsigned char)token[j]>126) goto done;
        char auth[4120]; snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
        t.headers=curl_slist_append(NULL,auth); if (!t.headers) goto done;
    }
    struct curl_slist *next=curl_slist_append(t.headers,"Content-Type: application/json"); if(!next) goto done; t.headers=next;
    next=curl_slist_append(t.headers,"Accept: application/json"); if(!next) goto done; t.headers=next;
    char path[360], status_path[100], name[96];
    snprintf(status_path,sizeof(status_path),"/api/v1/shards/%u/persistence",shard);
    before=request_json(&t,status_path,NULL); if(!before) goto done;
    if(!persistence_valid(yyjson_doc_get_root(before),shard)) { t.code="persistence_unavailable"; t.message="GraphSub has no verified server-reported persistence metadata"; goto done; }
    if(publish) {
        wire=yyjson_mut_doc_new(NULL); if(!wire) goto done;
        yyjson_mut_val *w=yyjson_mut_obj(wire); yyjson_mut_doc_set_root(wire,w);
        snprintf(name,sizeof(name),"lingo.world/%.64s",hash);
        yyjson_mut_obj_add_str(wire,w,"type_name","State"); yyjson_mut_obj_add_strcpy(wire,w,"name",name);
        yyjson_mut_obj_add_strcpy(wire,w,"payload",snapshot); post=yyjson_mut_write(wire,0,NULL); if(!post) goto done;
        snprintf(path,sizeof(path),"/api/v1/shards/%u/nodes",shard);
        created=request_json(&t,path,post); if(!created) goto done;
        yyjson_val *c=yyjson_doc_get_root(created);
        if(t.status!=201 || !identity_matches(c,shard,NULL) || !text_is(yyjson_obj_get(c,"type"),"State") || !text_is(yyjson_obj_get(c,"name"),name)) {
            t.code="malformed_response"; t.message="GraphSub returned an invalid publication receipt"; goto done;
        }
        strcpy(node,yyjson_get_str(yyjson_obj_get(c,"id")));
    }
    snprintf(path,sizeof(path),"/api/v1/shards/%u/nodes/%s",shard,node);
    fetched=request_json(&t,path,NULL); if(!fetched) goto done;
    yyjson_val *f=yyjson_doc_get_root(fetched), *payload=yyjson_obj_get(f,"payload");
    if(!identity_matches(f,shard,node) || !text_is(yyjson_obj_get(f,"type"),"State") ||
        !response_text(payload) || yyjson_get_len(payload)>SNAPSHOT_LIMIT) {
        t.code="malformed_response"; t.message="GraphSub returned an invalid world artifact"; goto done;
    }
    char actual[65]; sha256_hex((const uint8_t *)yyjson_get_str(payload),yyjson_get_len(payload),actual);
    if(strcmp(actual,hash) || (publish && strcmp(snapshot,yyjson_get_str(payload)))) {
        t.code="integrity_mismatch"; t.message="World artifact content differs from its expected digest"; goto done;
    }
    if(!publish) {
        snapshot=yyjson_get_str(payload); image=yyjson_read(snapshot,strlen(snapshot),YYJSON_READ_BIGNUM_AS_RAW);
        if(!image || !snapshot_valid(yyjson_doc_get_root(image),world)) {
            t.code="invalid_snapshot"; t.message="World artifact snapshot has an invalid shape or identity"; goto done;
        }
    }
    after=request_json(&t,status_path,NULL); if(!after) goto done;
    yyjson_val *bv=yyjson_doc_get_root(before),*av=yyjson_doc_get_root(after);
    if(!persistence_valid(av,shard) || (publish &&
       (yyjson_get_num(yyjson_obj_get(av,"latest_committed_tx_id"))<=yyjson_get_num(yyjson_obj_get(bv,"latest_committed_tx_id")) ||
        yyjson_get_num(yyjson_obj_get(yyjson_obj_get(av,"timings"),"commit_count"))<=yyjson_get_num(yyjson_obj_get(yyjson_obj_get(bv,"timings"),"commit_count"))))) {
        t.code="persistence_unconfirmed"; t.message="World publication persistence evidence is incomplete"; goto done;
    }
    output=yyjson_mut_doc_new(NULL); if(!output) goto done;
    yyjson_mut_val *out=yyjson_mut_obj(output); yyjson_mut_doc_set_root(output,out);
    yyjson_mut_obj_add_bool(output,out,"ok",true); yyjson_mut_obj_add_str(output,out,"profile","world_artifact");
    yyjson_mut_obj_add_str(output,out,"action",publish?"publish":"read");
    yyjson_mut_obj_add_str(output,out,"consistency","content_verified"); yyjson_mut_obj_add_str(output,out,"persistence","server_reported");
    yyjson_mut_obj_add_val(output,out,"artifact",artifact(output,shard,node,hash,world));
    yyjson_mut_obj_add_strcpy(output,out,"snapshot",snapshot);
    yyjson_mut_obj_add_val(output,out,"before",persistence_summary(output,bv));
    yyjson_mut_obj_add_val(output,out,"after",persistence_summary(output,av));
    size_t out_len=0; encoded=yyjson_mut_write(output,0,&out_len);
    if(!encoded || out_len>=capacity) { t.code="response_buffer_too_small"; t.message="Complete world artifact receipt does not fit the result buffer"; goto done; }
    memcpy(result,encoded,out_len+1); success=true;
done:
    if(!success) write_error(result,capacity,&t,shard,node[0]?node:NULL,hash[0]?hash:NULL,world);
    yyjson_doc_free(args); yyjson_doc_free(image); yyjson_doc_free(before); yyjson_doc_free(created); yyjson_doc_free(fetched); yyjson_doc_free(after);
    yyjson_mut_doc_free(wire); yyjson_mut_doc_free(output); free(post); free(encoded);
    curl_slist_free_all(t.headers); curl_url_cleanup(t.url); return success;
}
