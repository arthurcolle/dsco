#include "native_windows.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>

/* This model is shared by the tool thread, compositor and human input reader.
 * No callbacks under mu: in particular, repaint and governed reads happen in
 * their adapters after the model has released its lock. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static native_windows_snapshot_t state;
static uint64_t next_id = 1, next_event = 1;
static native_window_action_t pending[NATIVE_WINDOWS_EVENTS_MAX];
static native_window_action_t history[NATIVE_WINDOWS_EVENTS_MAX];
static bool delivered[NATIVE_WINDOWS_EVENTS_MAX];
static int pending_head, pending_count, history_head, history_count;
static int grip_width=16,grip_height=16;
static bool tiled_layout;
static void arrange(bool tiled);
static struct { int mode, x, y, button; uint64_t id; native_ui_rect_t rect; } capture;

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
static int clamp(int v, int lo, int hi) { return max_i(lo, min_i(v, max_i(lo, hi))); }
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a,b); }
static const char *str(yyjson_val *o, const char *key) { return yyjson_get_str(yyjson_obj_get(o,key)); }
static bool inside(native_ui_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
/* Drop terminal control bytes; preserve newlines and UTF-8. JSON parser has
 * already validated UTF-8; byte truncation finishes before a partial codepoint. */
static void text_copy(char *dst, size_t cap, const char *src, bool multiline) {
    size_t n = 0;
    if (!src) src = "";
    const unsigned char *end=(const unsigned char *)src+strlen(src);
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < cap; p++) {
        if (*p < 32 || *p == 127) {
            if (multiline && (*p == '\n' || *p == '\t')) dst[n++] = (char)*p;
            else if (*p == '\n' || *p == '\t') dst[n++] = ' ';
            continue;
        }
        int width = *p < 128 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : 4;
        if (n + (size_t)width >= cap || (size_t)(end-p) < (size_t)width) break;
        memcpy(dst+n,p,(size_t)width); n += (size_t)width; p += width-1;
    }
    dst[n] = 0;
}
static void feedback(const char *s) { text_copy(state.feedback,sizeof(state.feedback),s,false); }
static void changed(void) { state.revision++; }
static int find_id(uint64_t id) {
    for (int i=0;i<state.count;i++) if (state.windows[i].id == id) return i;
    return -1;
}
static native_ui_rect_t bounds(void) {
    native_ui_rect_t r=state.work_area;
    int toolbar=min_i(NATIVE_WINDOWS_TOOLBAR_HEIGHT,max_i(0,r.height));
    r.y+=toolbar; r.height=max_i(0,r.height-toolbar);
    return r;
}
static native_ui_rect_t fit(native_ui_rect_t r) {
    native_ui_rect_t b=bounds();
    r.width=clamp(r.width,min_i(240,b.width),b.width);
    r.height=clamp(r.height,min_i(160,b.height),b.height);
    r.x=clamp(r.x,b.x,b.x+b.width-r.width);
    r.y=clamp(r.y,b.y,b.y+b.height-r.height);
    return r;
}
static void focus(int idx) {
    if (idx < 0 || idx >= state.count) return;
    native_window_t w=state.windows[idx];
    memmove(&state.windows[idx],&state.windows[idx+1],(size_t)(state.count-idx-1)*sizeof(w));
    state.windows[state.count-1]=w;
    state.focused_id=w.id;
}
static void cancel(void) { memset(&capture,0,sizeof(capture)); state.captured_id=0; }
static bool close_idx(int i) {
    if (i < 0 || i >= state.count) return false;
    native_window_t *closing=&state.windows[i];
    if (closing->kind==NATIVE_WINDOW_BUFFER && closing->dirty) {
        feedback("Unsaved buffer edits retained. Press Ctrl-S, then close explicitly.");
        return false;
    }
    uint64_t id=closing->id;
    native_buffer_editor_dispose(&closing->editor);
    memmove(&state.windows[i],&state.windows[i+1],(size_t)(state.count-i-1)*sizeof(state.windows[0]));
    memset(&state.windows[--state.count],0,sizeof(state.windows[0]));
    if (state.focused_id==id) state.focused_id=state.count ? state.windows[state.count-1].id : 0;
    if (capture.id==id) cancel();
    if (!state.count) {state.visible=false;state.keyboard_focus=false;cancel();}
    else if(tiled_layout)arrange(true);
    feedback("Panel closed. Its persistent buffer is retained.");
    return true;
}
static void zoom(native_window_t *w) {
    tiled_layout=false;
    if (!w->zoomed) { w->restore_rect=w->rect; w->rect=bounds(); w->zoomed=true; }
    else { w->rect=fit(w->restore_rect); w->zoomed=false; }
    w->revision++;
}
static void arrange(bool tiled) {
    tiled_layout=tiled;
    native_ui_rect_t b=bounds();
    int cols=1;
    while (cols*cols<state.count) cols++;
    int rows=state.count ? (state.count+cols-1)/cols : 1;
    for (int i=0;i<state.count;i++) {
        native_window_t *w=&state.windows[i]; w->zoomed=false;
        if (tiled) {
            int x0=b.x+(i%cols)*b.width/cols, x1=b.x+(i%cols+1)*b.width/cols;
            int y0=b.y+(i/cols)*b.height/rows, y1=b.y+(i/cols+1)*b.height/rows;
            /* Tiling can use smaller than manual resize minimum, avoiding
             * overlap on a small terminal. Borders remain inside each cell. */
            w->rect=(native_ui_rect_t){x0+2,y0+2,max_i(0,x1-x0-4),max_i(0,y1-y0-4)};
        } else w->rect=fit((native_ui_rect_t){b.x+16+i*24,b.y+12+i*24,b.width*2/3,b.height*2/3});
        w->revision++;
    }
    cancel(); feedback(tiled ? "Panels tiled. Drag a title to rearrange." : "Panels stacked. Click a title to bring it forward.");
}
bool native_window_text_next(const char **cursor,int columns,const char **start,size_t *bytes) {
    if(!cursor || !*cursor || !**cursor || !start || !bytes)return false;
    columns=clamp(columns,1,240);
    const char *begin=*cursor,*at=begin,*space=NULL;
    bool was_space=false;int cells=0;
    while(*at && *at!='\n' && cells<columns) {
        bool is_space=*at==' ' || *at=='\t';
        if(is_space && !was_space)space=at;
        was_space=is_space;
        unsigned char c=(unsigned char)*at;
        size_t n=c<128 ? 1 : (c&0xe0)==0xc0 ? 2 : (c&0xf0)==0xe0 ? 3 : 4;
        /* Valid input is the normal contract; stay bounded even if a caller
         * hands us an incomplete codepoint. */
        for(size_t i=1;i<n;i++)if(!at[i] || ((unsigned char)at[i]&0xc0)!=0x80) {n=1;break;}
        at+=n;cells++;
    }
    const char *end=at,*next=at;
    if(*at=='\n')next=at+1;
    else if(*at) {
        if(*at==' ' || *at=='\t') {
            while(*next==' ' || *next=='\t')next++;
            if(*next=='\n')next++;
        } else if(space && space>begin) {
            end=space;next=space;
            while(*next==' ' || *next=='\t')next++;
        }
    }
    while(end>begin && (end[-1]==' ' || end[-1]=='\t'))end--;
    *start=begin;*bytes=(size_t)(end-begin);*cursor=next;return true;
}
static int line_count(const native_window_t *w) {
    int columns=clamp((w->rect.width-24)/8,1,240),n=0;
    const char *parts[]={w->text,w->next_step[0] ? "\nNEXT STEP" : "",w->next_step,
        w->evidence[0] ? "\nEVIDENCE" : "",w->evidence};
    for(size_t k=0;k<sizeof(parts)/sizeof(parts[0]);k++) {
        const char *cursor=parts[k],*start;size_t bytes;
        while(native_window_text_next(&cursor,columns,&start,&bytes))n++;
    }
    return max_i(1,n);
}

static int utf8_width_at(const char *p) {
    unsigned char c=(unsigned char)*p;
    if(c<0x80)return c ? 1 : 0;
    if((c&0xe0)==0xc0)return 2;
    if((c&0xf0)==0xe0)return 3;
    if((c&0xf8)==0xf0)return 4;
    return 1;
}

/* The renderer and input use the same bounded word-wrap iterator. A cursor
 * remains a byte offset, but its screen position is always a codepoint cell. */
static bool editor_position(const native_window_t *w,int columns,int *row,int *column) {
    if(!w || !row || !column || !native_buffer_editor_cursor_valid(&w->editor,w->text))return false;
    columns=clamp(columns,1,240);size_t target=w->editor.cursor;int r=0;
    const char *cursor=w->text,*start;size_t bytes;
    while(native_window_text_next(&cursor,columns,&start,&bytes)) {
        size_t begin=(size_t)(start-w->text),end=begin+bytes;
        if(target>=begin && target<=end) {
            int col=0;size_t at=begin;
            while(at<target && at<end) {int n=utf8_width_at(w->text+at);if(!n)break;at+=(size_t)n;col++;}
            *row=r;*column=col;return true;
        }
        if(target<(size_t)(cursor-w->text)) { *row=r;*column=(int)(bytes ? columns : 0);return true; }
        r++;
    }
    *row=r;*column=0;return true;
}
static size_t editor_offset_at(const native_window_t *w,int columns,int wanted_row,int wanted_column) {
    if(!w)return 0;columns=clamp(columns,1,240);if(wanted_row<0)wanted_row=0;if(wanted_column<0)wanted_column=0;
    const char *cursor=w->text,*start;size_t bytes;int r=0;
    while(native_window_text_next(&cursor,columns,&start,&bytes)) {
        if(r++!=wanted_row)continue;
        size_t at=(size_t)(start-w->text),end=at+bytes;int col=0;
        while(at<end && col<wanted_column) {int n=utf8_width_at(w->text+at);if(!n)break;at+=(size_t)n;col++;}
        return at;
    }
    return strlen(w->text);
}
static void editor_keep_visible(native_window_t *w) {
    if(!w)return;int row=0,col=0,columns=clamp((w->rect.width-24)/8,1,240);
    if(!editor_position(w,columns,&row,&col))return;
    int available=max_i(1,(w->rect.height-NATIVE_WINDOW_TITLE_HEIGHT-38-
        (w->kind==NATIVE_WINDOW_WORKFLOW ? NATIVE_WINDOW_FOOTER_HEIGHT : 0))/18);
    if(row<w->scroll)w->scroll=row;
    if(row>=w->scroll+available)w->scroll=row-available+1;
    w->scroll=clamp(w->scroll,0,max_i(0,line_count(w)-available));
}
static bool editor_find(native_window_t *w,bool backwards) {
    size_t n=strlen(w->search_text),len=strlen(w->text);if(!n || n>len)return false;
    size_t lo=w->editor.anchor<w->editor.cursor?w->editor.anchor:w->editor.cursor;
    size_t hi=w->editor.anchor>w->editor.cursor?w->editor.anchor:w->editor.cursor;
    const char *hit=NULL;
    if(!backwards){hit=strstr(w->text+hi,w->search_text);if(!hit)hit=strstr(w->text,w->search_text);}
    else {for(const char *p=w->text;(p=strstr(p,w->search_text));p++){if((size_t)(p-w->text)<lo)hit=p;else break;}
        if(!hit)for(const char *p=w->text;(p=strstr(p,w->search_text));p++)hit=p;}
    if(!hit){feedback("No match in this buffer.");return false;}
    size_t at=(size_t)(hit-w->text);
    (void)native_buffer_editor_set_caret(&w->editor,w->text,at);
    if(!native_buffer_editor_extend_caret(&w->editor,w->text,at+n))return false;
    w->revision++;editor_keep_visible(w);return true;
}
static void editor_changed(native_window_t *w) {
    if(!w)return;w->dirty=true;w->edit_generation++;w->revision++;editor_keep_visible(w);
}
static void scroll_by(native_window_t *w, int lines) {
    int available=max_i(1,(w->rect.height-NATIVE_WINDOW_TITLE_HEIGHT-38-
        (w->kind==NATIVE_WINDOW_WORKFLOW ? NATIVE_WINDOW_FOOTER_HEIGHT : 0))/18);
    w->scroll=clamp(w->scroll+lines,0,max_i(0,line_count(w)-available));
    w->revision++;
}
static native_window_t *new_window(void) {
    if (state.count>=NATIVE_WINDOWS_MAX) return NULL;
    tiled_layout=false;
    native_window_t *w=&state.windows[state.count++]; memset(w,0,sizeof(*w));
    w->id=next_id++; w->revision=1;
    native_buffer_editor_reset(&w->editor,0);
    native_ui_rect_t b=bounds();
    int offset=(state.count-1)*24;
    w->rect=fit((native_ui_rect_t){b.x+16+offset,b.y+12+offset,520,360});
    text_copy(w->title,sizeof(w->title),"Untitled",false);
    state.focused_id=w->id; state.visible=true;
    return w;
}
static void show(void) {
    state.visible=true;
    if (!state.count) {
        native_window_t *w=new_window();
        text_copy(w->title,sizeof(w->title),"Your workspace",false);
        text_copy(w->text,sizeof(w->text),
            "Keep the outcome, evidence and next step visible while DSCO works.\n\n"
            "Ask DSCO to open a workflow or buffer here. Drag a title to move a panel; drag its lower-right corner to resize.\n\n"
            "Ctrl+G focuses panels. Tab switches panels; arrows move; Shift+arrows resize. Z zooms, T tiles, R stacks, X closes. Escape returns to your draft.\n\n"
            "Workflow buttons send your intent to DSCO. Window movement never submits your draft.",true);
    }
    feedback("Drag titles to move panels. Ctrl+G for keyboard controls; Escape returns to your draft.");
}
const char *native_window_kind_name(native_window_kind_t k) {
    return k==NATIVE_WINDOW_WORKFLOW ? "workflow" : k==NATIVE_WINDOW_BUFFER ? "buffer" : "note";
}
const char *native_window_status_name(native_window_status_t s) {
    static const char *names[]={"ready","running","blocked","done"};
    return s>=NATIVE_WINDOW_READY && s<=NATIVE_WINDOW_DONE ? names[s] : "ready";
}
const char *native_window_action_name(native_window_action_kind_t k) {
    static const char *names[]={"continue","revise","retry","inspect"};
    return k>=NATIVE_WINDOW_CONTINUE && k<=NATIVE_WINDOW_INSPECT ? names[k] : "inspect";
}
void native_windows_reset(void) {
    pthread_mutex_lock(&mu);
    for(int i=0;i<state.count;i++) native_buffer_editor_dispose(&state.windows[i].editor);
    memset(&state,0,sizeof(state)); state.work_area=(native_ui_rect_t){0,60,1000,600};
    memset(pending,0,sizeof(pending)); memset(history,0,sizeof(history)); memset(delivered,0,sizeof(delivered));
    pending_head=pending_count=history_head=history_count=0; next_id=next_event=1; cancel();
    grip_width=grip_height=16;
    tiled_layout=false;
    pthread_mutex_unlock(&mu);
}
void native_windows_set_work_area(native_ui_rect_t r) {
    r.x=clamp(r.x,0,16384);r.y=clamp(r.y,0,16384);
    r.width=clamp(r.width,0,16384);r.height=clamp(r.height,0,16384);
    pthread_mutex_lock(&mu);
    if (memcmp(&r,&state.work_area,sizeof(r))) {
        state.work_area=r; cancel();
        if(tiled_layout)arrange(true);
        else for(int i=0;i<state.count;i++) {
            native_window_t *w=&state.windows[i];
            w->rect=w->zoomed ? bounds() : fit(w->rect); w->revision++;
            scroll_by(w,0);
        }
        changed();
    }
    pthread_mutex_unlock(&mu);
}
void native_windows_snapshot(native_windows_snapshot_t *out) {
    if (!out) return;
    pthread_mutex_lock(&mu); *out=state; pthread_mutex_unlock(&mu);
}
bool native_windows_visible(void) { pthread_mutex_lock(&mu); bool v=state.visible; pthread_mutex_unlock(&mu); return v; }
bool native_windows_focused(void) { pthread_mutex_lock(&mu); bool v=state.visible && state.keyboard_focus; pthread_mutex_unlock(&mu); return v; }
bool native_windows_sensitive(void) {
    pthread_mutex_lock(&mu); bool v=false;
    for (int i=0;i<state.count;i++) v|=state.windows[i].sensitive;
    pthread_mutex_unlock(&mu); return v;
}
void native_windows_cancel_gesture(void) { pthread_mutex_lock(&mu);cancel();pthread_mutex_unlock(&mu); }

static bool fail(char *out,size_t cap,const char *error) {
    if(out && cap) {
        int n=snprintf(out,cap,"{\"ok\":false,\"error\":\"%s\"}",error);
        if(n<0 || (size_t)n>=cap) {
            if(cap>=13)memcpy(out,"{\"ok\":false}",13);
            else if(cap>=3)memcpy(out,"{}",3);
            else out[0]=0;
        }
    }
    return false;
}
static const char *validate_obj(yyjson_val *root) {
    if (!yyjson_is_obj(root) || !str(root,"action")) return "object_with_action_required";
    yyjson_doc *schema=yyjson_read(NATIVE_WINDOW_SCHEMA,strlen(NATIVE_WINDOW_SCHEMA),0);
    if (!schema) return "out_of_memory";
    yyjson_val *props=yyjson_obj_get(yyjson_doc_get_root(schema),"properties");
    const char *error=NULL;
    size_t i,n; yyjson_val *key,*v;
    yyjson_obj_foreach(root,i,n,key,v) {
        const char *name=yyjson_get_str(key);
        yyjson_val *rule=yyjson_obj_get(props,name);
        if (!rule || strlen(name)!=yyjson_get_len(key)) {error="unknown_field";break;}
        size_t j,m; yyjson_val *prior,*unused;
        yyjson_obj_foreach(root,j,m,prior,unused) {
            (void)unused; if(j==i)break;
            if(yyjson_equals_str(prior,name)) {error="duplicate_field";break;}
        }
        if(error)break;
        if(eq(str(rule,"type"),"string")) {
            const char *s=yyjson_get_str(v);
            if(!s || strlen(s)!=yyjson_get_len(v)) {error="invalid_string";break;}
            if(yyjson_get_len(v)>yyjson_get_uint(yyjson_obj_get(rule,"maxLength")) && yyjson_obj_get(rule,"maxLength")) {error="string_too_long";break;}
            yyjson_val *choices=yyjson_obj_get(rule,"enum");
            if(choices) {
                bool found=false;size_t j,m;yyjson_val *choice;
                yyjson_arr_foreach(choices,j,m,choice) if(yyjson_equals_str(choice,s))found=true;
                if(!found) {error="unsupported_value";break;}
            }
        } else {
            if(!yyjson_is_int(v) || (yyjson_is_uint(v) && yyjson_get_uint(v)>INT64_MAX)) {error="integer_required";break;}
            int64_t value=yyjson_get_sint(v);
            yyjson_val *lo=yyjson_obj_get(rule,"minimum"),*hi=yyjson_obj_get(rule,"maximum");
            if((lo && value<yyjson_get_sint(lo)) || (hi && value>yyjson_get_sint(hi))) {error="integer_out_of_range";break;}
        }
    }
    yyjson_doc_free(schema); return error;
}
const char *native_windows_validate(const char *json) {
    size_t n=json ? strnlen(json,128u*1024u) : 0;
    yyjson_doc *d=n && n<128u*1024u ? yyjson_read(json,n,0) : NULL;
    const char *error=d ? validate_obj(yyjson_doc_get_root(d)) : "invalid_json";
    yyjson_doc_free(d); return error;
}
static void rect_json(yyjson_mut_doc *d,yyjson_mut_val *o,const char *key,native_ui_rect_t r) {
    yyjson_mut_val *v=yyjson_mut_obj(d); yyjson_mut_obj_add_val(d,o,key,v);
    yyjson_mut_obj_add_int(d,v,"x",r.x);yyjson_mut_obj_add_int(d,v,"y",r.y);
    yyjson_mut_obj_add_int(d,v,"width",r.width);yyjson_mut_obj_add_int(d,v,"height",r.height);
}
static void window_json(yyjson_mut_doc *d,yyjson_mut_val *a,const native_window_t *w) {
    yyjson_mut_val *o=yyjson_mut_obj(d);yyjson_mut_arr_add_val(a,o);
    yyjson_mut_obj_add_uint(d,o,"id",w->id);yyjson_mut_obj_add_uint(d,o,"revision",w->revision);
    yyjson_mut_obj_add_strcpy(d,o,"title",w->title);
    yyjson_mut_obj_add_str(d,o,"kind",native_window_kind_name(w->kind));
    yyjson_mut_obj_add_str(d,o,"status",native_window_status_name(w->status));
    yyjson_mut_obj_add_bool(d,o,"status_is_agent_reported",true);
    yyjson_mut_obj_add_bool(d,o,"zoomed",w->zoomed);yyjson_mut_obj_add_int(d,o,"scroll",w->scroll);
    yyjson_mut_obj_add_bool(d,o,"dirty",w->dirty);
    yyjson_mut_obj_add_bool(d,o,"editor_active",w->editor_active);
    yyjson_mut_obj_add_uint(d,o,"cursor",w->editor.cursor);
    yyjson_mut_obj_add_uint(d,o,"anchor",w->editor.anchor);
    rect_json(d,o,"rect",w->rect);
    char preview[513];text_copy(preview,sizeof(preview),w->text,true);
    yyjson_mut_obj_add_strcpy(d,o,"text_preview",preview);
    yyjson_mut_obj_add_bool(d,o,"text_truncated",strlen(w->text)>strlen(preview));
    yyjson_mut_obj_add_strcpy(d,o,"evidence",w->evidence);
    yyjson_mut_obj_add_strcpy(d,o,"next_step",w->next_step);
    if(w->buffer_id[0]) {
        yyjson_mut_obj_add_strcpy(d,o,"buffer_id",w->buffer_id);
        yyjson_mut_obj_add_strcpy(d,o,"buffer_revision",w->buffer_revision);
        yyjson_mut_obj_add_strcpy(d,o,"workspace",w->workspace);
        yyjson_mut_obj_add_bool(d,o,"snapshot",true);
    }
}
/* Caller holds model lock. JSON has copied all mutable strings before return. */
static bool result_json(char *out,size_t cap,uint64_t selected,bool events,uint64_t since) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);
    if(!d)return fail(out,cap,"out_of_memory");
    yyjson_mut_val *o=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,o);
    yyjson_mut_obj_add_bool(d,o,"ok",true);
    yyjson_mut_obj_add_uint(d,o,"revision",state.revision);
    yyjson_mut_obj_add_bool(d,o,"visible",state.visible);
    yyjson_mut_obj_add_bool(d,o,"keyboard_focus",state.keyboard_focus);
    yyjson_mut_obj_add_uint(d,o,"focused_id",state.focused_id);
    if(selected)yyjson_mut_obj_add_uint(d,o,"id",selected);
    rect_json(d,o,"work_area",state.work_area);
    yyjson_mut_obj_add_strcpy(d,o,"feedback",state.feedback);
    if(events) {
        yyjson_mut_val *a=yyjson_mut_arr(d);yyjson_mut_obj_add_val(d,o,"events",a);
        int count=0;uint64_t last=since;bool more=false;
        for(int i=0;i<history_count;i++) {
            int idx=(history_head+i)%NATIVE_WINDOWS_EVENTS_MAX;
            const native_window_action_t *e=&history[idx];
            if(e->event_id<=since)continue;
            if(count++==16) {more=true;break;}
            yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_arr_add_val(a,v);
            yyjson_mut_obj_add_uint(d,v,"event_id",e->event_id);
            yyjson_mut_obj_add_uint(d,v,"window_id",e->window_id);
            yyjson_mut_obj_add_str(d,v,"action",native_window_action_name(e->kind));
            yyjson_mut_obj_add_strcpy(d,v,"title",e->title);
            yyjson_mut_obj_add_bool(d,v,"delivered",delivered[idx]);
            yyjson_mut_obj_add_str(d,v,"source","terminal_input");last=e->event_id;
        }
        yyjson_mut_obj_add_uint(d,o,"next_since",last);yyjson_mut_obj_add_bool(d,o,"has_more",more);
        yyjson_mut_obj_add_uint(d,o,"oldest_event_id",history_count ? history[history_head].event_id : 0);
        yyjson_mut_obj_add_int(d,o,"pending",pending_count);
    } else {
        yyjson_mut_val *a=yyjson_mut_arr(d);yyjson_mut_obj_add_val(d,o,"windows",a);
        for(int i=0;i<state.count;i++) if(!selected || state.windows[i].id==selected)window_json(d,a,&state.windows[i]);
    }
    char *json=yyjson_mut_write(d,0,NULL);
    bool ok=json && out && cap;
    if(ok && strlen(json)<cap)memcpy(out,json,strlen(json)+1);
    else if(ok)snprintf(out,cap,"{\"ok\":true,\"id\":%llu,\"revision\":%llu,\"result_truncated\":true}",
        (unsigned long long)selected,(unsigned long long)state.revision);
    free(json);yyjson_mut_doc_free(d);return ok;
}

bool native_windows_command(const char *json,char *out,size_t cap) {
    if(!out || cap<128)return fail(out,cap,"output_capacity_too_small");
    const char *error=native_windows_validate(json);
    if(error)return fail(out,cap,error);
    yyjson_doc *d=yyjson_read(json,strlen(json),0);
    yyjson_val *o=yyjson_doc_get_root(d);const char *action=str(o,"action");
    uint64_t id=yyjson_get_uint(yyjson_obj_get(o,"id"));
    bool read=eq(action,"list") || eq(action,"events");
    pthread_mutex_lock(&mu);
    int idx=find_id(id);native_window_t *w=idx>=0 ? &state.windows[idx] : NULL;
    bool needs=eq(action,"update") || eq(action,"move") || eq(action,"resize") || eq(action,"focus") ||
        eq(action,"zoom") || eq(action,"close") || eq(action,"scroll");
    if(needs && !w)error="window_not_found";
    if(eq(action,"close") && w && w->kind==NATIVE_WINDOW_BUFFER && w->dirty)
        error="dirty_buffer_requires_explicit_save";
    if(eq(action,"buffer") || eq(action,"refresh"))error="buffer_requires_governed_adapter";
    if(eq(action,"move") && (!yyjson_obj_get(o,"x") || !yyjson_obj_get(o,"y")))error="x_and_y_required";
    if(eq(action,"resize") && (!yyjson_obj_get(o,"width") || !yyjson_obj_get(o,"height")))error="width_and_height_required";
    if(eq(action,"scroll") && !yyjson_obj_get(o,"lines"))error="lines_required";
    native_window_t candidate={0};
    if(eq(action,"open") || eq(action,"update")) {
        if(w && eq(action,"update"))candidate=*w;
        const char *s=str(o,"kind");if(s)candidate.kind=eq(s,"workflow") ? NATIVE_WINDOW_WORKFLOW : NATIVE_WINDOW_NOTE;
        else if(candidate.kind==NATIVE_WINDOW_NOTE && (str(o,"status") || str(o,"evidence") || str(o,"next_step")))candidate.kind=NATIVE_WINDOW_WORKFLOW;
        s=str(o,"title");if(s)text_copy(candidate.title,sizeof(candidate.title),s,false);
        s=str(o,"text");if(s)text_copy(candidate.text,sizeof(candidate.text),s,true);
        s=str(o,"evidence");if(s)text_copy(candidate.evidence,sizeof(candidate.evidence),s,true);
        s=str(o,"next_step");if(s)text_copy(candidate.next_step,sizeof(candidate.next_step),s,true);
        s=str(o,"status");if(s)candidate.status=eq(s,"done") ? NATIVE_WINDOW_DONE : eq(s,"blocked") ? NATIVE_WINDOW_BLOCKED : eq(s,"running") ? NATIVE_WINDOW_RUNNING : NATIVE_WINDOW_READY;
        if(candidate.status==NATIVE_WINDOW_DONE && !candidate.evidence[0])error="done_requires_evidence";
        if(eq(action,"open") && state.count>=NATIVE_WINDOWS_MAX)error="window_limit";
        if(eq(action,"update") && w && w->kind==NATIVE_WINDOW_BUFFER &&
            (str(o,"text") || str(o,"kind")))error="buffer_snapshot_use_refresh";
    }
    if(!error) {
        if(eq(action,"open")) {
            native_window_t *fresh=new_window();
            candidate.id=fresh->id;candidate.revision=1;candidate.rect=fresh->rect;
            if(!candidate.title[0])text_copy(candidate.title,sizeof(candidate.title),"Untitled",false);
            if(yyjson_obj_get(o,"x"))candidate.rect.x=(int)yyjson_get_sint(yyjson_obj_get(o,"x"));
            if(yyjson_obj_get(o,"y"))candidate.rect.y=(int)yyjson_get_sint(yyjson_obj_get(o,"y"));
            if(yyjson_obj_get(o,"width"))candidate.rect.width=(int)yyjson_get_sint(yyjson_obj_get(o,"width"));
            if(yyjson_obj_get(o,"height"))candidate.rect.height=(int)yyjson_get_sint(yyjson_obj_get(o,"height"));
            candidate.rect=fit(candidate.rect);*fresh=candidate;id=fresh->id;
            feedback("Panel opened. Drag its title or use Ctrl+G to arrange it.");
        } else if(eq(action,"update")) {candidate.revision++;*w=candidate;scroll_by(w,0);}
        else if(eq(action,"move")) {tiled_layout=false;w->zoomed=false;w->rect.x=(int)yyjson_get_sint(yyjson_obj_get(o,"x"));w->rect.y=(int)yyjson_get_sint(yyjson_obj_get(o,"y"));w->rect=fit(w->rect);w->revision++;cancel();}
        else if(eq(action,"resize")) {tiled_layout=false;w->zoomed=false;w->rect.width=(int)yyjson_get_sint(yyjson_obj_get(o,"width"));w->rect.height=(int)yyjson_get_sint(yyjson_obj_get(o,"height"));w->rect=fit(w->rect);scroll_by(w,0);cancel();}
        else if(eq(action,"focus")) {focus(idx);state.visible=true;}
        else if(eq(action,"zoom")) {zoom(w);cancel();}
        else if(eq(action,"tile"))arrange(true);
        else if(eq(action,"cascade"))arrange(false);
        else if(eq(action,"close"))close_idx(idx);
        else if(eq(action,"show"))show();
        else if(eq(action,"hide")) {state.visible=false;state.keyboard_focus=false;cancel();}
        else if(eq(action,"scroll"))scroll_by(w,(int)yyjson_get_sint(yyjson_obj_get(o,"lines")));
        if(!read)changed();
    }
    bool ok=error ? fail(out,cap,error) : result_json(out,cap,id,eq(action,"events"),yyjson_get_uint(yyjson_obj_get(o,"since")));
    pthread_mutex_unlock(&mu);yyjson_doc_free(d);return ok;
}

bool native_windows_bind_buffer(uint64_t id,const char *title,const char *text,const char *buffer_id,
    const char *revision,const char *workspace,bool sensitive,char *out,size_t cap) {
    if(!out || cap<128)return fail(out,cap,"output_capacity_too_small");
    pthread_mutex_lock(&mu);
    int i=find_id(id);native_window_t *w=i>=0 ? &state.windows[i] : NULL;
    const char *error=NULL;
    if(id && (!w || w->kind!=NATIVE_WINDOW_BUFFER))error="buffer_window_not_found";
    char checked[NATIVE_WINDOW_TEXT_CAP];
    if(!text || strlen(text)>=sizeof(checked))error="buffer_too_large_to_edit";
    else { text_copy(checked,sizeof(checked),text,true); if(strcmp(checked,text))error="buffer_contains_unsupported_controls"; }
    if(id && w && w->dirty)error="dirty_buffer_requires_explicit_save";
    if(!id && state.count>=NATIVE_WINDOWS_MAX)error="window_limit";
    if(!error) {
        if(!w)w=new_window();
        w->kind=NATIVE_WINDOW_BUFFER;w->sensitive=sensitive;
        text_copy(w->title,sizeof(w->title),title,false);text_copy(w->text,sizeof(w->text),text,true);
        text_copy(w->buffer_id,sizeof(w->buffer_id),buffer_id,false);
        text_copy(w->buffer_revision,sizeof(w->buffer_revision),revision,false);
        text_copy(w->workspace,sizeof(w->workspace),workspace,false);
        native_buffer_editor_reset(&w->editor,strlen(w->text));
        w->editor_active=false;w->dirty=false;w->save_in_flight=false;w->edit_generation++;
        w->revision++;id=w->id;scroll_by(w,0);changed();
        feedback("Buffer snapshot loaded. Click its text to edit. Ctrl-S saves explicitly; dirty text is retained on close/refresh.");
    }
    bool ok=error ? fail(out,cap,error) : result_json(out,cap,id,false,0);
    pthread_mutex_unlock(&mu);return ok;
}

uint64_t native_windows_paste_target(void) {
    pthread_mutex_lock(&mu);int i=find_id(state.focused_id);uint64_t id=0;
    if(state.visible && state.keyboard_focus && i>=0 && state.windows[i].kind==NATIVE_WINDOW_BUFFER && state.windows[i].editor_active)id=state.windows[i].id;
    pthread_mutex_unlock(&mu);return id;
}
bool native_windows_insert_paste(uint64_t id,const char *text,size_t len) {
    pthread_mutex_lock(&mu);int i=find_id(id);bool ok=false;
    if(i>=0 && state.windows[i].kind==NATIVE_WINDOW_BUFFER) {
        native_window_t *w=&state.windows[i];
        ok=native_buffer_editor_insert(&w->editor,w->text,sizeof(w->text),text,len);
        if(ok) {w->editor.pending_len=0;editor_changed(w);}
    }
    if(!ok)feedback("Paste rejected: original buffer unavailable, invalid UTF-8, or 8191-byte limit; document unchanged.");
    changed();pthread_mutex_unlock(&mu);return ok;
}

void native_windows_editor_feedback(const char *message) {
    pthread_mutex_lock(&mu);feedback(message);changed();pthread_mutex_unlock(&mu);
}
bool native_windows_replace_region(uint64_t id,uint64_t generation,size_t anchor,size_t cursor,const char *text,size_t len) {
    pthread_mutex_lock(&mu);int i=find_id(id);bool ok=false;
    native_window_t *w=i>=0?&state.windows[i]:NULL;
    if(w && w->kind==NATIVE_WINDOW_BUFFER && w->edit_generation==generation &&
       w->editor.anchor==anchor && w->editor.cursor==cursor) {
        ok=len ? native_buffer_editor_insert(&w->editor,w->text,sizeof(w->text),text,len) :
            native_buffer_editor_delete_selection(&w->editor,w->text);
        if(ok)editor_changed(w);
    }
    if(!ok)feedback("Clipboard edit rejected: selection changed or content exceeds buffer capacity.");
    changed();pthread_mutex_unlock(&mu);return ok;
}

bool native_windows_take_save_request(native_window_save_request_t *out) {
    if(!out)return false;
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&mu);
    native_window_t *w=NULL;
    int i=find_id(state.focused_id);
    if(i>=0)w=&state.windows[i];
    bool ok=w && w->kind==NATIVE_WINDOW_BUFFER && w->editor_active && w->dirty && !w->save_in_flight;
    if(ok && w->editor.pending_len) {
        feedback("Save waits for a complete UTF-8 character; finish the current input first.");
        ok=false;
    }
    if(ok) {
        out->window_id=w->id;out->edit_generation=w->edit_generation;
        text_copy(out->buffer_id,sizeof(out->buffer_id),w->buffer_id,false);
        text_copy(out->expected_revision,sizeof(out->expected_revision),w->buffer_revision,false);
        text_copy(out->workspace,sizeof(out->workspace),w->workspace,false);
        memcpy(out->content,w->text,sizeof(out->content));
        w->save_in_flight=true;w->revision++;
        feedback("Saving buffer through the governed buffer adapter...");
        changed();
    }
    pthread_mutex_unlock(&mu);return ok;
}

void native_windows_finish_save(const native_window_save_request_t *request,bool ok,
                                const char *revision,const char *error) {
    if(!request)return;
    pthread_mutex_lock(&mu);
    int i=find_id(request->window_id);native_window_t *w=i>=0 ? &state.windows[i] : NULL;
    if(w && w->kind==NATIVE_WINDOW_BUFFER && w->save_in_flight) {
        if(ok && revision && strlen(revision)==64) {
            text_copy(w->buffer_revision,sizeof(w->buffer_revision),revision,false);
            bool same_edit=w->edit_generation==request->edit_generation && !strcmp(w->text,request->content);
            if(same_edit)w->dirty=false;
            w->save_in_flight=false;
            feedback(same_edit ? "Buffer saved. Close is now safe." :
                     "Buffer snapshot saved; newer edits remain dirty. Save again when ready.");
        } else {
            w->save_in_flight=false;
            char message[192];snprintf(message,sizeof(message),"Save failed (%s); edits retained.",error && *error ? error : "governed buffer write");
            feedback(message);
        }
        w->revision++;changed();
    }
    pthread_mutex_unlock(&mu);
}

bool native_window_editor_position(const native_window_t *window,int columns,int *row,int *column) {
    return editor_position(window,columns,row,column);
}

static void enqueue_action(native_window_t *w,int button) {
    if(pending_count>=NATIVE_WINDOWS_EVENTS_MAX) {feedback("Action queue is full. Wait for DSCO to receive your earlier requests.");return;}
    native_window_action_t e={.event_id=next_event++,.window_id=w->id,.kind=(native_window_action_kind_t)button};
    text_copy(e.title,sizeof(e.title),w->title,false);text_copy(e.objective,sizeof(e.objective),w->text,true);
    text_copy(e.next_step,sizeof(e.next_step),w->next_step,true);
    pending[(pending_head+pending_count++)%NATIVE_WINDOWS_EVENTS_MAX]=e;
    if(history_count==NATIVE_WINDOWS_EVENTS_MAX) {history_head=(history_head+1)%NATIVE_WINDOWS_EVENTS_MAX;history_count--;}
    int h=(history_head+history_count++)%NATIVE_WINDOWS_EVENTS_MAX;history[h]=e;delivered[h]=false;
    char msg[192];snprintf(msg,sizeof(msg),"%s requested for %.90s. DSCO will receive it at the next safe boundary.",native_window_action_name(e.kind),w->title);
    feedback(msg);state.keyboard_focus=false;
}
/* Controls activate on release over their original target. Gestures keep the
 * original ID/geometry even when crossing overlapping windows or their edges. */
static int hit_control(const native_window_t *w,int x,int y) {
    native_ui_rect_t r=w->rect;
    if(!inside(r,x,y))return 0;
    if(x>=r.x+r.width-grip_width && y>=r.y+r.height-grip_height)return 3; /* resize */
    if(y<r.y+NATIVE_WINDOW_TITLE_HEIGHT) {
        if(x>=r.x+r.width-28)return 4;
        if(x>=r.x+r.width-56)return 5;
        return 1;
    }
    if(w->kind==NATIVE_WINDOW_WORKFLOW && y>=r.y+r.height-NATIVE_WINDOW_FOOTER_HEIGHT)
        return 10+clamp((x-r.x)*4/max_i(1,r.width),0,3);
    return 2;
}
void native_windows_set_pointer_cell(int width,int height) {
    pthread_mutex_lock(&mu);
    grip_width=max_i(16,clamp(width,1,128)+1);grip_height=max_i(16,clamp(height,1,128)+1);
    pthread_mutex_unlock(&mu);
}
bool native_windows_pointer(int button,int x,int y,bool released) {
    pthread_mutex_lock(&mu);bool consumed=false;
    if(!state.visible)goto done;
    if(capture.mode) {
        consumed=true;int i=find_id(capture.id);native_window_t *w=i>=0 ? &state.windows[i] : NULL;
        if(w && capture.mode==21 && ((button&32) || released)) {
            int row=w->scroll+max_i(0,(y-(w->rect.y+NATIVE_WINDOW_TITLE_HEIGHT+29))/18);
            int col=max_i(0,(x-w->rect.x-12)/8);
            w->editor.cursor=editor_offset_at(w,clamp((w->rect.width-24)/8,1,240),row,col);
            w->revision++;editor_keep_visible(w);changed();
        }
        if(released) {
            if(capture.mode==20) {
                native_ui_rect_t a=state.work_area;int cw=min_i(80,a.width/3),start=a.x+a.width-3*cw;
                if(cw>0 && y>=a.y && y<a.y+32 && x>=start && x<a.x+a.width && (x-start)/cw==capture.button) {
                    if(capture.button<2)arrange(capture.button==0);
                    else {state.visible=false;state.keyboard_focus=false;feedback("Workspace hidden. Ctrl+G restores it.");}
                }
            } else if(w && hit_control(w,x,y)==capture.mode) {
                if(capture.mode==4)close_idx(i);
                else if(capture.mode==5)zoom(w);
                else if(capture.mode>=10 && capture.mode<=13)enqueue_action(w,capture.mode-10);
            }
            cancel();changed();goto done;
        }
        if((button&32) && w && (capture.mode==1 || capture.mode==3)) {
            int dx=clamp(x,-32768,32768)-capture.x,dy=clamp(y,-32768,32768)-capture.y;
            tiled_layout=false;w->zoomed=false;w->rect=capture.rect;
            if(capture.mode==1) {w->rect.x+=dx;w->rect.y+=dy;}
            else {w->rect.width+=dx;w->rect.height+=dy;}
            w->rect=fit(w->rect);scroll_by(w,0);changed();
        }
        goto done;
    }
    if(released || (button&32))goto done;
    if(!inside(state.work_area,x,y)) {state.keyboard_focus=false;goto done;}
    for(int i=state.count-1;i>=0;i--) {
        native_window_t *w=&state.windows[i];
        if(!inside(w->rect,x,y))continue;
        consumed=true;
        if(button&64) {scroll_by(w,(button&1) ? 3 : -3);changed();goto done;}
        if((button&3)!=0)goto done;
        int mode=hit_control(w,x,y);uint64_t id=w->id;native_ui_rect_t r=w->rect;
        focus(i);state.keyboard_focus=true;
        i=find_id(id);w=i>=0 ? &state.windows[i] : NULL;
        if(mode==2 && w && w->kind==NATIVE_WINDOW_BUFFER) {
            int columns=clamp((w->rect.width-24)/8,1,240);
            int body_top=w->rect.y+NATIVE_WINDOW_TITLE_HEIGHT+29;
            int visual_row=w->scroll+max_i(0,(y-body_top)/18);
            int visual_col=max_i(0,(x-w->rect.x-12)/8);
            w->editor.cursor=editor_offset_at(w,columns,visual_row,visual_col);
            w->editor.anchor=w->editor.cursor;w->editor.pending_len=0;w->editor_active=true;editor_keep_visible(w);
            mode=21; /* body click: caret placement, never a layout gesture */
            changed();
        }
        capture.mode=mode;capture.id=id;capture.x=clamp(x,-32768,32768);capture.y=clamp(y,-32768,32768);capture.rect=r;
        state.captured_id=id;changed();goto done;
    }
    if(y<state.work_area.y+32 && !(button&64) && (button&3)==0) {
        int cw=min_i(80,state.work_area.width/3),start=state.work_area.x+state.work_area.width-cw*3;
        if(cw>0 && x>=start) {capture.mode=20;capture.button=clamp((x-start)/cw,0,2);consumed=true;}
    }
done:
    pthread_mutex_unlock(&mu);return consumed;
}
bool native_windows_key(int key,unsigned mods) {
    pthread_mutex_lock(&mu);bool consumed=false;
    if(key==NATIVE_WINDOW_KEY_TOGGLE_FOCUS) {
        if(!state.visible)show();
        state.keyboard_focus=!state.keyboard_focus;
        int focused=find_id(state.focused_id);
        if(focused>=0 && state.windows[focused].kind==NATIVE_WINDOW_BUFFER)
            state.windows[focused].editor_active=state.keyboard_focus;
        feedback(state.keyboard_focus ? "Panels focused. Buffer text accepts typing; Ctrl-S saves. Esc returns to draft."
                                      : "Draft focused. Panels remain visible.");
        consumed=true;goto changed;
    }
    if(!state.visible || !state.keyboard_focus)goto done;
    consumed=true;
    int i=find_id(state.focused_id);native_window_t *w=i>=0 ? &state.windows[i] : NULL;
    if(key==NATIVE_WINDOW_KEY_ESCAPE) {
        if(w && w->searching){w->searching=false;w->revision++;goto changed;}
        if(w && w->editor_active && w->editor.anchor!=w->editor.cursor) {
            w->editor.anchor=w->editor.cursor;w->revision++;goto changed;
        }
        state.keyboard_focus=false;int focused=find_id(state.focused_id);
        if(focused>=0)state.windows[focused].editor_active=false;
        cancel();feedback("Draft focused. Panels remain visible.");goto changed;
    }

    if((key=='\r' || key=='\n') && !(w && w->kind==NATIVE_WINDOW_BUFFER && w->editor_active)) {
        state.keyboard_focus=false;if(w)w->editor_active=false;cancel();
        feedback("Draft focused. Press Enter again to send it.");goto changed;
    }
    if(key==NATIVE_WINDOW_KEY_TAB && state.count) {
        /* Stable IDs, not current z-order, determine keyboard cycle. */
        int target=-1;uint64_t best=(mods&NATIVE_WINDOW_MOD_SHIFT) ? 0 : UINT64_MAX;
        for(int j=0;j<state.count;j++) {
            uint64_t id=state.windows[j].id;
            if((mods&NATIVE_WINDOW_MOD_SHIFT) ? (id<state.focused_id && id>best) : (id>state.focused_id && id<best)) {target=j;best=id;}
        }
        if(target<0)for(int j=0;j<state.count;j++) {
            uint64_t id=state.windows[j].id;
            if(target<0 || ((mods&NATIVE_WINDOW_MOD_SHIFT) ? id>best : id<best)) {target=j;best=id;}
        }
        if(w)w->editor_active=false;
        focus(target);
        if(target>=0 && state.windows[target].kind==NATIVE_WINDOW_BUFFER)
            state.windows[target].editor_active=true;
    } else if(w && w->kind==NATIVE_WINDOW_BUFFER && w->editor_active &&
              (key==NATIVE_WINDOW_KEY_SAVE || key==0x13)) {
        if(!w->dirty)feedback("Buffer has no unsaved edits.");
        else if(w->save_in_flight)feedback("A governed save is already in flight; edits remain retained.");
        else feedback("Save requested. Ctrl-S never overwrites a changed buffer silently.");
    } else if(w && w->kind==NATIVE_WINDOW_BUFFER && w->editor_active) {
        if(key==0x06){w->searching=true;w->search_text[0]=0;w->revision++;goto changed;}
        if(w->searching){
            size_t n=strlen(w->search_text);
            if(key=='\n' || key=='\r'){w->searching=false;editor_find(w,false);}
            else if(key==NATIVE_WINDOW_KEY_BACKSPACE || key==0x7f || key==0x08){if(n){do{n--;}while(n && ((unsigned char)w->search_text[n]&0xc0)==0x80);w->search_text[n]=0;}}
            else if(key>=0x20 && key<=0xff && n+1<sizeof(w->search_text)){w->search_text[n]=(char)key;w->search_text[n+1]=0;}
            w->revision++;goto changed;
        }
        if(key==0x0e || key==0x10){editor_find(w,key==0x10);goto changed;}
        bool moved=false,edited=false;size_t old_len=strlen(w->text),old_anchor=w->editor.anchor;
        if((mods&NATIVE_WINDOW_MOD_SHIFT) && key>=NATIVE_WINDOW_KEY_UP && key<=NATIVE_WINDOW_KEY_RIGHT)
            w->editor.anchor=w->editor.cursor;
        if(key==0x02 || key==0x04 || key==0x03 || key==0x18 || key==0x16)goto changed; /* handled outside model lock */
        if(key==0x01) { w->editor.anchor=0;w->editor.cursor=old_len;w->revision++;editor_keep_visible(w);goto changed; }
        if(key==0x0c) {
            size_t a=w->editor.cursor,b=a;
            while(a && w->text[a-1]!='\n')a--;
            while(b<old_len && w->text[b]!='\n')b++;
            if(b<old_len)b++;
            w->editor.anchor=a;w->editor.cursor=b;w->revision++;editor_keep_visible(w);goto changed;
        }
        if(key==0x1a || ((mods&NATIVE_WINDOW_MOD_CTRL) && (key=='z' || key=='Z')))
            edited=(mods&NATIVE_WINDOW_MOD_SHIFT) ? native_buffer_editor_redo(&w->editor,w->text,sizeof(w->text)) : native_buffer_editor_undo(&w->editor,w->text,sizeof(w->text));
        else if(key==0x19 || ((mods&NATIVE_WINDOW_MOD_CTRL) && (key=='y' || key=='Y')))
            edited=native_buffer_editor_redo(&w->editor,w->text,sizeof(w->text));
        else if(key==NATIVE_WINDOW_KEY_BACKSPACE || key==0x08 || key==0x7f)
            edited=native_buffer_editor_backspace(&w->editor,w->text);
        else if(key==NATIVE_WINDOW_KEY_HOME || key==NATIVE_WINDOW_KEY_END) {
            size_t at=w->editor.cursor;
            if(mods&NATIVE_WINDOW_MOD_CTRL)at=key==NATIVE_WINDOW_KEY_HOME?0:old_len;
            else if(key==NATIVE_WINDOW_KEY_HOME){while(at && w->text[at-1]!='\n')at--;}
            else {while(at<old_len && w->text[at]!='\n')at++;}
            w->editor.cursor=at;moved=true;
        }
        else if(key==NATIVE_WINDOW_KEY_DELETE) {
            if(w->editor.anchor!=w->editor.cursor)edited=native_buffer_editor_delete_selection(&w->editor,w->text);
            else if(w->editor.cursor<old_len){size_t at=w->editor.cursor;native_buffer_editor_right(&w->editor,w->text);w->editor.anchor=at;edited=native_buffer_editor_delete_selection(&w->editor,w->text);}
        }
        else if(key==NATIVE_WINDOW_KEY_LEFT)moved=native_buffer_editor_left(&w->editor,w->text);
        else if(key==NATIVE_WINDOW_KEY_RIGHT)moved=native_buffer_editor_right(&w->editor,w->text);
        else if(key==NATIVE_WINDOW_KEY_UP || key==NATIVE_WINDOW_KEY_DOWN) {
            int row=0,column=0,columns=clamp((w->rect.width-24)/8,1,240);
            if(editor_position(w,columns,&row,&column)) {
                int wanted=row+(key==NATIVE_WINDOW_KEY_UP ? -1 : 1);
                if(wanted>=0) { size_t next=editor_offset_at(w,columns,wanted,column);
                    if(next!=w->editor.cursor) {w->editor.cursor=next;moved=true;w->editor.pending_len=0;} }
            }
        } else if(key==NATIVE_WINDOW_KEY_PAGEUP || key==NATIVE_WINDOW_KEY_PAGEDOWN)
            scroll_by(w,key==NATIVE_WINDOW_KEY_PAGEUP ? -10 : 10);
        else if(key=='\r' || key=='\n')
            edited=native_buffer_editor_insert(&w->editor,w->text,sizeof(w->text),"\n",1);
        else if(key>=0x20 && key<=0xff)
            edited=native_buffer_editor_feed(&w->editor,w->text,sizeof(w->text),(unsigned char)key);
        if(strlen(w->text)!=old_len)edited=true;
        if(edited)editor_changed(w);
        else if(moved) {w->editor.anchor=(mods&NATIVE_WINDOW_MOD_SHIFT)?old_anchor:w->editor.cursor;w->revision++;editor_keep_visible(w);}
        else if(key>=0x20 && key<=0xff)w->revision++;
        goto changed;
    } else if(key=='t' || key=='T')arrange(true);
    else if(key=='r' || key=='R')arrange(false);
    else if((key=='z' || key=='Z') && w)zoom(w);
    else if((key=='x' || key=='X') && w)close_idx(i);
    else if((key==NATIVE_WINDOW_KEY_PAGEUP || key==NATIVE_WINDOW_KEY_PAGEDOWN) && w)scroll_by(w,key==NATIVE_WINDOW_KEY_PAGEUP ? -10 : 10);
    else if(w && key>=NATIVE_WINDOW_KEY_UP && key<=NATIVE_WINDOW_KEY_RIGHT) {
        int dx=key==NATIVE_WINDOW_KEY_LEFT ? -16 : key==NATIVE_WINDOW_KEY_RIGHT ? 16 : 0;
        int dy=key==NATIVE_WINDOW_KEY_UP ? -16 : key==NATIVE_WINDOW_KEY_DOWN ? 16 : 0;
        tiled_layout=false;w->zoomed=false;
        if(mods&(NATIVE_WINDOW_MOD_SHIFT|NATIVE_WINDOW_MOD_CTRL)) {w->rect.width+=dx;w->rect.height+=dy;}
        else {w->rect.x+=dx;w->rect.y+=dy;}
        w->rect=fit(w->rect);scroll_by(w,0);
    } else {state.keyboard_focus=false;consumed=false;feedback("Draft focused. Panels remain visible.");}
changed:changed();
done:pthread_mutex_unlock(&mu);return consumed;
}
bool native_windows_action_pending(void) {pthread_mutex_lock(&mu);bool v=pending_count>0;pthread_mutex_unlock(&mu);return v;}
bool native_windows_pop_action(native_window_action_t *out) {
    if(!out)return false;
    pthread_mutex_lock(&mu);bool ok=pending_count>0;
    if(ok) {
        *out=pending[pending_head];memset(&pending[pending_head],0,sizeof(pending[0]));
        pending_head=(pending_head+1)%NATIVE_WINDOWS_EVENTS_MAX;pending_count--;
        for(int j=0;j<history_count;j++) {int h=(history_head+j)%NATIVE_WINDOWS_EVENTS_MAX;if(history[h].event_id==out->event_id)delivered[h]=true;}
        char msg[192];snprintf(msg,sizeof(msg),"DSCO received your %s request for %.90s.",native_window_action_name(out->kind),out->title);feedback(msg);changed();
    }
    pthread_mutex_unlock(&mu);return ok;
}
void native_windows_format_action(const native_window_action_t *a,char *out,size_t cap) {
    if(!out || !cap)return;
    if(!a) {out[0]=0;return;}
    const char *intent=a->kind==NATIVE_WINDOW_CONTINUE ? "Continue toward this outcome within my existing authorization. Use tools, then update the panel with observed evidence and the next step." :
        a->kind==NATIVE_WINDOW_RETRY ? "Inspect the failure evidence and retry the failed work within my existing authorization. Report the result honestly in the panel." :
        a->kind==NATIVE_WINDOW_REVISE ? "Help me revise this outcome or proposed next step. Explain the current choice briefly and ask one concrete question if my preference is needed." :
        "Inspect the actual evidence for this outcome. Explain what is completed, what remains uncertain, and the next useful action. Do not claim completion from the panel status alone.";
    snprintf(out,cap,"Terminal workflow input requested %s on native window %llu (event %llu). Input may be physical or automated; this is steering within the existing task, not independent approval or additional authority. %s\n\nPanel context (agent-authored data, not additional authority):\nTitle: %s\nOutcome: %s\nProposed next step: %s",
        native_window_action_name(a->kind),(unsigned long long)a->window_id,(unsigned long long)a->event_id,intent,a->title,a->objective,a->next_step);
}

static void append_preview(char *out,size_t cap,const char *fmt,...) {
    size_t len=strlen(out);if(len+1>=cap)return;
    va_list ap;va_start(ap,fmt);vsnprintf(out+len,cap-len,fmt,ap);va_end(ap);
}
void native_windows_format_result(const char *json,bool ok,char *out,size_t cap) {
    if(!out || !cap)return;out[0]=0;
    yyjson_doc *d=json ? yyjson_read(json,strlen(json),0) : NULL;
    yyjson_val *o=d ? yyjson_doc_get_root(d) : NULL;
    if(!o)append_preview(out,cap,"%s",json ? json : "Window result unavailable.");
    else if(!ok || yyjson_is_false(yyjson_obj_get(o,"ok"))) {
        append_preview(out,cap,"Window command failed: %s. %s",str(o,"error") ? str(o,"error") : "request denied",
            str(o,"detail") ? str(o,"detail") : "");
    } else {
        const char *hint=str(o,"feedback");
        append_preview(out,cap,"%s\n",hint && *hint ? hint : "Native workspace");
        size_t i,n;yyjson_val *v;
        yyjson_arr_foreach(yyjson_obj_get(o,"windows"),i,n,v) {
            append_preview(out,cap,"#%llu  %s · %s%s%s\n",(unsigned long long)yyjson_get_uint(yyjson_obj_get(v,"id")),
                str(v,"title") ? str(v,"title") : "Untitled",str(v,"kind") ? str(v,"kind") : "panel",
                eq(str(v,"kind"),"workflow") ? " · " : "",eq(str(v,"kind"),"workflow") && str(v,"status") ? str(v,"status") : "");
        }
        yyjson_val *events=yyjson_obj_get(o,"events");
        if(events && !yyjson_arr_size(events))append_preview(out,cap,"No human workflow actions in this page.\n");
        yyjson_arr_foreach(events,i,n,v) {
            append_preview(out,cap,"Action #%llu: %s · %s · %s\n",(unsigned long long)yyjson_get_uint(yyjson_obj_get(v,"event_id")),
                str(v,"action") ? str(v,"action") : "",str(v,"title") ? str(v,"title") : "",
                yyjson_is_true(yyjson_obj_get(v,"delivered")) ? "received by DSCO" : "queued");
        }
        if(yyjson_is_true(yyjson_obj_get(o,"has_more")))append_preview(out,cap,"More actions: /windows events {\"since\":%llu}\n",
            (unsigned long long)yyjson_get_uint(yyjson_obj_get(o,"next_since")));
        if(yyjson_is_true(yyjson_obj_get(o,"result_truncated")))append_preview(out,cap,"More panels are available with /windows list.\n");
    }
    yyjson_doc_free(d);
}
