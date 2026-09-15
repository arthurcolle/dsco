/* Private transport fixture: no remote services, tool dispatch or UI. */
#include "../src/mcp.c"
int dsco_env_int(const char *name, int fallback, int min, int max) {
    (void)name; (void)min; (void)max; return fallback;
}
int main(int argc, char **argv) {
    if(argc!=2)return 2;
    mcp_server_t server={0};snprintf(server.url,sizeof(server.url),"%s",argv[1]);
    char *out=http_post_rpc(&server,"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}",1500);
    if(!out)return 1;
    puts(out);free(out);return 0;
}
