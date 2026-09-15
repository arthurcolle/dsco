#define _DARWIN_C_SOURCE 1
#include "deepseek_pricing.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static time_t utc(int day,int hour,int min){struct tm t={.tm_year=126,.tm_mon=8,.tm_mday=day,.tm_hour=hour,.tm_min=min};return timegm(&t);}
int main(int argc,char **argv){
 const char *path=argc>1?argv[1]:"tests/fixtures/deepseek_pricing.html";
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *s=malloc(n+1);assert(s);assert(fread(s,1,n,f)==(size_t)n);s[n]=0;fclose(f);
 char home[]="/tmp/dsco-deepseek-fixture-XXXXXX";assert(mkdtemp(home));assert(!setenv("HOME",home,1));
 char dir[256],cache[256];snprintf(dir,sizeof(dir),"%s/.dsco",home);assert(!mkdir(dir,0700));snprintf(cache,sizeof(cache),"%s/deepseek_pricing.html",dir);
 FILE *saved=fopen(cache,"wb");assert(saved);assert(fwrite(s,1,n,saved)==(size_t)n);assert(!fclose(saved));
 deepseek_pricing_load_cached();model_price_t cached;assert(deepseek_pricing_lookup("deepseek-v4-flash",utc(4,1,0),&cached,NULL,NULL));assert(cached.input==.44);
 assert(deepseek_pricing_load_html(s,123));model_price_t p;const char *source;time_t observed;
 assert(deepseek_pricing_lookup("deepseek-v4-flash",utc(4,1,0),&p,&source,&observed));assert(p.input==.44&&p.cached_input==.014&&p.output==1.32&&p.cache_write==-1&&observed==123);
 assert(deepseek_pricing_lookup("deepseek/deepseek-v4-flash",utc(4,4,0),&p,&source,NULL));assert(p.input==.22);
 assert(deepseek_pricing_lookup("deepseek-v4-pro",utc(4,6,0),&p,NULL,NULL));assert(p.input==1.32);
 assert(deepseek_pricing_lookup("deepseek-v4-pro",utc(4,10,0),&p,NULL,NULL));assert(p.input==.66);
 assert(deepseek_pricing_lookup("deepseek-v4-flash",utc(5,2,0),&p,NULL,NULL));assert(p.input==.22);
 assert(!deepseek_pricing_lookup("deepseek-future",utc(4,2,0),&p,NULL,NULL));
 char *v=strstr(s,"$0.007");assert(v);memcpy(v,"$-1.00",6);assert(!deepseek_pricing_load_html(s,999));
 assert(deepseek_pricing_lookup("deepseek-v4-flash",utc(4,2,0),&p,NULL,&observed)&&observed==123);
 memcpy(v,"$0.007",6);v=strstr(s,"Monday through Friday");assert(v);v[0]='X';assert(!deepseek_pricing_load_html(s,999));
 assert(!deepseek_pricing_load_html("<html>home page</html>",999));free(s);
 saved=fopen(cache,"wb");assert(saved);fputs("broken cache",saved);fclose(saved);deepseek_pricing_load_cached();
 assert(deepseek_pricing_lookup("deepseek-v4-flash",utc(4,2,0),&p,NULL,&observed)&&observed==123);
 unlink(cache);rmdir(dir);rmdir(home);puts("PASS: live schema, six rates, UTC boundaries, unknown model, malformed retention");
}
