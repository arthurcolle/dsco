#include "tool_integrity_index.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

typedef struct { const char *id; int at; bool user; } occurrence_t;
static int compare(const void *aa,const void *bb) {
    const occurrence_t *a=aa,*b=bb;int n=strcmp(a->id,b->id);
    return n?n:(a->at>b->at)-(a->at<b->at);
}
/* Lower bound in lexicographic (ID, message position) order. */
static size_t bound(const occurrence_t *a,size_t n,const char *id,int at) {
    size_t lo=0,hi=n;
    while(lo<hi) {
        size_t m=lo+(hi-lo)/2;int c=strcmp(a[m].id,id);
        if(c<0 || (!c && a[m].at<at)) lo=m+1;else hi=m;
    }
    return lo;
}
static void fail(tool_integrity_result_t *r,const char *kind,const char *id,int at) {
    if(!r->ok)return;r->ok=false;
    snprintf(r->first_error,sizeof(r->first_error),"%s: tool_id=%s msg=%d",kind,id,at);
}
static bool type(const msg_content_t *b,const char *s) {return b->type && !strcmp(b->type,s);}

bool tool_integrity_index_validate(const conversation_t *c,bool pending,tool_integrity_result_t *r) {
    memset(r,0,sizeof(*r));r->ok=true;
    if(!c || !c->count)return true;
    size_t nu=0,nr=0;
    for(int i=0;i<c->count;i++)for(int j=0;j<c->msgs[i].content_count;j++) {
        const msg_content_t *b=&c->msgs[i].content[j];if(!b->tool_id)continue;
        nu+=type(b,"tool_use");nr+=type(b,"tool_result");
    }
    if(nu>SIZE_MAX/sizeof(occurrence_t) || nr>SIZE_MAX/sizeof(occurrence_t) ||
       (size_t)c->count>SIZE_MAX/sizeof(int))return false;
    occurrence_t *uses=nu?malloc(nu*sizeof(*uses)):NULL,*results=nr?malloc(nr*sizeof(*results)):NULL;
    int *end=malloc((size_t)c->count*sizeof(*end));
    if((nu&&!uses)||(nr&&!results)||!end) {free(uses);free(results);free(end);return false;}
    size_t u=0,v=0;int next=c->count;
    for(int i=c->count-1;i>=0;i--) {
        end[i]=next;if(c->msgs[i].role!=ROLE_USER)next=i;
        for(int j=0;j<c->msgs[i].content_count;j++) {
            const msg_content_t *b=&c->msgs[i].content[j];if(!b->tool_id)continue;
            occurrence_t x={b->tool_id,i,c->msgs[i].role==ROLE_USER};
            if(type(b,"tool_use"))uses[u++]=x;
            else if(type(b,"tool_result"))results[v++]=x;
        }
    }
    if(nu)qsort(uses,nu,sizeof(*uses),compare);if(nr)qsort(results,nr,sizeof(*results),compare);
    int pending_at=c->count-1;
    if(pending)while(pending_at>=0) {
        const message_t *m=&c->msgs[pending_at];bool only=m->role==ROLE_USER && m->content_count>0;
        for(int j=0;j<m->content_count;j++)only &= type(&m->content[j],"tool_result");
        if(!only)break;pending_at--;
    }
    /* Walk source order to preserve both diagnostic ordering and the legacy
     * convention that duplicate uses within one message are handled by the
     * span matcher, rather than counted as later-message duplicates here. */
    for(int i=0;i<c->count;i++)for(int j=0;j<c->msgs[i].content_count;j++) {
        const msg_content_t *b=&c->msgs[i].content[j];if(!type(b,"tool_use")||!b->tool_id)continue;
        size_t later=bound(uses,nu,b->tool_id,i+1),last=bound(uses,nu,b->tool_id,INT_MAX);
        if(last>later) {
            size_t n=last-later;
            if(n>(size_t)(INT_MAX-r->duplicate_tool_use)) {free(uses);free(results);free(end);return false;}
            r->duplicate_tool_use+=(int)n;fail(r,"duplicate_tool_use",b->tool_id,uses[later].at);
        }
        size_t at=bound(results,nr,b->tool_id,i+1);
        bool found=at<nr && !strcmp(results[at].id,b->tool_id) && results[at].at<end[i];
        /* end[i] bounds the contiguous user run, so any result within it is a
         * user result. An assistant result outside the run cannot satisfy it. */
        if(!found && !(pending && i==pending_at)) {
            r->missing_tool_result++;fail(r,"missing_tool_result",b->tool_id,i);
        }
    }
    for(int i=0;i<c->count;i++)for(int j=0;j<c->msgs[i].content_count;j++) {
        const msg_content_t *b=&c->msgs[i].content[j];if(!type(b,"tool_result")||!b->tool_id)continue;
        size_t at=bound(uses,nu,b->tool_id,0);
        bool found=at<nu && !strcmp(uses[at].id,b->tool_id);
        if(found && uses[at].at<i)continue;
        if(found) {r->out_of_order_result++;fail(r,"out_of_order_result",b->tool_id,i);}
        else {r->missing_tool_use++;fail(r,"missing_tool_use",b->tool_id,i);}
    }
    free(uses);free(results);free(end);return true;
}

bool tool_integrity_index_unique_exchanges(const conversation_t *c) {
    if(!c)return false;
    size_t n=0;
    for(int i=0;i<c->count;i++)for(int j=0;j<c->msgs[i].content_count;j++) {
        const msg_content_t *b=&c->msgs[i].content[j];
        n+=b->tool_id && (type(b,"tool_use")||type(b,"tool_result"));
    }
    if(!n)return true;
    if(n>SIZE_MAX/sizeof(occurrence_t))return false;
    occurrence_t *a=malloc(n*sizeof(*a));if(!a)return false;
    size_t at=0;
    for(int i=0;i<c->count;i++)for(int j=0;j<c->msgs[i].content_count;j++) {
        const msg_content_t *b=&c->msgs[i].content[j];
        if(b->tool_id && (type(b,"tool_use")||type(b,"tool_result")))
            a[at++]=(occurrence_t){b->tool_id,i,type(b,"tool_use")};
    }
    qsort(a,n,sizeof(*a),compare);bool ok=true;
    for(size_t i=0;i<n && ok;) {
        size_t end=i+1,uses=a[i].user,results=!a[i].user;
        while(end<n && !strcmp(a[i].id,a[end].id)) {uses+=a[end].user;results+=!a[end].user;end++;}
        ok=uses==1 && results<=1;i=end;
    }
    free(a);return ok;
}
