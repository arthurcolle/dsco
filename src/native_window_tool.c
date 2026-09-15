#include "native_windows.h"
#include "pixel_tui.h"
#include "tools.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *str(yyjson_val *o,const char *k) {return yyjson_get_str(yyjson_obj_get(o,k));}
static bool eq(const char *a,const char *b) {return a && b && !strcmp(a,b);}
static bool hex_string(const char *s,size_t len,bool uuid) {
    if(!s || strlen(s)!=len)return false;
    for(size_t i=0;i<len;i++) {
        if(uuid && (i==8 || i==13 || i==18 || i==23)) {if(s[i]!='-')return false;}
        else if(!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f')))return false;
    }
    return true;
}
static bool fail(char *out,size_t cap,const char *error,const char *detail) {
    if(!out || !cap)return false;
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);
    if(!d) {if(cap)snprintf(out,cap,"{\"ok\":false,\"error\":\"out_of_memory\"}");return false;}
    yyjson_mut_val *o=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,o);
    yyjson_mut_obj_add_bool(d,o,"ok",false);yyjson_mut_obj_add_str(d,o,"error",error);
    yyjson_mut_obj_add_str(d,o,"detail",detail ? detail : "");
    char *s=yyjson_mut_write(d,0,NULL);
    if(s && strlen(s)<cap)memcpy(out,s,strlen(s)+1);
    else if(out && cap>=13)memcpy(out,"{\"ok\":false}",13);
    else if(out && cap>=3)memcpy(out,"{}",3);
    else if(out && cap)out[0]=0;
    free(s);yyjson_mut_doc_free(d);return false;
}

bool tool_native_window(const char *input,char *result,size_t cap) {
    if(!result || cap<128)return fail(result,cap,"output_capacity_too_small",NULL);
    const char *error=native_windows_validate(input);
    if(error)return fail(result,cap,error,"See native_window schema for supported fields and limits.");
    yyjson_doc *d=yyjson_read(input,strlen(input),0);
    yyjson_val *o=yyjson_doc_get_root(d);const char *action=str(o,"action");
    bool observation=eq(action,"list") || eq(action,"events");
    if(!observation && !pixel_tui_session_active()) {
        yyjson_doc_free(d);
        return fail(result,cap,"native_session_required","Start dsco --native in Kitty; /windows opens the workspace. Use buffer for persistent text and surface for actual terminal windows.");
    }
    bool ok=false;
    if(eq(action,"buffer") || eq(action,"refresh")) {
        const char *workspace=str(o,"workspace"),*buffer_id=str(o,"buffer_id"),*name=str(o,"name"),*title=str(o,"title");
        uint64_t id=yyjson_get_uint(yyjson_obj_get(o,"id"));
        native_windows_snapshot_t *snapshot=NULL;
        if(eq(action,"refresh")) {
            snapshot=malloc(sizeof(*snapshot));
            if(!snapshot) {yyjson_doc_free(d);return fail(result,cap,"out_of_memory",NULL);}
            native_windows_snapshot(snapshot);const native_window_t *w=NULL;
            for(int i=0;i<snapshot->count;i++)if(snapshot->windows[i].id==id)w=&snapshot->windows[i];
            if(!w || w->kind!=NATIVE_WINDOW_BUFFER) {
                free(snapshot);yyjson_doc_free(d);return fail(result,cap,"buffer_window_not_found","Refresh requires the ID of a retained buffer panel.");
            }
            workspace=w->workspace;buffer_id=w->buffer_id;name=NULL;title=w->title;
        } else if(id) {
            yyjson_doc_free(d);return fail(result,cap,"id_not_allowed","buffer opens a new panel; use refresh with its returned ID.");
        }
        if((!buffer_id || !*buffer_id) && (!name || !*name)) {
            free(snapshot);yyjson_doc_free(d);return fail(result,cap,"buffer_selector_required","Provide buffer_id or name.");
        }
        yyjson_mut_doc *request=yyjson_mut_doc_new(NULL);
        if(!request) {free(snapshot);yyjson_doc_free(d);return fail(result,cap,"out_of_memory",NULL);}
        yyjson_mut_val *r=yyjson_mut_obj(request);yyjson_mut_doc_set_root(request,r);
        yyjson_mut_obj_add_str(request,r,"action","read");
        yyjson_mut_obj_add_str(request,r,"workspace",workspace && *workspace ? workspace : "main");
        if(buffer_id && *buffer_id)yyjson_mut_obj_add_str(request,r,"buffer_id",buffer_id);
        if(name && *name)yyjson_mut_obj_add_str(request,r,"name",name);
        yyjson_mut_obj_add_int(request,r,"max_bytes",NATIVE_WINDOW_TEXT_CAP-1);
        char *query=yyjson_mut_write(request,0,NULL),*reply=calloc(65536,1);
        yyjson_mut_doc_free(request);
        /* Preserve caller principal, permission grants and taint on the nested
         * persistent-buffer read. No direct store entrypoint bypass. */
        bool read_ok=query && reply && tools_execute_for_tier("buffer",query,tools_execution_tier(),reply,65536);
        yyjson_doc *read_doc=read_ok ? yyjson_read(reply,strlen(reply),0) : NULL;
        yyjson_val *root=read_doc ? yyjson_doc_get_root(read_doc) : NULL;
        yyjson_val *meta=yyjson_obj_get(root,"buffer");
        if(!root || !yyjson_is_true(yyjson_obj_get(root,"ok")) || !str(root,"text") ||
            yyjson_get_len(yyjson_obj_get(root,"text"))>=NATIVE_WINDOW_TEXT_CAP ||
            strlen(str(root,"text"))!=yyjson_get_len(yyjson_obj_get(root,"text")) ||
            !hex_string(str(meta,"buffer_id"),36,true) || !hex_string(str(meta,"revision"),64,false) ||
            yyjson_get_len(yyjson_obj_get(meta,"buffer_id"))!=36 || yyjson_get_len(yyjson_obj_get(meta,"revision"))!=64 ||
            !str(meta,"name") || !yyjson_is_bool(yyjson_obj_get(meta,"sensitive")) ||
            strlen(str(meta,"name"))!=yyjson_get_len(yyjson_obj_get(meta,"name")) ||
            (buffer_id && *buffer_id && !eq(buffer_id,str(meta,"buffer_id"))))
            ok=fail(result,cap,"buffer_read_failed",reply ? reply : "Allocation failed.");
        else if(yyjson_is_true(yyjson_obj_get(root,"truncated")))
            ok=fail(result,cap,"buffer_too_large_to_edit","Native editing requires a complete buffer of at most 8191 bytes; no partial editable view was created.");
        else if(!pixel_tui_session_active())
            ok=fail(result,cap,"native_session_ended","The buffer read completed after the native session ended; no panel was created.");
        else {
            /* Native text is explicitly a bounded snapshot. The persistent
             * buffer stays canonical and carries its actual file revision. */
            ok=native_windows_bind_buffer(id,title && *title ? title : str(meta,"name"),str(root,"text"),
                str(meta,"buffer_id"),str(meta,"revision"),workspace && *workspace ? workspace : "main",
                yyjson_is_true(yyjson_obj_get(meta,"sensitive")),result,cap);
        }
        yyjson_doc_free(read_doc);free(reply);free(query);free(snapshot);
    } else ok=native_windows_command(input,result,cap);
    yyjson_doc_free(d);
    if(ok && !observation)pixel_tui_session_windows_changed(stderr);
    return ok;
}

static bool valid_revision(const char *s) {
    if(!s || strlen(s)!=64)return false;
    for(size_t i=0;i<64;i++)
        if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
    return true;
}

static void save_result_copy(char *out,size_t cap,const char *reply) {
    if(!out || !cap)return;
    if(reply && strlen(reply)<cap)memcpy(out,reply,strlen(reply)+1);
    else if(cap>=3)memcpy(out,"{}",3);
    else out[0]=0;
}

bool native_windows_save_pending(bool *attempted,char *result,size_t cap) {
    if(attempted)*attempted=false;
    if(result && cap)result[0]=0;
    native_window_save_request_t request;
    if(!native_windows_take_save_request(&request))return false;
    if(attempted)*attempted=true;
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root=doc ? yyjson_mut_obj(doc) : NULL;
    if(doc && root) {
        yyjson_mut_doc_set_root(doc,root);
        yyjson_mut_obj_add_str(doc,root,"action","write");
        yyjson_mut_obj_add_str(doc,root,"workspace",request.workspace);
        yyjson_mut_obj_add_str(doc,root,"buffer_id",request.buffer_id);
        yyjson_mut_obj_add_str(doc,root,"expected_revision",request.expected_revision);
        yyjson_mut_obj_add_str(doc,root,"content",request.content);
        char digest[65], request_id[96];
        char *identity=yyjson_mut_write(doc,0,NULL);
        if(!identity) { yyjson_mut_doc_free(doc); native_windows_finish_save(&request,false,NULL,"out_of_memory"); return false; }
        sha256_hex((const uint8_t *)identity,strlen(identity),digest);free(identity);
        snprintf(request_id,sizeof(request_id),"native-editor-%s",digest);
        yyjson_mut_obj_add_strcpy(doc,root,"request_id",request_id);
    }
    char *query=doc&&root ? yyjson_mut_write(doc,0,NULL) : NULL;
    yyjson_mut_doc_free(doc);
    char *reply=calloc(65536,1);
    bool tool_ok=query && reply && tools_execute_for_tier("buffer",query,tools_execution_tier(),reply,65536);
    yyjson_doc *parsed=tool_ok&&reply ? yyjson_read(reply,strlen(reply),0) : NULL;
    yyjson_val *response=parsed ? yyjson_doc_get_root(parsed) : NULL;
    yyjson_val *meta=response ? yyjson_obj_get(response,"buffer") : NULL;
    const char *revision=response ? str(response,"revision") : NULL;
    if(!revision)revision=str(meta,"revision");
    const char *error=response ? str(response,"error") : NULL;
    bool saved=tool_ok && response && yyjson_is_obj(response) && yyjson_is_true(yyjson_obj_get(response,"ok")) &&
        valid_revision(revision) && (!str(meta,"buffer_id") || eq(str(meta,"buffer_id"),request.buffer_id));
    save_result_copy(result,cap,reply ? reply : "{\"ok\":false,\"error\":\"buffer_write_unavailable\"}");
    native_windows_finish_save(&request,saved,revision,error ? error : (tool_ok ? "invalid_buffer_write_reply" : "governed_buffer_write_denied"));
    yyjson_doc_free(parsed);free(reply);free(query);
    return saved;
}

/* Direct human clipboard gestures retain the ordinary tool capability checks.
 * No clipboard I/O occurs under the model/compositor locks. */
bool native_windows_clipboard_key(int key) {
    native_windows_snapshot_t *snap=calloc(1,sizeof(*snap));
    if(!snap)return false;
    native_windows_snapshot(snap);native_window_t *w=NULL;
    for(int i=0;i<snap->count;i++)if(snap->windows[i].id==snap->focused_id)w=&snap->windows[i];
    if(!snap->keyboard_focus || !w || w->sensitive || !w->editor_active || w->kind!=NATIVE_WINDOW_BUFFER){free(snap);return false;}
    size_t a=0,b=0;bool selected=native_buffer_editor_get_selected_range(&w->editor,w->text,&a,&b);
    if(key!=0x16 && !selected){free(snap);return false;}
    char *reply=calloc(65536,1);yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o=d?yyjson_mut_obj(d):NULL;
    if(!reply || !o){free(reply);yyjson_mut_doc_free(d);free(snap);return false;}
    yyjson_mut_doc_set_root(d,o);yyjson_mut_obj_add_str(d,o,"action",key==0x16?"read":"write");
    if(key!=0x16)yyjson_mut_obj_add_strncpy(d,o,"text",w->text+a,b-a);
    char *q=yyjson_mut_write(d,0,NULL);yyjson_mut_doc_free(d);
    bool ok=q && tools_execute_for_tier("clipboard",q,tools_execution_tier(),reply,65536);free(q);
    yyjson_doc *parsed=ok?yyjson_read(reply,strlen(reply),0):NULL;
    yyjson_val *r=parsed?yyjson_doc_get_root(parsed):NULL;
    ok=ok && r && yyjson_is_true(yyjson_obj_get(r,"ok"));
    if(ok && key==0x16) {
        const char *text=str(r,"text");
        ok=text && native_windows_replace_region(w->id,w->edit_generation,w->editor.anchor,w->editor.cursor,text,strlen(text));
    } else if(ok && key==0x18) {
        /* Verify clipboard bytes before destructive cut. */
        char *verify=calloc(65536,1);
        bool read_ok=verify && tools_execute_for_tier("clipboard","{\"action\":\"read\"}",tools_execution_tier(),verify,65536);
        yyjson_doc *vd=read_ok?yyjson_read(verify,strlen(verify),0):NULL;
        yyjson_val *vr=vd?yyjson_doc_get_root(vd):NULL;const char *copied=str(vr,"text");
        ok=vr && yyjson_is_true(yyjson_obj_get(vr,"ok")) && copied && strlen(copied)==b-a && !memcmp(copied,w->text+a,b-a);
        if(ok)ok=native_windows_replace_region(w->id,w->edit_generation,w->editor.anchor,w->editor.cursor,"",0);
        yyjson_doc_free(vd);free(verify);
    }
    yyjson_doc_free(parsed);free(reply);free(snap);return ok;
}

bool native_windows_reuse_key(int key) {
    native_windows_snapshot_t *snap=calloc(1,sizeof(*snap));if(!snap)return false;
    native_windows_snapshot(snap);native_window_t *w=NULL;
    for(int i=0;i<snap->count;i++)if(snap->windows[i].id==snap->focused_id)w=&snap->windows[i];
    if(!snap->keyboard_focus || !w || !w->editor_active || w->kind!=NATIVE_WINDOW_BUFFER || snap->count>=NATIVE_WINDOWS_MAX){free(snap);return false;}
    size_t a,b;if(key==0x02 && !native_buffer_editor_get_selected_range(&w->editor,w->text,&a,&b)){free(snap);return false;}
    char *reply=calloc(65536,1);if(!reply){free(snap);return false;}
    bool ok=false;char new_id[37]={0};
    if(key==0x02) {
        yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_val *o=d?yyjson_mut_obj(d):NULL;
        if(o){yyjson_mut_doc_set_root(d,o);yyjson_mut_obj_add_str(d,o,"action","create");
            yyjson_mut_obj_add_str(d,o,"workspace",w->workspace);yyjson_mut_obj_add_str(d,o,"kind","scratch");
            yyjson_mut_obj_add_bool(d,o,"sensitive",w->sensitive);
            struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);char title[161];
            snprintf(title,sizeof(title),"%.70s region %zu-%zu %lld.%09ld",w->title,a,b,(long long)ts.tv_sec,ts.tv_nsec);
            yyjson_mut_obj_add_strcpy(d,o,"name",title);yyjson_mut_obj_add_strncpy(d,o,"content",w->text+a,b-a);
            char *q=yyjson_mut_write(d,0,NULL);ok=q&&tools_execute_for_tier("buffer",q,tools_execution_tier(),reply,65536);free(q);
        }yyjson_mut_doc_free(d);
        yyjson_doc *rd=ok?yyjson_read(reply,strlen(reply),0):NULL;yyjson_val *r=rd?yyjson_doc_get_root(rd):NULL;
        const char *id=str(yyjson_obj_get(r,"buffer"),"buffer_id");ok=r&&yyjson_is_true(yyjson_obj_get(r,"ok"))&&hex_string(id,36,true);
        if(ok)snprintf(new_id,sizeof(new_id),"%s",id);yyjson_doc_free(rd);
    } else {snprintf(new_id,sizeof(new_id),"%s",w->buffer_id);ok=true;}
    if(ok){yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_val *o=d?yyjson_mut_obj(d):NULL;
        if(!o)ok=false;
        else {yyjson_mut_doc_set_root(d,o);yyjson_mut_obj_add_str(d,o,"action","buffer");
            yyjson_mut_obj_add_str(d,o,"workspace",w->workspace);yyjson_mut_obj_add_str(d,o,"buffer_id",new_id);
            char *q=yyjson_mut_write(d,0,NULL);ok=q&&tools_execute_for_tier("native_window",q,tools_execution_tier(),reply,65536);free(q);}
        yyjson_mut_doc_free(d);
    }
    native_windows_editor_feedback(ok?(key==0x02?"Selected region saved as an independent buffer.":"Saved-revision view opened. Refresh explicitly; concurrent saves are conflict-checked, not live synchronized."):"Buffer reuse failed; original retained. A created copy, if any, remains in buffer list.");
    free(reply);free(snap);return ok;
}
