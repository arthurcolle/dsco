#define _DARWIN_C_SOURCE 1
#include "deepseek_pricing.h"
#include <curl/curl.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define LIMIT (2 * 1024 * 1024)
#define MODELS 16
typedef struct { char id[128]; model_price_t off, peak; } entry_t;
typedef struct { entry_t entries[MODELS]; int count, bands[4]; time_t observed; } snapshot_t;
static snapshot_t current;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* No entity decoding: an unexpected representation fails schema validation. */
static int plain(const char *p, const char *end, char *out, size_t cap) {
    size_t n=0; int tag=0, space=0;
    for (; p<end; p++) {
        if (*p=='<') {tag=1;space=1;continue;}
        if (*p=='>') {tag=0;continue;}
        if (tag) continue;
        if (isspace((unsigned char)*p)) {space=1;continue;}
        if (space && n) {if(n+1>=cap)return 0;out[n++]=' ';} space=0;
        if(n+1>=cap)return 0;out[n++]=*p;
    }
    out[n]=0; return 1;
}
int deepseek_pricing_load_html(const char *html, time_t observed) {
    if (!html || strlen(html)>LIMIT || !strstr(html,"Models &amp; Pricing | DeepSeek API Docs") ||
        !strstr(html,"https://api.deepseek.com") || !strstr(html,"per 1M tokens")) return 0;
    snapshot_t next={0}; next.observed=observed;
    const char *table=strstr(html,"<table"), *end=table?strstr(table,"</table>"):NULL;
    if (!end) return 0;
    char text[16384];
    const char *schedule=strstr(end,"Peak hours are");
    if (!schedule || !plain(schedule,html+strlen(html),text,sizeof(text))) return 0;
    int h1,m1,h2,m2,h3,m3,h4,m4,used=0;
    if(sscanf(text,"Peak hours are %d:%d - %d:%d and %d:%d - %d:%d UTC, Monday through Friday (all other hours are off-peak).%n",
        &h1,&m1,&h2,&m2,&h3,&m3,&h4,&m4,&used)!=8 || !used) return 0;
    int hs[]={h1,h2,h3,h4},ms[]={m1,m2,m3,m4};
    for(int i=0;i<4;i++){if(hs[i]<0||hs[i]>23||ms[i]<0||ms[i]>59)return 0;next.bands[i]=hs[i]*60+ms[i];}
    if(next.bands[0]>=next.bands[1]||next.bands[1]>next.bands[2]||next.bands[2]>=next.bands[3])return 0;
    int price_row=0;
    for(const char *row=table;(row=strstr(row,"<tr")) && row<end;) {
        const char *stop=strstr(row,"</tr>"); if(!stop||stop>end)return 0;
        char cells[20][256]; int count=0;
        for(const char *cell=row;(cell=strstr(cell,"<td")) && cell<stop;) {
            const char *a=strchr(cell,'>'), *b=a?strstr(a,"</td>"):NULL;
            if(!a||!b||b>stop||count==20||!plain(a+1,b,cells[count++],256))return 0;
            cell=b+5;
        }
        if(count && !strcmp(cells[0],"MODEL")) {
            if(next.count || count<2||count>MODELS+1)return 0;
            next.count=count-1;
            for(int i=0;i<next.count;i++) {
                if(strncmp(cells[i+1],"deepseek-",9)||strlen(cells[i+1])>=128)return 0;
                for(int j=0;j<i;j++)if(!strcmp(next.entries[j].id,cells[i+1]))return 0;
                strcpy(next.entries[i].id,cells[i+1]);
                next.entries[i].off.cache_write=next.entries[i].peak.cache_write=-1;
            }
        }
        int band=-1;
        for(int i=0;i<count;i++)if(!strcmp(cells[i],"OFF-PEAK")||!strcmp(cells[i],"PEAK")){if(band>=0)return 0;band=i;}
        if(band>=0) {
            if(!next.count||price_row>=6||count-band-1!=next.count || strcmp(cells[band],price_row%2?"PEAK":"OFF-PEAK"))return 0;
            const char *labels[]={"1M INPUT TOKENS (CACHE HIT)","1M INPUT TOKENS (CACHE MISS)","1M OUTPUT TOKENS"};
            if(!(price_row%2) && (band<1||strcmp(cells[band-1],labels[price_row/2])))return 0;
            if(price_row%2 && band!=0)return 0;
            for(int i=0;i<next.count;i++) {
                char *tail; const char *v=cells[band+1+i];
                double rate=strtod(v+1,&tail);
                if(*v!='$'||tail==v+1||*tail||!isfinite(rate)||rate<0)return 0;
                model_price_t *p=price_row%2?&next.entries[i].peak:&next.entries[i].off;
                if(price_row/2==0)p->cached_input=rate;else if(price_row/2==1)p->input=rate;else p->output=rate;
            }
            price_row++;
        }
        row=stop+5;
    }
    if(price_row!=6)return 0;
    pthread_mutex_lock(&lock);current=next;pthread_mutex_unlock(&lock);return 1;
}
int deepseek_pricing_lookup(const char *model,time_t at,model_price_t *out,const char **source,time_t *observed) {
    if(!model||!out)return 0;
    if(!strncmp(model,"deepseek/",9))model+=9;
    struct tm utc;if(!gmtime_r(&at,&utc))return 0;
    pthread_mutex_lock(&lock);
    for(int i=0;i<current.count;i++)if(!strcmp(model,current.entries[i].id)) {
        int minute=utc.tm_hour*60+utc.tm_min;
        int peak=utc.tm_wday>=1&&utc.tm_wday<=5&&((minute>=current.bands[0]&&minute<current.bands[1])||(minute>=current.bands[2]&&minute<current.bands[3]));
        *out=peak?current.entries[i].peak:current.entries[i].off;
        if(source)*source=peak?"deepseek_first_party_peak":"deepseek_first_party_off_peak";
        if(observed)*observed=current.observed;
        pthread_mutex_unlock(&lock);return 1;
    }
    pthread_mutex_unlock(&lock);return 0;
}
static int cache_path(char *out,size_t cap) {
    const char *home=getenv("HOME");return home&&snprintf(out,cap,"%s/.dsco/deepseek_pricing.html",home)<(int)cap;
}
void deepseek_pricing_load_cached(void) {
    char path[2048];struct stat st;if(!cache_path(path,sizeof(path))||stat(path,&st)||st.st_size<=0||st.st_size>LIMIT)return;
    FILE *f=fopen(path,"rb");if(!f)return;
    char *buf=malloc((size_t)st.st_size+1);if(!buf){fclose(f);return;}
    size_t n=fread(buf,1,(size_t)st.st_size,f);fclose(f);buf[n]=0;
    if(n==(size_t)st.st_size)deepseek_pricing_load_html(buf,st.st_mtime);free(buf);
}
typedef struct {char *data;size_t n;} body_t;
static size_t receive(void *data,size_t size,size_t count,void *arg) {
    body_t *b=arg;if(size&&count>LIMIT/size)return 0;size_t n=size*count;
    if(n>LIMIT-b->n)return 0;char *p=realloc(b->data,b->n+n+1);if(!p)return 0;
    b->data=p;memcpy(p+b->n,data,n);b->n+=n;p[b->n]=0;return n;
}
int deepseek_pricing_refresh_sync(void) {
    if(getenv("DSCO_PRICING_OFFLINE"))return 0;
    CURL *curl=curl_easy_init();if(!curl)return 0;body_t body={0};long status=0;
    curl_easy_setopt(curl,CURLOPT_URL,DEEPSEEK_PRICING_URL);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,8L);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,4L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);
    CURLcode result=curl_easy_perform(curl);curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_cleanup(curl);
    int ok=result==CURLE_OK&&status==200&&deepseek_pricing_load_html(body.data,time(NULL));
    if(ok) {
        char path[2048],tmp[2100];
        if(cache_path(path,sizeof(path))) {
            snprintf(tmp,sizeof(tmp),"%s.tmp.XXXXXX",path);int fd=mkstemp(tmp);
            if(fd>=0) {
                FILE *f=fdopen(fd,"wb");int saved=0;
                if(f){saved=fwrite(body.data,1,body.n,f)==body.n&&fflush(f)==0&&fsync(fd)==0;if(fclose(f))saved=0;}
                else close(fd);
                if(!saved||rename(tmp,path))unlink(tmp);
            }
        }
    }
    free(body.data);return ok;
}
