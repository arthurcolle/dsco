/* Focused headless contract for native bound-buffer editing. The governed
 * adapter is stubbed at tools_execute_for_tier; it observes the model while
 * servicing a write to prove the retained-window mutex is not held. */
#include "native_windows.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static char result[65536];
static bool active=true;
static int write_mode; /* 0 success, 1 conflict */
static int governed_calls;
static char fake_clip[8192];
static bool clip_denied,clip_corrupt,clip_race;
static char last_request_id[96];
static const char *const BUFFER_ID="11111111-1111-4111-8111-111111111111";
static char revision[65];

static void check(bool ok,const char *message) {
    checks++;
    if(!ok) { fprintf(stderr,"FAIL %u: %s\n%s\n",checks,message,result); exit(1); }
}
static native_windows_snapshot_t snapshot(void) {
    native_windows_snapshot_t s; memset(&s,0,sizeof(s)); native_windows_snapshot(&s); return s;
}
static native_window_t pane(void) {
    native_windows_snapshot_t s=snapshot();
    check(s.count==1,"one bound buffer panel exists");
    return s.windows[0];
}
static void reset_fixture(const char *text) {
    native_windows_reset();
    native_windows_set_work_area((native_ui_rect_t){0,0,900,650});
    memset(revision,'a',64);revision[64]='\0';
    write_mode=0;governed_calls=0;active=true;
    check(native_windows_bind_buffer(0,"Editable",text,BUFFER_ID,revision,"fixture",false,result,sizeof(result)),
          "buffer snapshot binds through retained model");
}
static void click_body(void) {
    native_window_t w=pane();
    int x=w.rect.x+480,y=w.rect.y+NATIVE_WINDOW_TITLE_HEIGHT+38;
    check(native_windows_pointer(0,x,y,false),"buffer body press is consumed");
    check(native_windows_pointer(0,x,y,true),"buffer body release is consumed");
    w=pane();check(w.editor_active && native_windows_focused(),"body click focuses the buffer editor");
}
static void type_ascii(const char *text) {
    for(const unsigned char *p=(const unsigned char *)text;*p;p++)
        check(native_windows_key(*p,0),"focused buffer consumes ordinary byte");
}
static bool field_equals(yyjson_val *root,const char *key,const char *value) {
    const char *got=yyjson_get_str(yyjson_obj_get(root,key));
    return got && value && !strcmp(got,value);
}

bool pixel_tui_session_active(void) { return active; }
void pixel_tui_session_windows_changed(FILE *out) { (void)out; }
const char *tools_execution_tier(void) { return "trusted"; }

bool tools_execute_for_tier(const char *name,const char *input,const char *tier,char *out,size_t cap) {
    if(name && !strcmp(name,"clipboard")) {
        check(tier && !strcmp(tier,"trusted"),"clipboard retains tier");
        native_windows_snapshot_t observed=snapshot();check(observed.count==1,"clipboard runs outside model lock");
        if(clip_denied){snprintf(out,cap,"{\"ok\":false}");return false;}
        yyjson_doc *d=yyjson_read(input,strlen(input),0);yyjson_val *r=yyjson_doc_get_root(d);
        if(field_equals(r,"action","write")){snprintf(fake_clip,sizeof(fake_clip),"%s",yyjson_get_str(yyjson_obj_get(r,"text")));snprintf(out,cap,"{\"ok\":true}");}
        else {yyjson_mut_doc *rd=yyjson_mut_doc_new(NULL);yyjson_mut_val *o=yyjson_mut_obj(rd);yyjson_mut_doc_set_root(rd,o);
            yyjson_mut_obj_add_bool(rd,o,"ok",true);yyjson_mut_obj_add_str(rd,o,"text",clip_corrupt?"wrong":fake_clip);
            char *j=yyjson_mut_write(rd,0,NULL);snprintf(out,cap,"%s",j);free(j);yyjson_mut_doc_free(rd);
            if(clip_race)native_windows_key(NATIVE_WINDOW_KEY_LEFT,0);
        }
        yyjson_doc_free(d);return true;
    }
    governed_calls++;
    check(name && !strcmp(name,"buffer"),"save uses the governed buffer tool name");
    check(tier && !strcmp(tier,"trusted"),"save preserves the caller tier");
    native_windows_snapshot_t observed=snapshot();
    check(observed.count==1,"governed save callback can observe model outside its mutex");
    yyjson_doc *doc=yyjson_read(input,strlen(input),0);
    yyjson_val *root=doc ? yyjson_doc_get_root(doc) : NULL;
    check(root && yyjson_is_obj(root) && field_equals(root,"action","write"),"save emits a write action");
    check(field_equals(root,"buffer_id",BUFFER_ID),"save targets the bound buffer identity");
    check(field_equals(root,"expected_revision",revision),"save uses the retained base revision as its conflict guard");
    const char *content=yyjson_get_str(yyjson_obj_get(root,"content"));
    check(content && !strcmp(content,observed.windows[0].text),"save payload is an exact bounded snapshot of current text");
    const char *rid=yyjson_get_str(yyjson_obj_get(root,"request_id"));
    check(rid && strlen(rid)==78 && !strncmp(rid,"native-editor-",14),"save retry identity is content-addressed, not resettable window sequence");
    snprintf(last_request_id,sizeof(last_request_id),"%s",rid);
    yyjson_doc_free(doc);
    if(write_mode==1) {
        snprintf(out,cap,"{\"ok\":false,\"error\":\"revision_conflict\"}");
        return false;
    }
    memset(revision,'b',64);revision[64]='\0';
    snprintf(out,cap,"{\"ok\":true,\"buffer\":{\"buffer_id\":\"%s\",\"revision\":\"%s\"}}",BUFFER_ID,revision);
    return true;
}

static void test_routing_and_utf8_editing(void) {
    reset_fixture("base"); click_body();
    type_ascii("trzx");
    native_window_t w=pane();
    check(!strcmp(w.text,"basetrzx"),"t/r/z/x insert into a bound buffer instead of arranging or closing panels");
    check(w.dirty && w.editor.cursor==strlen(w.text),"ordinary insertion marks dirty and advances a byte cursor");

    check(native_windows_key(NATIVE_WINDOW_KEY_LEFT,0),"left arrow is consumed by buffer editor");
    check(native_windows_key(NATIVE_WINDOW_KEY_LEFT,0),"second left arrow is consumed by buffer editor");
    check(native_windows_key(NATIVE_WINDOW_KEY_BACKSPACE,0),"backspace is consumed by buffer editor");
    w=pane();check(!strcmp(w.text,"basetzx"),"backspace removes one complete UTF-8-safe codepoint");
    check(native_windows_key('\n',0),"Enter inserts a newline in a bound buffer");
    type_ascii("雪"); /* UTF-8 bytes are routed one at a time. */
    w=pane();check(strstr(w.text,"\n雪")!=NULL,"multibyte text and newline stay in the bound buffer");
    check(native_windows_key(NATIVE_WINDOW_KEY_RIGHT,0),"right arrow is consumed without splitting UTF-8");

    /* Simulated bracketed paste payload: no byte is offered to layout keys or
     * the composer; newline and every ordinary byte remain buffer data. */
    type_ascii("paste t/r/z/x\nline");
    w=pane();check(strstr(w.text,"paste t/r/z/x\nline")!=NULL,"paste payload remains buffer data");
    check(w.dirty && w.editor.cursor<=strlen(w.text),"paste leaves a bounded dirty editor state");
}

static void test_click_caret_scroll_and_capacity(void) {
    char long_text[NATIVE_WINDOW_TEXT_CAP]; long_text[0]='\0';
    for(int i=0;i<240;i++) { char line[32]; snprintf(line,sizeof(line),"line-%03d\n",i); strncat(long_text,line,sizeof(long_text)-strlen(long_text)-1); }
    reset_fixture(long_text); click_body();
    for(int i=0;i<240;i++) check(native_windows_key(NATIVE_WINDOW_KEY_DOWN,0),"move caret through long content");
    native_window_t before=pane();
    type_ascii("x");
    native_window_t after=pane();
    check(after.scroll>0,"editor keeps the caret visible by scrolling long content");
    check(after.rect.x==before.rect.x && after.rect.y==before.rect.y,"editing does not move the native panel");

    char full[NATIVE_WINDOW_TEXT_CAP]; memset(full,'q',sizeof(full)-1);full[sizeof(full)-1]='\0';
    reset_fixture(full);click_body();type_ascii("t");
    native_window_t bounded=pane();
    check(strlen(bounded.text)==NATIVE_WINDOW_TEXT_CAP-1,"editor refuses insertion beyond the existing 8191-byte snapshot bound");
    check(!bounded.dirty,"rejected over-capacity input does not create a false dirty mutation");
}

static void test_save_conflict_and_preservation(void) {
    reset_fixture("draft");click_body();type_ascii(" edit");
    check(native_windows_key(NATIVE_WINDOW_KEY_SAVE,0),"Ctrl-S request is consumed by focused buffer");
    bool attempted=false;check(native_windows_save_pending(&attempted,result,sizeof(result)) && attempted,"explicit save calls the governed adapter");
    native_window_t saved=pane();
    check(!saved.dirty && !strcmp(saved.buffer_revision,revision),"successful save clears dirty state only after the governed reply");
    uint64_t id=saved.id;

    type_ascii(" conflict");
    write_mode=1;
    check(native_windows_key(NATIVE_WINDOW_KEY_SAVE,0),"conflicting save request remains a consumed explicit action");
    attempted=false;check(!native_windows_save_pending(&attempted,result,sizeof(result)) && attempted,"revision conflict is reported as a failed governed save");
    native_window_t conflicted=pane();
    check(conflicted.dirty && strstr(conflicted.text,"conflict")!=NULL,"conflict preserves all local dirty text");

    char command[256];
    snprintf(command,sizeof(command),"{\"action\":\"close\",\"id\":%" PRIu64 "}",id);
    check(!native_windows_command(command,result,sizeof(result)),"dirty close is rejected instead of discarding text");
    check(pane().dirty && snapshot().count==1,"rejected close retains the dirty panel");
    check(!native_windows_bind_buffer(id,"Refresh attempt","server text",BUFFER_ID,revision,"fixture",false,result,sizeof(result)),
          "refresh cannot replace dirty text silently");
    check(strstr(pane().text,"conflict")!=NULL,"rejected refresh preserves local text");

    write_mode=0;
    check(native_windows_key(NATIVE_WINDOW_KEY_SAVE,0),"second explicit save is accepted");
    attempted=false;check(native_windows_save_pending(&attempted,result,sizeof(result)) && attempted,"dirty text can be saved after a conflict");
    check(!pane().dirty,"successful retry clears dirty state");
    check(native_windows_command(command,result,sizeof(result)),"clean panel may be explicitly closed");
    check(snapshot().count==0,"close after save removes only the retained view");
    check(governed_calls==3,"all writes use the governed adapter and no direct store bypass");
}

static void test_pinned_paste_and_identity(void) {
    reset_fixture("");click_body();uint64_t id=native_windows_paste_target();
    check(id!=0,"paste captures editable identity");
    check(native_windows_key(NATIVE_WINDOW_KEY_ESCAPE,0),"focus may leave during paste");
    check(native_windows_paste_target()==0,"chat focus is not an editor paste target");
    check(native_windows_insert_paste(id,"t\tr\nx_z",7),"pinned paste treats tab and newline as data");
    check(!strcmp(pane().text,"t\tr\nx_z"),"paste lands only in original buffer");
    native_window_t before=pane();
    check(!native_windows_insert_paste(id+100,"wrong",5),"missing original paste target rejected");
    check(!strcmp(pane().text,before.text),"rejected paste leaves document untouched");
    char first[96];
    reset_fixture("");click_body();type_ascii("first");
    native_windows_key(NATIVE_WINDOW_KEY_SAVE,0);bool attempted=false;
    check(native_windows_save_pending(&attempted,result,sizeof(result)),"first content saved");
    snprintf(first,sizeof(first),"%s",last_request_id);
    reset_fixture("");click_body();type_ascii("other");
    native_windows_key(NATIVE_WINDOW_KEY_SAVE,0);
    check(native_windows_save_pending(&attempted,result,sizeof(result)),"different content after model reset saved");
    check(strcmp(first,last_request_id)!=0,"reset does not reuse request ID for different content");
}

static void test_history(void) {
    reset_fixture(""); click_body();
    type_ascii("ab");
    check(native_windows_key(0x1a,0),"undo consumed");
    check(!strcmp(pane().text,"a"),"undo restores prior text");
    check(native_windows_key(0x19,0),"redo consumed");
    check(!strcmp(pane().text,"ab"),"redo restores text");
    uint64_t id=pane().id;
    check(native_windows_insert_paste(id,"\nhello 🌍",strlen("\nhello 🌍")),"unicode paste");
    native_windows_key(0x1a,0);
    check(!strcmp(pane().text,"ab"),"paste undo is atomic");
    native_windows_key('z',NATIVE_WINDOW_MOD_CTRL|NATIVE_WINDOW_MOD_SHIFT);
    check(!strcmp(pane().text,"ab\nhello 🌍"),"shift ctrl z restores paste");
    native_windows_key(0x1a,0); type_ascii("c"); native_windows_key(0x19,0);
    check(!strcmp(pane().text,"abc"),"new edit invalidates redo");
    native_windows_reset();
}
static void test_selection_ui(void) {
    reset_fixture("");click_body();type_ascii("abc");
    native_windows_key(NATIVE_WINDOW_KEY_LEFT,NATIVE_WINDOW_MOD_SHIFT);
    check(pane().editor.anchor==3 && pane().editor.cursor==2,"shift left selects last byte");
    native_windows_key(NATIVE_WINDOW_KEY_LEFT,NATIVE_WINDOW_MOD_SHIFT);
    check(pane().editor.anchor==3 && pane().editor.cursor==1,"shift extends existing selection");
    type_ascii("XY");check(!strcmp(pane().text,"aXY"),"selection replacement");
    check(pane().dirty,"equal length replacement is dirty");
    native_windows_key(0x01,0);check(pane().editor.anchor==0 && pane().editor.cursor==3,"select all");
    native_windows_key(NATIVE_WINDOW_KEY_ESCAPE,0);check(native_windows_focused(),"escape selection keeps editing");
    check(pane().editor.anchor==pane().editor.cursor,"escape clears selection");
    native_windows_key(NATIVE_WINDOW_KEY_HOME,NATIVE_WINDOW_MOD_CTRL);
    check(pane().editor.cursor==0,"document home");
    native_windows_key(NATIVE_WINDOW_KEY_DELETE,0);check(!strcmp(pane().text,"XY"),"forward delete");
    native_windows_key(0x01,0);native_windows_key(NATIVE_WINDOW_KEY_BACKSPACE,0);
    check(!strcmp(pane().text,""),"delete selection");
    native_windows_key(0x1a,0);check(!strcmp(pane().text,"XY"),"undo delete selection");
    native_windows_key(0x19,0);check(!strcmp(pane().text,""),"redo delete selection");
    native_windows_reset();
}
static void test_clipboard_search(void) {
    reset_fixture("");click_body();type_ascii("alpha beta alpha");
    native_windows_key(0x06,0);type_ascii("beta");native_windows_key('\n',0);
    check(pane().editor.anchor==6 && pane().editor.cursor==10,"find selects match");
    check(native_windows_clipboard_key(0x03) && !strcmp(fake_clip,"beta"),"copy exact selection");
    clip_denied=true;check(!native_windows_clipboard_key(0x18),"denied cut rejected");clip_denied=false;
    check(!strcmp(pane().text,"alpha beta alpha"),"denied cut keeps content");
    clip_corrupt=true;check(!native_windows_clipboard_key(0x18),"mismatched clipboard prevents cut");clip_corrupt=false;
    check(native_windows_clipboard_key(0x18),"verified cut");check(!strcmp(pane().text,"alpha  alpha"),"cut deleted only selection");
    check(native_windows_clipboard_key(0x16),"paste");check(!strcmp(pane().text,"alpha beta alpha"),"paste exact bytes");
    native_windows_key(0x01,0);clip_race=true;check(!native_windows_clipboard_key(0x16),"selection race rejected");clip_race=false;
    check(!strcmp(pane().text,"alpha beta alpha"),"raced paste preserves text");
    native_windows_key(NATIVE_WINDOW_KEY_HOME,NATIVE_WINDOW_MOD_CTRL);native_windows_key(0x06,0);type_ascii("alpha");native_windows_key('\n',0);
    native_windows_key(0x0e,0);check(pane().editor.anchor==11,"next match");
    native_windows_key(0x10,0);check(pane().editor.anchor==0,"previous match");
    native_windows_reset();
}
int main(void) {
    test_clipboard_search();
    test_selection_ui();
    test_history();
    test_routing_and_utf8_editing();
    test_click_caret_scroll_and_capacity();
    test_save_conflict_and_preservation();
    test_pinned_paste_and_identity();
    printf("native_writing_editor: %u checks passed\n",checks);
    return 0;
}
