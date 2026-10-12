#include "context_archive.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *context_archive_serialize(const message_t *messages, int count) {
    if (!messages || count < 1) return NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root != NULL;
    for (int i = 0; i < count && ok; i++) {
        yyjson_mut_val *m = yyjson_mut_obj(doc), *content = yyjson_mut_arr(doc);
        ok = m && content && yyjson_mut_obj_add_str(doc, m, "role",
             messages[i].role == ROLE_USER ? "user" : "assistant") &&
             yyjson_mut_obj_add_val(doc, m, "content", content) &&
             yyjson_mut_arr_add_val(root, m);
        for (int j = 0; j < messages[i].content_count && ok; j++) {
            const msg_content_t *c = &messages[i].content[j];
            yyjson_mut_val *b = yyjson_mut_obj(doc);
            ok = b && yyjson_mut_obj_add_bool(doc,b,"is_error",c->is_error) &&
                 yyjson_mut_obj_add_int(doc,b,"cite_start",c->cite_start) &&
                 yyjson_mut_obj_add_int(doc,b,"cite_end",c->cite_end);
            const char *keys[] = {"type","text","tool_name","tool_id","tool_input",
                "image_media_type","image_data","image_url","video_media_type",
                "video_data","video_url","doc_media_type","doc_data","doc_title","cited_text"};
            const char *values[] = {c->type,c->text,c->tool_name,c->tool_id,c->tool_input,
                c->image_media_type,c->image_data,c->image_url,c->video_media_type,
                c->video_data,c->video_url,c->doc_media_type,c->doc_data,c->doc_title,c->cited_text};
            for (size_t k = 0; k < sizeof(keys)/sizeof(*keys) && ok; k++)
                ok = values[k] ? yyjson_mut_obj_add_str(doc,b,keys[k],values[k]) :
                                 yyjson_mut_obj_add_null(doc,b,keys[k]);
            if (ok) ok = yyjson_mut_arr_add_val(content,b);
        }
    }
    char *json = ok ? yyjson_mut_write(doc,0,NULL) : NULL;
    yyjson_mut_doc_free(doc);
    return json;
}

bool context_archive_store_verified_ex(const void *bytes, size_t len,
                                    const ctx_put_opts_t *options, ctxkey_t *key,
                                    const char **failure_stage) {
    const char *stage = "none";
    ctx_broker_t *broker=ctx_broker_default();
    if (!broker) stage="broker_unavailable";
    else if (!bytes || !key) stage="invalid_archive";
    else if (!ctx_put(broker,bytes,len,options,key,NULL)) stage="write_failed";
    else {
        size_t read_len=0;
        char *check=ctx_get(broker,key,&read_len);
        if (!check) stage="readback_failed";
        else if (read_len!=len) stage="readback_length_mismatch";
        else if (memcmp(check,bytes,len)) stage="readback_content_mismatch";
        free(check);
    }
    if (failure_stage) *failure_stage=stage;
    return !strcmp(stage,"none");
}
bool context_archive_store_verified(const void *bytes, size_t len,
                                    const ctx_put_opts_t *options, ctxkey_t *key) {
    return context_archive_store_verified_ex(bytes,len,options,key,NULL);
}

static bool archive_page_utf8(const unsigned char *s,size_t n) {
    for(size_t i=0;i<n;) {
        unsigned char c=s[i++];if(!c)return false;if(c<0x80)continue;
        size_t continuation=c>=0xc2 && c<=0xdf?1:c>=0xe0 && c<=0xef?2:c>=0xf0 && c<=0xf4?3:0;
        if(!continuation || continuation>n-i)return false;
        if((c==0xe0 && s[i]<0xa0) || (c==0xed && s[i]>=0xa0) ||
           (c==0xf0 && s[i]<0x90) || (c==0xf4 && s[i]>=0x90))return false;
        for(size_t j=0;j<continuation;j++)if((s[i+j]&0xc0)!=0x80)return false;
        i+=continuation;
    }
    return true;
}
static size_t archive_page_base64(const unsigned char *s,size_t n,char *out) {
    static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t w=0;
    for(size_t i=0;i<n;i+=3) {
        unsigned v=(unsigned)s[i]<<16;
        if(i+1<n)v|=(unsigned)s[i+1]<<8;if(i+2<n)v|=s[i+2];
        out[w++]=table[(v>>18)&63];out[w++]=table[(v>>12)&63];
        out[w++]=i+1<n?table[(v>>6)&63]:'=';out[w++]=i+2<n?table[v&63]:'=';
    }
    return w;
}

bool context_archive_page(ctx_broker_t *broker, const ctxkey_t *key,
                          char *result, size_t len) {
    if (!result || len<512 || !key) return false;
    ctxkey_t base=*key; base.slice_kind=CTX_SLICE_NONE;
    char base_key[CTX_KEY_STR_MAX];
    if (ctxkey_format(&base,base_key,sizeof(base_key))<0) return false;
    size_t total=0;
    char *text=ctx_get(broker,&base,&total);
    if (!text) { snprintf(result,len,"error: context archive unavailable or failed integrity check");return false; }
    size_t start=0,end=total;
    if (key->slice_kind==CTX_SLICE_BYTES) {
        start=key->slice_start<0?0:(size_t)key->slice_start;
        end=key->slice_end<0?total:(size_t)key->slice_end;
        if(start>total)start=total;
        if(end>total)end=total;
        if(end<start)end=start;
    } else if (key->slice_kind==CTX_SLICE_LINES) {
        long line=1; bool found=key->slice_start<=1;
        for(size_t i=0;i<total;i++) if(text[i]=='\n') {
            if(line==key->slice_end) {end=i+1;break;}
            ++line;
            if(line==key->slice_start){start=i+1;found=true;}
        }
        if(!found)start=end=total;
    }
    bool utf8=archive_page_utf8((const unsigned char *)text+start,end-start);
    size_t copy=end-start;
    if(copy>len-1)copy=len-1;
    char header[1024]; int n;
    for(;;) {
        while(utf8 && copy && start+copy<end && ((unsigned char)text[start+copy]&0xc0)==0x80)copy--;
        char next[CTX_KEY_STR_MAX]="";
        if(start+copy<end) {
            ctxkey_t continuation=base; continuation.slice_kind=CTX_SLICE_BYTES;
            continuation.slice_start=(long)(start+copy);continuation.slice_end=(long)end;
            if(ctxkey_format(&continuation,next,sizeof(next))<0){free(text);return false;}
        }
        n=snprintf(header,sizeof(header),
            "{\"key\":\"%s\",\"total_bytes\":%zu,\"range\":[%zu,%zu],\"requested_end\":%zu,\"complete\":%s,\"encoding\":\"%s\",\"next_key\":\"%s\"}\n",
            base_key,total,start,start+copy,end,start+copy==end?"true":"false",utf8?"utf-8":"base64",next);
        if(n<0 || (size_t)n>=sizeof(header) || (size_t)n>=len-1) {free(text);return false;}
        size_t encoded=utf8?copy:4*((copy+2)/3);
        if((size_t)n+encoded<len)break;
        size_t room=len-(size_t)n-1;
        copy=utf8?room:room/4*3;
    }
    if(!copy && end>start){free(text);snprintf(result,len,"error: output buffer has no room for a context page");return false;}
    memcpy(result,header,(size_t)n);
    size_t emitted=copy;
    if(utf8)memcpy(result+n,text+start,copy);
    else emitted=archive_page_base64((const unsigned char *)text+start,copy,result+n);
    result[n+emitted]=0;
    free(text);return true;
}

/* Historical previews sometimes ended inside a codepoint. The provider JSON
 * encoder replaced those invalid bytes with U+FFFD. Authenticate that exact
 * wire representation too; never normalize away arbitrary user text. */
static bool legacy_wire_text_matches(const char *expected,const char *observed) {
    if(!strcmp(expected,observed))return true;
    jbuf_t json;jbuf_init(&json,strlen(expected)+32);if(!json.data)return false;
    jbuf_append_json_str(&json,expected);
    yyjson_doc *doc=yyjson_read(json.data,json.len,0);
    const char *decoded=doc?yyjson_get_str(yyjson_doc_get_root(doc)):NULL;
    bool match=decoded && !strcmp(decoded,observed);
    yyjson_doc_free(doc);jbuf_free(&json);return match;
}

static bool legacy_receipt_verified(const message_t *a, const message_t *r, unsigned depth) {
    if(depth>=64)return false; /* leave an unverifiable index live, never discard it */
    const char *prefix="[Archived completed tool exchange; key=";
    if(!a || !r || a->role!=ROLE_ASSISTANT || r->role!=ROLE_USER ||
       a->content_count!=1 || r->content_count!=1 ||
       !a->content[0].type || strcmp(a->content[0].type,"text") ||
       !r->content[0].type || strcmp(r->content[0].type,"text") ||
       a->content[0].tool_name || r->content[0].tool_name ||
       !a->content[0].text || !r->content[0].text ||
       strncmp(r->content[0].text,prefix,strlen(prefix)))return false;
    const char *begin=r->content[0].text+strlen(prefix),*semi=strchr(begin,';');
    char handle[CTX_KEY_STR_MAX];ctxkey_t key;
    if(!semi || (size_t)(semi-begin)>=sizeof(handle))return false;
    memcpy(handle,begin,(size_t)(semi-begin));handle[semi-begin]=0;
    if(!ctxkey_parse(handle,&key) || key.kind!=CTX_KIND_TOOL || key.slice_kind!=CTX_SLICE_NONE)return false;
    char expected_header[512];
    int header=snprintf(expected_header,sizeof(expected_header),
        "%s%s; retrieve with context_recall, optionally #b:0-4096]\n",prefix,handle);
    if(header<0 || (size_t)header>=sizeof(expected_header))return false;
    if(strncmp(r->content[0].text,expected_header,(size_t)header)) {
        header=snprintf(expected_header,sizeof(expected_header),"%s%s; retrieve with context_recall]\n",prefix,handle);
        if(header<0 || (size_t)header>=sizeof(expected_header) ||
           strncmp(r->content[0].text,expected_header,(size_t)header))return false;
    }
    size_t bytes=0;char *archive=ctx_get(ctx_broker_default(),&key,&bytes);
    if(!archive)return false;
    yyjson_doc *doc=yyjson_read(archive,bytes,0);free(archive);
    yyjson_val *root=doc?yyjson_doc_get_root(doc):NULL;
    /* Older receipt indexes had no provenance tag either. Authenticate the
     * exact generated envelope and every child, not just an index-looking key. */
    if(!strcmp(a->content[0].text,"Earlier completed tool work is archived; consult the receipt index for details.") &&
       !strcmp(r->content[0].text+header,"Earlier tool receipts and child archive keys are preserved in this index.")) {
        size_t count=yyjson_arr_size(root);bool valid=yyjson_is_arr(root) && count>=8 && !(count%2);
        for(size_t i=0;i<count && valid;i+=2) {
            message_t pair[2]={0};msg_content_t blocks[2]={0};
            for(size_t j=0;j<2 && valid;j++) {
                yyjson_val *m=yyjson_arr_get(root,i+j),*content=yyjson_obj_get(m,"content");
                yyjson_val *b=yyjson_arr_get(content,0);
                const char *tag=yyjson_get_str(yyjson_obj_get(b,"tool_name"));
                valid=yyjson_equals_str(yyjson_obj_get(m,"role"),j?"user":"assistant") &&
                      yyjson_is_arr(content) && yyjson_arr_size(content)==1 &&
                      yyjson_equals_str(yyjson_obj_get(b,"type"),"text") &&
                      (!tag || !strcmp(tag,"context_evict"));
                const char *extras[]={"tool_id","tool_input","image_data","image_url","video_data","video_url","doc_data","cited_text"};
                for(size_t k=0;k<sizeof(extras)/sizeof(*extras);k++) {
                    const char *value=yyjson_get_str(yyjson_obj_get(b,extras[k]));if(value && *value)valid=false;
                }
                blocks[j].type="text";blocks[j].text=(char *)yyjson_get_str(yyjson_obj_get(b,"text"));
                pair[j].role=j?ROLE_USER:ROLE_ASSISTANT;pair[j].content=&blocks[j];pair[j].content_count=1;
            }
            if(valid)valid=legacy_receipt_verified(&pair[0],&pair[1],depth+1);
        }
        yyjson_doc_free(doc);return valid;
    }
    conversation_t original;conv_init(&original);
    bool ok=yyjson_is_arr(root) && yyjson_arr_size(root)==2;
    if(ok) { original.msgs=calloc(2,sizeof(message_t));ok=original.msgs!=NULL;original.count=original.cap=ok?2:0; }
    for(int i=0;i<2 && ok;i++) {
        yyjson_val *m=yyjson_arr_get(root,(size_t)i),*blocks=yyjson_obj_get(m,"content");
        ok=yyjson_equals_str(yyjson_obj_get(m,"role"),i?"user":"assistant") && yyjson_is_arr(blocks);
        if(!ok)break;
        message_t *target=&original.msgs[i];target->role=i?ROLE_USER:ROLE_ASSISTANT;
        target->content_count=(int)yyjson_arr_size(blocks);
        target->content=calloc((size_t)target->content_count,sizeof(msg_content_t));
        if(!target->content){target->content_count=0;ok=false;break;}
        for(int j=0;j<target->content_count && ok;j++) {
            yyjson_val *b=yyjson_arr_get(blocks,(size_t)j);msg_content_t *c=&target->content[j];
            char **fields[]={&c->type,&c->text,&c->tool_name,&c->tool_id,&c->tool_input};
            const char *names[]={"type","text","tool_name","tool_id","tool_input"};
            for(int k=0;k<5 && ok;k++) {
                const char *value=yyjson_get_str(yyjson_obj_get(b,names[k]));
                if(value){*fields[k]=strdup(value);if(!*fields[k])ok=false;}
            }
            c->is_error=yyjson_get_bool(yyjson_obj_get(b,"is_error"));
            if(!c->type || (i && strcmp(c->type,"tool_result")))ok=false;
        }
    }
    yyjson_doc_free(doc);
    ok=ok && conv_validate_tool_call_integrity(&original,false).ok &&
        conv_render_legacy_tool_turn(&original,384,0) &&
        legacy_wire_text_matches(original.msgs[0].content[0].text,a->content[0].text) &&
        legacy_wire_text_matches(original.msgs[1].content[0].text,r->content[0].text+header);
    conv_free(&original);return ok;
}

bool context_archive_legacy_receipt(const message_t *a,const message_t *r) {
    return legacy_receipt_verified(a,r,0);
}

context_archive_replace_result_t context_archive_replace(conversation_t *conv, int start, int count,
                             const char *key, bool tool_span, int preview_chars, bool allow_expansion) {
    int replacements = tool_span ? 2 : 1;
    if (!conv || !key || start < 1 || count < replacements ||
        start > conv->count - count || preview_chars < 0 || preview_chars > 8192) return false;
    /* Preview only readable text/results, never replay metadata or ciphertext. */
    const char *preview = "";
    for (int i = 0; i < count && !*preview; i++)
        for (int j = 0; j < conv->msgs[start+i].content_count; j++) {
            const msg_content_t *c = &conv->msgs[start+i].content[j];
            if (c->type && (!strcmp(c->type,"text") || !strcmp(c->type,"tool_result")) && c->text) {
                preview = c->text; if (*preview) break;
            }
        }
    size_t cut = strlen(preview);
    if (cut > (size_t)preview_chars) cut = (size_t)preview_chars;
    while (cut && ((unsigned char)preview[cut] & 0xc0) == 0x80) cut--;
    char *receipt = malloc(strlen(key)+cut+256);
    if (!receipt) return false;
    int prefix = sprintf(receipt,"[Archived %s; key=%s; retrieve with context_recall, optionally #b:0-4096]\n",
                         tool_span ? "completed tool exchange" : "assistant response",key);
    if (cut) {
        prefix += sprintf(receipt+prefix,"[Partial preview: %zu of %zu bytes; use the key for full evidence]\n",cut,strlen(preview));
    }
    memcpy(receipt+prefix,preview,cut); receipt[prefix+cut]=0;
    conversation_t replacement; conv_init(&replacement);
    /* Allocate explicitly: conv_add_* helpers do not report allocation failure. */
    replacement.msgs = calloc((size_t)replacements,sizeof(message_t));
    replacement.count = replacement.cap = replacements;
    if (!replacement.msgs) { free(receipt); return false; }
    bool ok = true;
    for (int i = 0; i < replacements; i++) {
        message_t *m = &replacement.msgs[i];
        m->role = i ? ROLE_USER : ROLE_ASSISTANT;
        m->content = calloc(1,sizeof(msg_content_t));
        if (!m->content) { ok = false; break; }
        m->content_count = 1;
        m->content[0].type = strdup("text");
        m->content[0].tool_name = strdup("context_evict");
        m->content[0].text = strdup(tool_span && i == 0 ?
            "Completed tool work is archived; consult the following retrieval receipt." : receipt);
        if (!m->content[0].type || !m->content[0].tool_name || !m->content[0].text) { ok=false; break; }
    }
    free(receipt);
    if (!ok) { conv_free(&replacement); return false; }
    conversation_t view = {.msgs=conv->msgs+start,.count=count,.cap=count};
    if (!allow_expansion && replacements >= count &&
        conv_rough_estimate(&replacement) >= conv_rough_estimate(&view)) {
        conv_free(&replacement); return CONTEXT_ARCHIVE_NOT_REDUCING;
    }
    while (view.count) conv_pop_last(&view);
    memcpy(conv->msgs+start,replacement.msgs,(size_t)replacements*sizeof(message_t));
    free(replacement.msgs);
    int removed = count-replacements;
    memmove(conv->msgs+start+replacements,conv->msgs+start+count,
            (size_t)(conv->count-start-count)*sizeof(message_t));
    conv->count -= removed;
    memset(conv->msgs+conv->count,0,(size_t)removed*sizeof(message_t));
    return true;
}
