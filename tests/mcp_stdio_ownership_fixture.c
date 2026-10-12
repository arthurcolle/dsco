/* Actual client transport with owned pipes, no tool or provider execution. */
#include "../src/mcp.c"
#include <assert.h>
_Thread_local volatile int tl_tool_cancelled;
int dsco_env_int(const char *n,int d,int min,int max){(void)n;(void)min;(void)max;return d;}
#define THREADS 8
#define CALLS 1000
static mcp_server_t server;
static int read_fd,write_fd;
static bool seen[THREADS*CALLS+1];
static void *peer(void *ctx) {
    (void)ctx;FILE *input=fdopen(read_fd,"r");assert(input);
    char *line=NULL;size_t capacity=0;
    for(int i=0;i<THREADS*CALLS;i++) {
        assert(getline(&line,&capacity,input)>0);
        int id=json_get_int(line,"id",0);assert(id>0 && id<=THREADS*CALLS && !seen[id]);seen[id]=true;
        char reply[1024];int n=snprintf(reply,sizeof(reply),
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":\"stale\"}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"owned\":%d}}\n",id+100000,id,id);
        assert(write(write_fd,reply,(size_t)n)==n);
    }
    free(line);fclose(input);close(write_fd);return NULL;
}
static void *caller(void *ctx) {
    (void)ctx;
    for(int i=0;i<CALLS;i++) {
        int64_t id=rpc_next_id(&server);assert(id>0);
        char *request=rpc_build(id,"tools/call","{}");assert(request);
        char *response=send_rpc_request(&server,request,5000);
        char *result=response ? json_get_raw(response,"result") : NULL;
        assert(result && json_get_int(result,"owned",0)==id);free(result);
        free(request);free(response);
    }
    return NULL;
}
int main(void) {
    alarm(30); /* test bound */
    int in[2],out[2];assert(pipe(in)==0 && pipe(out)==0);
    server.stdin_fd=in[1];server.stdout_fd=out[0];server.rpc_id=1;
    server.stdio_stream=mcp_stream_new();assert(server.stdio_stream);
    read_fd=in[0];write_fd=out[1];
    assert(fcntl(server.stdout_fd,F_SETFL,O_NONBLOCK)==0);
    pthread_t p,threads[THREADS];assert(pthread_create(&p,NULL,peer,NULL)==0);
    for(int i=0;i<THREADS;i++)assert(pthread_create(&threads[i],NULL,caller,NULL)==0);
    for(int i=0;i<THREADS;i++)assert(pthread_join(threads[i],NULL)==0);
    assert(pthread_join(p,NULL)==0);
    for(int i=1;i<=THREADS*CALLS;i++)assert(seen[i]);
    close(server.stdin_fd);close(server.stdout_fd);mcp_stream_free(server.stdio_stream);
    /* A timed-out partial reply remains framed, then is rejected as stale
     * when the next request receives it alongside its own complete result. */
    assert(pipe(in)==0 && pipe(out)==0);
    server.stdin_fd=in[1];server.stdout_fd=out[0];server.stdio_stream=mcp_stream_new();
    assert(server.stdio_stream && fcntl(out[0],F_SETFL,O_NONBLOCK)==0);
    const char *partial="{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":";
    assert(write(out[1],partial,strlen(partial))==(ssize_t)strlen(partial));
    char *req=rpc_build(1,"tools/call","{}");
    assert(!send_rpc_request(&server,req,20) && errno==ETIMEDOUT);free(req);
    assert(mcp_stream_pending(server.stdio_stream)==strlen(partial));
    const char *tail="\"late\"}\n{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":\"current\"}\n";
    assert(write(out[1],tail,strlen(tail))==(ssize_t)strlen(tail));
    req=rpc_build(2,"tools/call","{}");
    tl_tool_cancelled=1;assert(!send_rpc_request(&server,req,1000));tl_tool_cancelled=0;
    char *response=send_rpc_request(&server,req,1000);
    assert(response && strstr(response,"current") && !strstr(response,"late"));
    free(response);free(req);
    close(in[0]);close(in[1]);close(out[0]);close(out[1]);mcp_stream_free(server.stdio_stream);
    server.rpc_id=INT64_MAX-1;
    assert(rpc_next_id(&server)==INT64_MAX-1 && rpc_next_id(&server)==-1 && errno==EOVERFLOW);
    assert(!rpc_build(-1,"tools/call","{}"));
    puts("MCP stdio production transport: 8 concurrent callers x 1000 exchanges, atomic unique IDs, coalesced notifications/stale replies, matched results, timeout/late-reply recovery and cancelled admission passed");
    return 0;
}
