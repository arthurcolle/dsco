/* Headless regression: literal identifiers must remain a single token because
 * native wrapping separates semantic tokens. No font, UI, or terminal needed. */
#include "rich_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const char *cases[] = {
        "tool_response", "tui_composer_set_slash_commands", "KITTY_LISTEN_ON",
        "src/native_window_tool.c", "one_two three_four five_six",
        "_internal and __dunder__ names survive", "a_b_c", "trailing_", "a*b", 
        "read a_b then c_d then e_f end", "/tmp/dsco_resize_test/zoom_burst_009.png"
    };
    rich_token_t tokens[128];
    for (size_t c=0;c<sizeof(cases)/sizeof(*cases);c++) {
        size_t n=rich_text_parse(cases[c],tokens,128), text=0;
        for(size_t i=0;i<n;i++) if(tokens[i].type==RICH_TOKEN_TEXT) {
            text++;
            if(tokens[i].style!=RICH_STYLE_BODY || strcmp(tokens[i].text,cases[c])) {
                fprintf(stderr,"identifier split: %s => [%s] style=%d\n",cases[c],tokens[i].text,tokens[i].style);
                return 1;
            }
        }
        assert(text==1);
    }
    size_t n=rich_text_parse("call foo_bar and *see* baz_qux with _emphasis_",tokens,128);
    int emph=0, identifiers=0;
    for(size_t i=0;i<n;i++) if(tokens[i].type==RICH_TOKEN_TEXT) {
        emph+=tokens[i].style==RICH_STYLE_EMPHASIS;
        identifiers+=(strstr(tokens[i].text,"foo_bar")!=NULL)+(strstr(tokens[i].text,"baz_qux")!=NULL);
    }
    assert(emph==2 && identifiers==2);
    puts("PASS: 11 literal identifier runs remain intact; real emphasis preserved");
    return 0;
}
