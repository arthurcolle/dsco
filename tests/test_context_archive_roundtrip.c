#include "context_archive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    conversation_t c;conv_init(&c);conv_add_user_text(&c,"preserved objective");
    conv_add_assistant_text(&c,"UTF8 citation: 雪 🦉; empty and null remain distinct");
    msg_content_t *b=&c.msgs[1].content[0];
    b->image_media_type=strdup("image/png");b->image_data=strdup("IMAGE_DATA");b->image_url=strdup("");
    b->video_media_type=strdup("video/mp4");b->video_data=strdup("VIDEO_DATA");b->video_url=strdup("video-local");
    b->doc_media_type=strdup("text/plain");b->doc_data=strdup("DOC_DATA");b->doc_title=strdup("DOC_TITLE");
    b->cited_text=strdup("CITED_TEXT");b->cite_start=7;b->cite_end=42;b->is_error=true;
    conv_add_assistant_text(&c,"{\"encrypted_content\":\"OPAQUE_EXACT\"}");
    free(c.msgs[2].content[0].type);c.msgs[2].content[0].type=strdup("openai_codex_reasoning:fixture-model");
    conv_add_user_text(&c,"latest instruction");
    session_state_t session={.non_conv_overhead_tokens=1234,.last_input_tokens=5678};
    char path[]="/tmp/dsco-context-roundtrip-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    char *before=context_archive_serialize(c.msgs,c.count);assert(before);
    assert(conv_save_ex(&c,&session,path));
    conversation_t loaded;conv_init(&loaded);session_state_t restored={0};
    assert(conv_load_ex(&loaded,&restored,path));unlink(path);
    char *after=context_archive_serialize(loaded.msgs,loaded.count);assert(after);
    assert(!strcmp(before,after));
    assert(restored.non_conv_overhead_tokens==1234 && restored.last_input_tokens==5678);
    free(before);free(after);conv_free(&c);conv_free(&loaded);
    /* Real historical sessions exceed the old arbitrary 10 MiB load ceiling. */
    conv_init(&c);conv_init(&loaded);
    conv_add_user_text(&c,"large-history objective");
    size_t n=11*1024*1024;
    char *large=malloc(n+1);assert(large);memset(large,'x',n);large[n]=0;
    conv_add_assistant_text(&c,large);free(large);
    char large_path[]="/tmp/dsco-large-history-XXXXXX";
    fd=mkstemp(large_path);assert(fd>=0);close(fd);
    assert(conv_save_ex(&c,NULL,large_path));
    assert(conv_load_ex(&loaded,NULL,large_path));unlink(large_path);
    assert(loaded.count==2 && strlen(loaded.msgs[1].content[0].text)==n);
    assert(!strcmp(c.msgs[1].content[0].text,loaded.msgs[1].content[0].text));
    conv_free(&c);conv_free(&loaded);
    puts("PASS full conversation save/reload: all block fields, UTF8, opaque replay state, null/empty distinction, estimate versus historical usage");
}
