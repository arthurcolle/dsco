#include "mcp_stream.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static const char *request="{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\"}";
static const char *batch="garbage\n\r\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":\"stale\"}\n{\"jsonrpc\":\"2.0\",\"id\":\"7\",\"result\":\"wrong type\"}\n{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"owned\":\"π 🦉\"}}\r\n{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-1,\"message\":\"next\"}}\n";
int main(void) {
    size_t len=strlen(batch);
    for(size_t split=0;split<=len;split++) {
        mcp_stream_t *s=mcp_stream_new();assert(s && mcp_stream_lock(s));
        assert(mcp_stream_feed(s,batch,split));char *result=mcp_stream_take(s,request);
        assert(mcp_stream_feed(s,batch+split,len-split));
        if(!result)result=mcp_stream_take(s,request);
        assert(result && strstr(result,"π 🦉") && !strstr(result,"stale"));free(result);
        result=mcp_stream_take(s,"{\"id\":8}");assert(result && strstr(result,"next"));free(result);
        assert(!mcp_stream_pending(s));mcp_stream_unlock(s);mcp_stream_free(s);
    }
    mcp_stream_t *s=mcp_stream_new();assert(s);
    /* Every byte boundary, including nested closing braces and UTF8 splits. */
    const char *nested="{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"inner\":{},\"tail\":\"雪\"}}";
    for(size_t i=0;i<strlen(nested);i++) {
        assert(mcp_stream_feed(s,nested+i,1));char *r=mcp_stream_take(s,request);
        if(i+1<strlen(nested))assert(!r);else {assert(r && strstr(r,"雪"));free(r);}
    }
    for(int i=0;i<100000;i++) {
        assert(mcp_stream_feed(s,batch,strlen(batch)));
        char *r=mcp_stream_take(s,request);assert(r);free(r);
        r=mcp_stream_take(s,"{\"id\":8}");assert(r);free(r);
        assert(mcp_stream_pending(s)==0);
    }
    const char *partial="{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":";
    assert(mcp_stream_feed(s,partial,strlen(partial)));assert(!mcp_stream_take(s,request));
    const char *finish="{} }\n{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":false}\n";
    assert(mcp_stream_feed(s,finish,strlen(finish)));char *r=mcp_stream_take(s,request);assert(r && strstr(r,"false"));free(r);
    assert(!mcp_stream_feed(s,"x\0y",3) && errno==EPROTO);
    assert(!mcp_stream_feed(s,"x",16u*1024u*1024u+1) && errno==EOVERFLOW);
    assert(mcp_stream_feed(s,"{",1));assert(!mcp_stream_take(s,request));
    assert(mcp_stream_pending(s)==1);mcp_stream_free(s);
    printf("MCP framing: %zu split boundaries, bytewise nested UTF8, 100000 persistent bursts, stale/type/notification filtering, partial retention and limits passed\n",len+1);
    return 0;
}
