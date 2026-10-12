#include "presentation_blocks.h"
#include "presentation_json.h"
#include "rich_text.h"
#include "transcript_view.h"
#include "tui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PRESENTATION_STUB_PAGER
void tui_pager_init(tui_pager_t*p,const char**l,int n,const char*t){(void)p;(void)l;(void)n;(void)t;}
void tui_pager_run(tui_pager_t*p){(void)p;}
#endif
static rich_token_t tokens[256];
static void flattened(const char *source, char *out, size_t cap) {
    size_t n=rich_text_parse(source,tokens,256), used=0;out[0]=0;
    for(size_t i=0;i<n;i++) if(tokens[i].type==RICH_TOKEN_TEXT) {
        size_t len=strlen(tokens[i].text);assert(used+len<cap);
        memcpy(out+used,tokens[i].text,len+1);used+=len;
    }
}
int main(void) {
    char text[4096];
    flattened("Run `a | b` or x | y.",text,sizeof(text));assert(!strcmp(text,"Run a | b or x | y."));
    const char *tables[]={"| Name | State |\n| --- | --- |\n| Alpha | Ready |\n",
                         "+------+-------+\n| Name | State |\n+------+-------+\n| Alpha | Ready |\n+------+-------+\n"};
    for(size_t i=0;i<2;i++) {
        flattened(tables[i],text,sizeof(text));
        assert(strstr(text,"Name: Alpha")&&strstr(text,"State: Ready"));assert(!strchr(text,'|')&&!strstr(text,"---"));
        assert(tokens[0].style==RICH_STYLE_TABLE);
    }
    flattened("| Name | State |\n| --- | --- |",text,sizeof(text));assert(strstr(text,"Name")&&strstr(text,"State"));
    presentation_table_t table={0};assert(!presentation_table_begin("a | b","- | -",&table));
    char cells[PRESENTATION_COLUMNS][PRESENTATION_CELL];
    assert(presentation_table_cells("| `a|b` | c\\|d |",cells)==2);assert(!strcmp(cells[0],"`a|b`"));
    assert(presentation_table_cells("| `unterminated | cell |",cells)==0);
    rich_text_parse("````c\n   x();\n```\n~~~\n````",tokens,256);
    assert(!strcmp(tokens[2].text,"   x();"));assert(!strcmp(tokens[4].text,"```"));assert(!strcmp(tokens[6].text,"~~~"));
    rich_text_parse("```diff\n@@ -1 +1 @@\n-old\n+new\n```",tokens,256);
    assert(tokens[2].style==RICH_STYLE_DIFF_HUNK&&tokens[4].style==RICH_STYLE_DIFF_REMOVE&&tokens[6].style==RICH_STYLE_DIFF_ADD);
    assert(presentation_json("not JSON",true)==NULL);
    const char *raw="{\"n\":123456789012345678901234567890,\"items\":[1,2],\"s\":\"a,b:{x}\"}";
    char *pretty=presentation_json(raw,true);assert(pretty&&strchr(pretty,'\n')&&strstr(pretty,"123456789012345678901234567890")&&strstr(pretty,"a,b:{x}"));free(pretty);
    pretty=presentation_json("{\"error\":\"denied\\u001b[2J\"}",false);assert(pretty&&!strchr(pretty,27)&&strstr(pretty,"denied"));free(pretty);
    pretty=presentation_json("{\"message\":\"ok\\u0000hidden\"}",false);assert(pretty&&strstr(pretty,"hidden"));free(pretty);
    pretty=presentation_json("{\"result\":\"{\\\"nested\\\":[1,2]}\"}",true);
    assert(pretty&&strstr(pretty,"\"nested\": [")&&!strstr(pretty,"\\\\n"));free(pretty);
    pretty=presentation_json("{\"output\":\"line1\\n  line2\"}",true);assert(pretty&&!strcmp(pretty,"line1\n  line2"));free(pretty);
    const char *edit="{\"kind\":\"code_edit\",\"status\":\"written\",\"path\":\"α.c\",\"before_sha256\":\"old\",\"after_sha256\":\"new\",\"verified\":true,\"durable\":true,\"diff\":\"@@ -1 +1 @@\\n-old\\n+new\\n\"}";
    pretty=presentation_json(edit,false);assert(pretty&&strstr(pretty,"Edit written · α.c")&&!strstr(pretty,"@@"));free(pretty);
    pretty=presentation_json(edit,true);assert(pretty&&strstr(pretty,"old → new")&&strstr(pretty,"Readback: verified")&&strstr(pretty,"\n-old\n+new\n")&&!strstr(pretty,"\\n"));free(pretty);
    pretty=presentation_json("{\"kind\":\"skill\",\"action\":\"invoke\",\"name\":\"fixture\",\"path\":\"SKILL.md\",\"sha256\":\"abc123\",\"instructions\":\"# First\\nDo this.\"}",true);
    assert(pretty&&strstr(pretty,"Skill invoke · fixture · abc123")&&strstr(pretty,"Scope: this invocation")&&strstr(pretty,"# First\nDo this."));free(pretty);
    msg_content_t parts[]={{.type="text",.text="```c\n   x();\n```"},{.type="thinking",.text="DO_NOT_RENDER"},{.type="tool_result",.tool_name="fixture",.text=(char*)raw}};
    message_t message={.role=ROLE_ASSISTANT,.content=parts,.content_count=3};
    conversation_t conv={.msgs=&message,.count=1};
    char *review=transcript_view_snapshot(&conv);assert(review&&strstr(review,"   x();")&&strstr(review,"123456789012345678901234567890")&&!strstr(review,"DO_NOT_RENDER"));free(review);
    assert(parts[2].text==raw);
    puts("PASS: table cards, literal pipes, fences, indentation, diff semantics, JSON lexemes/control safety, transcript projection");
}
