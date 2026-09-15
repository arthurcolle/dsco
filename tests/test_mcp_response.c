#include "mcp_response.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *request="{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
int main(void) {
 const char *ok="{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"text\":\"snow 雪 }\"}}";
 char *s=mcp_response_match(ok,strlen(ok),request);assert(s);free(s);
 const char *bad[]={"{}","{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}","{\"jsonrpc\":\"2.0\",\"id\":\"7\",\"result\":{}}","{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}","{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{},\"error\":{}}","{\"id\":7,\"result\":{}}"};
 for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!mcp_response_match(bad[i],strlen(bad[i]),request));
 const char *stream=": ping\r\n\r\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\ndata: {\"jsonrpc\":\"2.0\",\"id\":8,\"result\":0}\n\nevent: message\r\ndata: {\"jsonrpc\":\"2.0\",\r\ndata: \"id\":7,\"result\":{\"value\":\"雪\"}}\r\n\r\n";
 size_t off=0,len=strlen(stream);
 for(size_t i=1;i<len;i++)assert(!mcp_response_sse(stream,i,&off,request));
 s=mcp_response_sse(stream,len,&off,request);assert(s && strstr(s,"雪"));assert(off==len);free(s);
 assert(!mcp_response_sse(stream,len,&off,request));
 const char *err="data: {\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-1,\"message\":\"failed\"}}\n\n";
 off=0;s=mcp_response_sse(err,strlen(err),&off,request);assert(s && strstr(s,"failed"));free(s);
 puts("MCP response tests passed: split UTF-8/CRLF, multiline SSE, notifications, wrong IDs, JSON/error responses, partial events.");
}
