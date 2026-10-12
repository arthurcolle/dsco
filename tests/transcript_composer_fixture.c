#include "tui.h"
#include "transcript_view.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
int main(void) {
    setenv("DSCO_PIXEL_TUI","0",1);
    setenv("DSCO_TUI_ANIMATIONS","0",1);
    msg_content_t part={.type="text",.text="TRANSCRIPT_FIXTURE_CODE\n```c\n   return 7;\n```"};
    message_t message={.role=ROLE_ASSISTANT,.content=&part,.content_count=1};
    conversation_t conv={.msgs=&message,.count=1};
    tui_composer_set_transcript_hook(transcript_view_show,&conv);
    tui_status_bar_t status;
    tui_status_bar_init(&status,"transcript fixture");status.animations_enabled=false;
    char input[128];char *result=tui_composer_read(&status,NULL,input,sizeof(input));
    tui_composer_set_transcript_hook(NULL,NULL);
    tui_cleanup();
    printf("\nSUBMITTED:%s\n",result?result:"cancelled");fflush(stdout);
    pthread_mutex_destroy(&status.mutex);
    return result?0:1;
}
