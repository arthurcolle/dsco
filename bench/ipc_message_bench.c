/* Native SQLite IPC message drains; compiled against before/after production code. */
#include "ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0+ts.tv_nsec/1e6; }
int main(int argc,char **argv) {
    assert(argc>=4);
    const char *mode=argv[1],*db=argv[2],*identity=argv[3];
    int batch=argc>4?atoi(argv[4]):256, rounds=argc>5?atoi(argv[5]):1;
    assert(batch>0 && batch<=256);
    assert(ipc_init(db,identity));
    if(!strcmp(mode,"init")) { ipc_shutdown(); puts("{}"); return 0; }
    if(!strcmp(mode,"invalid")) {
        ipc_message_t message;
        assert(ipc_recv(NULL,1)==0 && ipc_recv(&message,0)==0 && ipc_recv(&message,-1)==0);
        ipc_shutdown(); puts("{\"invalid_arguments_ignored\":true}"); return 0;
    }
    if(!strcmp(mode,"send") || !strcmp(mode,"live")) {
        assert(ipc_register(NULL,0,"ipc-test","native"));
        puts("ready"); fflush(stdout); assert(getchar()=='g');
    }
    if(!strcmp(mode,"send")) {
        for(int i=0;i<256;i++) { char body[32]; snprintf(body,sizeof(body),"%d",i);
            assert(ipc_send("receiver-a","live",body)); assert(ipc_send("receiver-b","live",body)); }
        ipc_shutdown(); puts("{\"sent\":512}"); return 0;
    }
    ipc_message_t messages[256]; int total=0,steps=0;
    double deadline=ms()+10000;
    printf("{\"batches\":[");
    while((!strcmp(mode,"live")?total<256:steps<rounds)) {
        assert(ms()<deadline);
        double begin=ms();
        int n=!strcmp(mode,"topic")?ipc_recv_topic("chosen",messages,batch):
              !strcmp(mode,"inbox")?ipc_list_inbox(identity,true,true,messages,batch):
              !strcmp(mode,"peek")?ipc_list_inbox(identity,false,false,messages,batch):
              !strcmp(mode,"sent")?ipc_list_sent(identity,messages,batch):ipc_recv(messages,batch);
        double elapsed=ms()-begin;
        if(steps++) putchar(',');
        printf("{\"ms\":%.6f,\"messages\":[",elapsed);
        for(int i=0;i<n;i++) {
            if(i) putchar(',');
            printf("{\"id\":%d,\"from\":\"%s\",\"to\":\"%s\",\"topic\":\"%s\",\"read\":%s,\"body\":\"%s\"}",messages[i].id,messages[i].from_agent,messages[i].to_agent,messages[i].topic,messages[i].read?"true":"false",messages[i].body);
            free(messages[i].body);
        }
        printf("]}"); total+=n;
        if(!strcmp(mode,"live") && !n) usleep(1000);
    }
    printf("],\"total\":%d}\n",total); ipc_shutdown(); return 0;
}
