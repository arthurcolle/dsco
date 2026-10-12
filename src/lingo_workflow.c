#include "lingo_workflow.h"
#include "toolmgmt.h"
#include "service_boundary.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define INPUT_LIMIT (64u * 1024u)
#define OUTPUT_LIMIT (128u * 1024u)

/* The same narrow profile is independently checked by Autobot before dispatch.
 * No arbitrary URL, Python condition, code string, retry, or gate is admitted. */
const char lingo_workflow_schema[] =
"{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"execute\",\"read\",\"reconcile\"]},"
"\"workflow\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"maxLength\":128},"
"\"name\":{\"type\":\"string\",\"maxLength\":128},\"version\":{\"type\":\"string\",\"maxLength\":64},"
"\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120},"
"\"steps\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"items\":{\"type\":\"object\","
"\"properties\":{\"id\":{\"type\":\"string\",\"maxLength\":64},\"tool\":{\"type\":\"string\",\"maxLength\":200},"
"\"mode\":{\"enum\":[\"passthrough\",\"map\"]},\"input_from\":{\"type\":\"string\",\"maxLength\":64},"
"\"input_mapping\":{\"type\":\"object\",\"maxProperties\":32,\"additionalProperties\":{\"type\":\"string\"}}},"
"\"required\":[\"id\",\"tool\",\"mode\"],\"additionalProperties\":false}}},"
"\"required\":[\"id\",\"name\",\"version\",\"timeout_seconds\",\"steps\"],\"additionalProperties\":false},"
"\"inputs\":{\"type\":\"object\"},\"idempotency_key\":{\"type\":\"string\",\"maxLength\":128},"
"\"trace_id\":{\"type\":\"string\",\"maxLength\":128},\"execution_id\":{\"type\":\"string\",\"maxLength\":128}},"
"\"required\":[\"action\"],\"additionalProperties\":false,\"oneOf\":["
"{\"properties\":{\"action\":{\"const\":\"execute\"}},\"required\":[\"workflow\",\"inputs\",\"idempotency_key\"],\"not\":{\"required\":[\"execution_id\"]}},"
"{\"properties\":{\"action\":{\"const\":\"read\"}},\"required\":[\"execution_id\"],\"not\":{\"anyOf\":[{\"required\":[\"inputs\"]},{\"required\":[\"workflow\"]},{\"required\":[\"trace_id\"]},{\"required\":[\"idempotency_key\"]}]}},"
"{\"properties\":{\"action\":{\"const\":\"reconcile\"}},\"required\":[\"idempotency_key\"],\"not\":{\"anyOf\":[{\"required\":[\"inputs\"]},{\"required\":[\"workflow\"]},{\"required\":[\"trace_id\"]},{\"required\":[\"execution_id\"]}]}}]}";
const char lingo_workflow_output_schema[] =
"{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"profile\":{\"const\":\"lingo.workflow/1\"},"
"\"owner\":{\"const\":\"autobot\"},\"action\":{\"enum\":[\"execute\",\"read\",\"reconcile\"]},\"receipt\":{\"type\":\"object\"},"
"\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"code\",\"message\"],\"additionalProperties\":false}},"
"\"required\":[\"ok\"],\"additionalProperties\":false,\"oneOf\":[{\"properties\":{\"ok\":{\"const\":true}},\"required\":[\"profile\",\"owner\",\"action\",\"receipt\"]},"
"{\"properties\":{\"ok\":{\"const\":false}},\"required\":[\"error\"]}]}";

static bool fail(char *out, size_t cap, const char *code, const char *message) {
    if (out && cap) snprintf(out, cap,
        "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, message);
    return false;
}
static bool is(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v)==strlen(s) && !memcmp(yyjson_get_str(v),s,strlen(s));
}
static bool text_value(yyjson_val *v, size_t max, bool identifier) {
    if (!yyjson_is_str(v) || !yyjson_get_len(v) || yyjson_get_len(v)>max) return false;
    const unsigned char *s=(const unsigned char *)yyjson_get_str(v);
    if (identifier && (is(v,".") || is(v,".."))) return false;
    for (size_t i=0;i<yyjson_get_len(v);i++) {
        unsigned char c=s[i];
        if (c<32 || c==127) return false;
        if (identifier && !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c==':')) return false;
    }
    return true;
}
static bool digest_value(yyjson_val *v) {
    if (!yyjson_is_str(v)||yyjson_get_len(v)!=64) return false;
    const char *s=yyjson_get_str(v);
    for (size_t i=0;i<64;i++) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    return true;
}
static bool nullable_text(yyjson_val *v, size_t maximum) {
    return yyjson_is_null(v)||text_value(v,maximum,false);
}
static bool receipt_valid(yyjson_val *v, bool read) {
    yyjson_val *meta=read?yyjson_obj_get(v,"metadata"):v,*status=yyjson_obj_get(v,"status");
    if (!yyjson_is_obj(v)||!text_value(yyjson_obj_get(v,"execution_id"),128,true)||
        !text_value(yyjson_obj_get(v,"workflow_id"),128,true)||
        !text_value(yyjson_obj_get(v,"idempotency_key"),128,true)||
        !is(yyjson_obj_get(meta,"execution_profile"),"lingo.v1")||
        !digest_value(yyjson_obj_get(meta,"request_hash"))||
        !(is(status,"completed")||is(status,"failed")||is(status,"running"))) return false;
    if (read) return yyjson_is_arr(yyjson_obj_get(v,"steps"));
    yyjson_val *duration=yyjson_obj_get(v,"duration_ms");
    if (!text_value(yyjson_obj_get(v,"workflow_name"),128,false)||
        !yyjson_is_obj(yyjson_obj_get(v,"step_results"))||!yyjson_obj_get(v,"output")||
        !yyjson_is_num(duration)||!isfinite(yyjson_get_num(duration))||yyjson_get_num(duration)<0||
        !nullable_text(yyjson_obj_get(v,"trace_id"),128)||
        !yyjson_is_bool(yyjson_obj_get(v,"idempotent_replay"))) return false;
    yyjson_val *error=yyjson_obj_get(v,"error");
    if (is(status,"failed")) return text_value(error,256,false);
    return yyjson_is_null(error);
}
static bool fields(yyjson_val *v, const char *allowed) {
    if (!yyjson_is_obj(v)) return false;
    size_t i,n; yyjson_val *k,*x;
    yyjson_obj_foreach(v,i,n,k,x) {
        const char *name=yyjson_get_str(k); char token[96];
        if (yyjson_get_len(k)>80 || strlen(name)!=yyjson_get_len(k) || yyjson_obj_get(v,name)!=x) return false;
        snprintf(token,sizeof(token),"|%s|",name);
        if (!strstr(allowed,token)) return false;
    }
    return true;
}
static bool portable(yyjson_val *v, unsigned depth, size_t *count) {
    if (depth>48 || ++*count>16384 || yyjson_is_raw(v)) return false;
    if (yyjson_is_num(v)) {
        double x=yyjson_get_num(v);
        return isfinite(x) && fabs(x)<=9007199254740991.;
    }
    size_t i,n; yyjson_val *k,*x;
    if (yyjson_is_obj(v)) {
        yyjson_obj_foreach(v,i,n,k,x)
            if (strlen(yyjson_get_str(k))!=yyjson_get_len(k) ||
                yyjson_obj_get(v,yyjson_get_str(k))!=x || !portable(x,depth+1,count)) return false;
    } else if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v,i,n,x) if (!portable(x,depth+1,count)) return false;
    }
    return true;
}
static bool workflow_valid(yyjson_val *w) {
    if (!fields(w,"|id|name|version|timeout_seconds|steps|") ||
        !text_value(yyjson_obj_get(w,"id"),128,true) ||
        !text_value(yyjson_obj_get(w,"name"),128,false) ||
        !text_value(yyjson_obj_get(w,"version"),64,true)) return false;
    yyjson_val *t=yyjson_obj_get(w,"timeout_seconds"), *steps=yyjson_obj_get(w,"steps");
    double timeout=yyjson_get_num(t);
    if (!yyjson_is_num(t)||timeout<1||timeout>120||floor(timeout)!=timeout||
        !yyjson_is_arr(steps)||!yyjson_arr_size(steps)||yyjson_arr_size(steps)>16) return false;
    size_t i,n; yyjson_val *s;
    yyjson_arr_foreach(steps,i,n,s) {
        yyjson_val *id=yyjson_obj_get(s,"id"), *from=yyjson_obj_get(s,"input_from"), *map=yyjson_obj_get(s,"input_mapping");
        if (!fields(s,"|id|tool|mode|input_from|input_mapping|") || !text_value(id,64,true) ||
            !text_value(yyjson_obj_get(s,"tool"),200,true) ||
            !(is(yyjson_obj_get(s,"mode"),"passthrough")||is(yyjson_obj_get(s,"mode"),"map"))) return false;
        bool found=!from;
        if (from && !text_value(from,64,true)) return false;
        for (size_t j=0;j<i;j++) {
            yyjson_val *prior=yyjson_obj_get(yyjson_arr_get(steps,j),"id");
            if (is(prior,yyjson_get_str(id))) return false;
            if (from && is(prior,yyjson_get_str(from))) found=true;
        }
        if (!found) return false;
        if (map) {
            if (!yyjson_is_obj(map)||yyjson_obj_size(map)>32) return false;
            size_t a,b; yyjson_val *key,*value;
            yyjson_obj_foreach(map,a,b,key,value)
                if (!text_value(key,128,false)||!text_value(value,128,false)) return false;
        }
    }
    return true;
}
static bool origin_allowed(const char *action) {
    const char *base=toolmgmt_base_url();
    if (!base || strnlen(base,2049)>2048 || strchr(base,'?') || strchr(base,'#')) return false;
    for (const unsigned char *p=(const unsigned char*)base;*p;p++) if (*p<=32||*p==127) return false;
    CURLU *u=curl_url(); char *scheme=NULL,*host=NULL,*path=NULL;
    bool ok=u && curl_url_set(u,CURLUPART_URL,base,CURLU_DISALLOW_USER)==CURLUE_OK &&
        curl_url_get(u,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK &&
        curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK &&
        curl_url_get(u,CURLUPART_PATH,&path,0)==CURLUE_OK && !strcmp(path,"/");
    if (ok && !strcmp(scheme,"http")) {
        struct in_addr v4;
        ok=!strcmp(host,"[::1]") || (inet_pton(AF_INET,host,&v4)==1 && (ntohl(v4.s_addr)>>24)==127);
    } else if (ok) ok=!strcmp(scheme,"https");
    curl_free(scheme);curl_free(host);curl_free(path);curl_url_cleanup(u);
    return ok && service_action_destination_allowed("autobot_workflow",action,base);
}
bool lingo_workflow_execute(const char *input, char *out, size_t cap) {
    if (!out||cap<OUTPUT_LIMIT+1024) return fail(out,cap,"response_buffer_too_small","Workflow requires a complete bounded receipt buffer");
    size_t len=input?strnlen(input,INPUT_LIMIT+1):0,count=0;
    if (!len||len>INPUT_LIMIT) return fail(out,cap,"invalid_request","Workflow request exceeds 64 KiB");
    yyjson_doc *doc=yyjson_read(input,len,YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_val *r=doc?yyjson_doc_get_root(doc):NULL,*a=yyjson_obj_get(r,"action");
    bool execute=is(a,"execute"),read=is(a,"read"),reconcile=is(a,"reconcile");
    bool valid=portable(r,0,&count) && ((execute && fields(r,"|action|workflow|inputs|idempotency_key|trace_id|") &&
        workflow_valid(yyjson_obj_get(r,"workflow")) && yyjson_is_obj(yyjson_obj_get(r,"inputs")) &&
        text_value(yyjson_obj_get(r,"idempotency_key"),128,true) &&
        (!yyjson_obj_get(r,"trace_id")||text_value(yyjson_obj_get(r,"trace_id"),128,true))) ||
        (read && fields(r,"|action|execution_id|") && text_value(yyjson_obj_get(r,"execution_id"),128,true)) ||
        (reconcile && fields(r,"|action|idempotency_key|") && text_value(yyjson_obj_get(r,"idempotency_key"),128,true)));
    if (!valid) { yyjson_doc_free(doc); return fail(out,cap,"invalid_request","Expected a bounded workflow with unique ordered dependencies, exact mappings and an idempotency key, or an execution ID to read"); }
    const char *action=execute?"execute":read?"read":"reconcile";
    if (!origin_allowed(action)) { yyjson_doc_free(doc); return fail(out,cap,"destination_denied","Autobot origin must be HTTPS or numeric loopback HTTP and satisfy network grants"); }
    const char *enabled=getenv("DSCO_TOOLMGMT");
    if ((enabled&&(!strcmp(enabled,"0")||!strcasecmp(enabled,"false")||!strcasecmp(enabled,"off"))) || !toolmgmt_token()) {
        yyjson_doc_free(doc); return fail(out,cap,"service_unavailable","Autobot is disabled or its credentials are not configured");
    }
    yyjson_mut_doc *bodydoc=yyjson_mut_doc_new(NULL); yyjson_mut_val *bodyroot=bodydoc?yyjson_mut_obj(bodydoc):NULL;
    /* Build from validated fields, setting backend-only profile and names here. */
    if (bodyroot) yyjson_mut_doc_set_root(bodydoc,bodyroot);
    long timeout=15000; char path[320]; char *body=NULL;
    if (execute && bodyroot) {
        bodyroot=yyjson_val_mut_copy(bodydoc,r); yyjson_mut_doc_set_root(bodydoc,bodyroot);
        yyjson_mut_obj_remove_key(bodyroot,"action");
        yyjson_mut_obj_add_str(bodydoc,bodyroot,"execution_profile","lingo.v1");
        yyjson_mut_val *w=yyjson_mut_obj_get(bodyroot,"workflow");
        yyjson_mut_obj_add_int(bodydoc,w,"max_parallel_steps",1);
        yyjson_mut_val *steps=yyjson_mut_obj_get(w,"steps"),*s;size_t i,n;
        yyjson_mut_arr_foreach(steps,i,n,s) yyjson_mut_obj_add_strcpy(bodydoc,s,"name",yyjson_mut_get_str(yyjson_mut_obj_get(s,"id")));
        timeout=(long)yyjson_get_num(yyjson_obj_get(yyjson_obj_get(r,"workflow"),"timeout_seconds"))*1000+5000;
        body=yyjson_mut_write(bodydoc,0,NULL); snprintf(path,sizeof(path),"/api/v1/compose/execute");
    } else if (read) snprintf(path,sizeof(path),"/api/v1/compose/executions/%s",yyjson_get_str(yyjson_obj_get(r,"execution_id")));
    else snprintf(path,sizeof(path),"/api/v1/compose/lingo/keys/%s",yyjson_get_str(yyjson_obj_get(r,"idempotency_key")));
    if (!bodydoc || !bodyroot || (execute&&!body)) {free(body);yyjson_mut_doc_free(bodydoc);yyjson_doc_free(doc);return fail(out,cap,"allocation_failed","Cannot encode workflow");}
    if (body && strlen(body)>INPUT_LIMIT) {free(body);yyjson_mut_doc_free(bodydoc);yyjson_doc_free(doc);return fail(out,cap,"invalid_request","Encoded Autobot request exceeds 64 KiB");}
    char *response=NULL;
    long status=toolmgmt_request_bounded(execute?"POST":"GET",path,body,&response,timeout,OUTPUT_LIMIT);
    free(body);yyjson_mut_doc_free(bodydoc);
    if (status<200||status>=300) {
        free(response);yyjson_doc_free(doc);
        bool unknown=execute&&(status<0||status>=500||status==408);
        return fail(out,cap,unknown?"outcome_unknown":"backend_rejected",
            unknown?"Autobot outcome is unknown; do not automatically resubmit. Reconcile the idempotency key with the execution owner":"Autobot did not return an accepted workflow receipt");
    }
    yyjson_doc *rd=response?yyjson_read(response,strlen(response),YYJSON_READ_BIGNUM_AS_RAW):NULL;
    yyjson_val *v=rd?yyjson_doc_get_root(rd):NULL;count=0;
    valid=portable(v,0,&count)&&receipt_valid(v,read);
    if (valid && execute) {
        yyjson_val *workflow=yyjson_obj_get(r,"workflow"), *steps=yyjson_obj_get(workflow,"steps"),*results=yyjson_obj_get(v,"step_results");
        valid=is(yyjson_obj_get(v,"workflow_id"),yyjson_get_str(yyjson_obj_get(workflow,"id")))&&
            is(yyjson_obj_get(v,"workflow_name"),yyjson_get_str(yyjson_obj_get(workflow,"name")))&&
            is(yyjson_obj_get(v,"idempotency_key"),yyjson_get_str(yyjson_obj_get(r,"idempotency_key")));
        if (valid && is(yyjson_obj_get(v,"status"),"completed")) {
            valid=yyjson_obj_size(results)==yyjson_arr_size(steps);
            size_t i,n;yyjson_val *step;
            yyjson_arr_foreach(steps,i,n,step) {
                yyjson_val *output=yyjson_obj_get(results,yyjson_get_str(yyjson_obj_get(step,"id")));
                if (!output || (i==n-1&&!yyjson_equals(output,yyjson_obj_get(v,"output")))) valid=false;
            }
        }
    }
    if (valid && read) valid=is(yyjson_obj_get(v,"execution_id"),yyjson_get_str(yyjson_obj_get(r,"execution_id")));
    if (valid && reconcile) valid=is(yyjson_obj_get(v,"idempotency_key"),yyjson_get_str(yyjson_obj_get(r,"idempotency_key")));
    if (valid) snprintf(out,cap,"{\"ok\":true,\"profile\":\"lingo.workflow/1\",\"owner\":\"autobot\",\"action\":\"%s\",\"receipt\":%s}",action,response);
    free(response);yyjson_doc_free(rd);yyjson_doc_free(doc);
    return valid || fail(out,cap,"invalid_receipt","Autobot receipt has invalid profile, portable values or execution identity; the execution may already have occurred");
}
