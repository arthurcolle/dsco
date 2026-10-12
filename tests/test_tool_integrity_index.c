/* Differential validation against the preserved production scan fallback. */
#include <assert.h>
#include <time.h>
#include "../src/llm.c"
static unsigned state=1979;
static unsigned random_value(void) {state=state*1664525u+1013904223u;return state;}
int main(void) {
    for(int iteration=0;iteration<10000;iteration++) {
        conversation_t c={0};c.count=c.cap=(int)(random_value()%45);c.msgs=calloc((size_t)c.count,sizeof(message_t));
        for(int i=0;i<c.count;i++) {
            message_t *m=&c.msgs[i];m->role=random_value()%2?ROLE_USER:ROLE_ASSISTANT;
            m->content_count=(int)(random_value()%4);m->content=calloc((size_t)m->content_count,sizeof(msg_content_t));
            for(int j=0;j<m->content_count;j++) {
                msg_content_t *b=&m->content[j];unsigned k=random_value()%4;
                b->type=k==0?NULL:strdup(k==1?"text":k==2?"tool_use":"tool_result");
                unsigned id=random_value()%9;if(id) {char key[32];snprintf(key,sizeof(key),"id%u",id%7);b->tool_id=strdup(key);}
            }
        }
        for(int pending=0;pending<2;pending++) {
            tool_integrity_result_t original=conv_validate_tool_call_integrity_scan(&c,pending),indexed;
            assert(tool_integrity_index_validate(&c,pending,&indexed));
            assert(original.ok==indexed.ok && original.missing_tool_use==indexed.missing_tool_use &&
                original.missing_tool_result==indexed.missing_tool_result && original.duplicate_tool_use==indexed.duplicate_tool_use &&
                original.out_of_order_result==indexed.out_of_order_result && !strcmp(original.first_error,indexed.first_error));
        }
        bool unique=true;
        for(int i=0;i<c.count;i++)for(int j=0;j<c.msgs[i].content_count;j++) {
            msg_content_t *b=&c.msgs[i].content[j];
            if(!b->tool_id || !b->type || (strcmp(b->type,"tool_use") && strcmp(b->type,"tool_result")))continue;
            int uses=0,results=0;
            for(int k=0;k<c.count;k++)for(int l=0;l<c.msgs[k].content_count;l++) {
                msg_content_t *x=&c.msgs[k].content[l];
                if(x->tool_id && x->type && !strcmp(x->tool_id,b->tool_id)) {
                    uses+=!strcmp(x->type,"tool_use");results+=!strcmp(x->type,"tool_result");
                }
            }
            if(uses!=1 || results>1)unique=false;
        }
        assert(unique==tool_integrity_index_unique_exchanges(&c));
        conv_free(&c);
    }
    conversation_t c;conv_init(&c);
    for(int i=0;i<20000;i++) {
        char id[64];snprintf(id,sizeof(id),"scale-%d",i);conv_add_assistant_tool_use(&c,id,"read_file","{}");conv_add_tool_result(&c,id,"result",false);
    }
    clock_t start=clock();tool_integrity_result_t indexed;assert(tool_integrity_index_validate(&c,true,&indexed)&&indexed.ok);
    assert(tool_integrity_index_unique_exchanges(&c));
    printf("PASS 20000 differential comparisons and 40000-message/20000-call index; CPU seconds %.4f\n",(double)(clock()-start)/CLOCKS_PER_SEC);conv_free(&c);
}
