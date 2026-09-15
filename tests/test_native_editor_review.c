/* Independent headless boundary review; coordinator-owned, not author tests. */
#include "native_buffer_editor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char text[16]="abcd";
    native_buffer_editor_t e = {0};
    native_buffer_editor_reset(&e,4);
    assert(!native_buffer_editor_insert(&e,text,4,"x",1));
    assert(!strcmp(text,"abcd") && e.cursor==4);
    text[0]=0; native_buffer_editor_reset(&e,0);
    assert(native_buffer_editor_feed(&e,text,sizeof(text),0xe2));
    native_buffer_editor_backspace(&e,text);
    assert(e.pending_len==0);
    assert(native_buffer_editor_feed(&e,text,sizeof(text),'a'));
    assert(!strcmp(text,"a"));
    assert(native_buffer_editor_insert(&e,text,sizeof(text),"雪🙂",7));
    assert(native_buffer_editor_left(&e,text) && e.cursor==4);
    assert(native_buffer_editor_backspace(&e,text) && !strcmp(text,"a🙂") && e.cursor==1);
    assert(native_buffer_editor_right(&e,text) && e.cursor==5);
    const char *invalid[]={"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\x80","\xe2\x82"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        assert(!native_buffer_editor_insert(&e,text,sizeof(text),invalid[i],strlen(invalid[i])));
        assert(!strcmp(text,"a🙂") && e.cursor==5);
    }
    char bounded[5]=""; native_buffer_editor_reset(&e,0);
    assert(native_buffer_editor_insert(&e,bounded,sizeof(bounded),"🙂",4));
    assert(!native_buffer_editor_insert(&e,bounded,sizeof(bounded),"x",1));
    assert(!strcmp(bounded,"🙂") && native_buffer_editor_cursor_valid(&e,bounded));
    puts("PASS: independent capacity, partial UTF-8 backspace, cursor, malformed Unicode, exact-full buffer checks");
    return 0;
}
