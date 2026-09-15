#include "buffer_textedit.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int calls, launches;
static int mode;
static const char *path = "/tmp/owned 雪 'draft;$(id).txt";
const char *tools_execution_tier(void) { return "trusted"; }
bool tools_execute_for_tier(const char *tool, const char *input, const char *tier, char *out, size_t cap) {
    calls++; assert(!strcmp(tier, "trusted"));
    yyjson_doc *d = yyjson_read(input, strlen(input), 0); assert(d);
    yyjson_val *o = yyjson_doc_get_root(d);
    if (!strcmp(tool, "buffer")) {
        assert(yyjson_equals_str(yyjson_obj_get(o,"action"),"read"));
        assert(yyjson_get_uint(yyjson_obj_get(o,"max_bytes")) == 1);
        if (mode == 1) { snprintf(out, cap, "secrets denied"); yyjson_doc_free(d); return false; }
        yyjson_mut_doc *r = yyjson_mut_doc_new(NULL); yyjson_mut_val *root = yyjson_mut_obj(r), *b = yyjson_mut_obj(r);
        yyjson_mut_doc_set_root(r,root); yyjson_mut_obj_add_bool(r,root,"ok",true);
        yyjson_mut_obj_add_val(r,root,"buffer",b); yyjson_mut_obj_add_str(r,b,"content_path",path);
        yyjson_mut_obj_add_bool(r,b,"closed",mode == 2);
        char *s = yyjson_mut_write(r,0,NULL); snprintf(out,cap,"%s",s); free(s); yyjson_mut_doc_free(r);
    } else {
        assert(!strcmp(tool,"bash")); launches++;
        const char *cmd = yyjson_get_str(yyjson_obj_get(o,"command"));
        assert(strstr(cmd,"/usr/bin/osascript - '/tmp/owned 雪 '\\''draft;$(id).txt'"));
        assert(!strstr(cmd,"pbcopy") && !strstr(cmd,"set text"));
        assert(yyjson_get_uint(yyjson_obj_get(o,"timeout")) == 20);
        if (mode == 3) { snprintf(out,cap,"exec denied"); yyjson_doc_free(d); return false; }
        snprintf(out,cap,"DSCO_TEXTEDIT_OPENED\n%s\n",mode == 4 ? "/wrong/path" : path);
    }
    yyjson_doc_free(d); return true;
}
int main(void) {
    const char *input="{\"action\":\"open\",\"mode\":\"textedit\",\"name\":\"draft\"}";
    char out[32768];
#ifdef __APPLE__
    for (mode=0; mode<5; mode++) {
        calls=launches=0;
        assert(buffer_textedit_open(input,out,sizeof(out)) == (mode==0));
        assert(launches == (mode==1 || mode==2 ? 0 : 1));
        yyjson_doc *d=yyjson_read(out,strlen(out),0); assert(d); yyjson_doc_free(d);
    }
    const char *bad[]={"{\"action\":\"open\",\"mode\":\"textedit\"}","{\"action\":\"open\",\"name\":\"draft\",\"visible\":false}","{\"action\":\"close\",\"name\":\"draft\"}"};
    for(size_t i=0;i<3;i++){calls=0;assert(!buffer_textedit_open(bad[i],out,sizeof(out)));assert(calls==0);}
    mode=0;path="relative";launches=0;assert(!buffer_textedit_open(input,out,sizeof(out)));assert(!launches);
    path="/tmp/bad\npath";assert(!buffer_textedit_open(input,out,sizeof(out)));assert(!launches);
#else
    assert(!buffer_textedit_open(input,out,sizeof(out))); assert(calls==0);
#endif
    puts("TextEdit adapter: gated reads/launch, closed buffers, quoting, strict document verification, invalid options passed");
}
