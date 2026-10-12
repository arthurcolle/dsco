#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include "rollout.h"
#include "crypto.h"
#include "json_util.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define HASH_CAP 129
#define CACHE_FILE_CAP (64u*1024u*1024u)
#define CACHE_RECORD_CAP (4u*1024u*1024u)
typedef struct transition {
    char key[65],state_hash[HASH_CAP],action_key[HASH_CAP],next_hash[HASH_CAP];
    rollout_regime_t regime;
    rollout_step_result_t result;
    struct transition *next;
} transition_t;
typedef struct node node_t;
typedef struct {
    rollout_action_t action;
    char key[HASH_CAP];
    rollout_regime_t regime;
    int visits;
    double value;
    transition_t *transition;
    node_t *child;
} edge_t;
struct node {
    rollout_state_t state;
    edge_t *edges;
    int count,visits;
    bool expanded;
    node_t *next_allocated;
};
typedef struct {
    rollout_env_t env;
    rollout_config_t config;
    rollout_result_t *out;
    transition_t *cache[1024];
    node_t *nodes;
    uint64_t rng;
    int cache_fd;
} engine_t;
typedef struct {
    void (*free_action)(rollout_action_t,void *);
    void *ud;
    char (*keys)[HASH_CAP];
} result_owner_t;

void rollout_config_defaults(rollout_config_t *c) {
    if(c) *c=(rollout_config_t){.iterations=400,.rollout_depth=12,.c_uct=1.4142135623730951,
                             .gamma=.98,.max_actions=32,.rng_seed=1};
}
static uint64_t next_random(engine_t *e) {
    uint64_t x=e->rng;x^=x>>12;x^=x<<25;x^=x>>27;e->rng=x;return x*UINT64_C(2685821657736338717);
}
static bool cancelled(engine_t *e) {
    if(e->env.cancelled_fn&&e->env.cancelled_fn(e->env.user_data)) {
        e->out->status=ROLLOUT_STATUS_CANCELLED;return true;
    }return false;
}
static bool valid_hash(const char *s) {
    size_t n=strnlen(s,HASH_CAP);if(!n||n>=HASH_CAP) return false;
    for(size_t i=0;i<n;i++) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F'))) return false;
    return true;
}
static bool state_hash(engine_t *e,rollout_state_t s,char out[HASH_CAP]) {
    memset(out,0,HASH_CAP);e->env.state_hash_fn(s,out,HASH_CAP,e->env.user_data);return valid_hash(out);
}
static void cache_key(engine_t *e,const char *sh,const char *ak,rollout_regime_t regime,char out[65]) {
    sha256_ctx_t h;uint8_t digest[32];sha256_init(&h);
    const char *ns=e->env.cache_namespace?e->env.cache_namespace:"ephemeral";
    sha256_update(&h,(const uint8_t *)"dsco.rollout.v1",15);
    sha256_update(&h,(const uint8_t *)ns,strlen(ns)+1);sha256_update(&h,(const uint8_t *)sh,strlen(sh)+1);
    sha256_update(&h,(const uint8_t *)ak,strlen(ak)+1);uint8_t r=(uint8_t)regime;sha256_update(&h,&r,1);
    sha256_final(&h,digest);hex_encode(digest,32,out);
}
static unsigned bucket(const char *key) {
    unsigned n=0;for(int i=0;i<8;i++) n=n*33u+(unsigned char)key[i];return n%1024;
}
static transition_t *cache_find(engine_t *e,const char *key) {
    for(transition_t *t=e->cache[bucket(key)];t;t=t->next) if(!strcmp(t->key,key)) return t;return NULL;
}
static void free_state(engine_t *e,rollout_state_t s) {
    if(s&&s!=e->env.initial_state&&e->env.free_state_fn) e->env.free_state_fn(s,e->env.user_data);
}
static void cache_add(engine_t *e,transition_t *t) {
    unsigned i=bucket(t->key);t->next=e->cache[i];e->cache[i]=t;
}
static int cache_open(engine_t *e) {
    if(!e->config.cache_path) return -1;
    int fd=open(e->config.cache_path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);if(fd<0) return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_size>CACHE_FILE_CAP||flock(fd,LOCK_EX|LOCK_NB)) {
        close(fd);errno=EPERM;return -1;
    }return fd;
}
static void cache_load(engine_t *e) {
    int copy=fcntl(e->cache_fd,F_DUPFD_CLOEXEC,0);if(copy<0) { e->out->cache_errors++;return; }
    FILE *f=fdopen(copy,"r");if(!f) { close(copy);e->out->cache_errors++;return; }
    char *line=NULL;size_t cap=0;ssize_t n;int count=0;off_t row_start=0;
    while(!cancelled(e)&&(n=getline(&line,&cap,f))>0) {
        if(line[n-1]!='\n') {
            e->out->cache_errors++;
            if(ftruncate(e->cache_fd,row_start)||fsync(e->cache_fd)) { close(e->cache_fd);e->cache_fd=-1; }
            break;
        }
        row_start+=n;
        if((size_t)n>CACHE_RECORD_CAP||++count>65536) { e->out->cache_errors++;break; }
        json_view_t *v=json_view_open(line);if(!v) { e->out->cache_errors++;continue; }
        const char *ns=json_view_str(v,"namespace"),*key=json_view_str(v,"key"),*sh=json_view_str(v,"state_hash"),
                   *ak=json_view_str(v,"action_key"),*nh=json_view_str(v,"next_hash"),*state=json_view_str(v,"state");
        long long raw_regime=json_view_i64(v,"regime",-1);int r=raw_regime>=0&&raw_regime<=2?(int)raw_regime:-1;
        double reward=json_view_f64(v,"reward",NAN);const char *schema=json_view_str(v,"schema");
        if(!ns||strcmp(ns,e->env.cache_namespace)) { json_view_close(v);continue; }
        char expected[65];
        if(!schema||strcmp(schema,"dsco.rollout.v1")||!key||!sh||!ak||!nh||!state||!valid_hash(sh)||!valid_hash(ak)||!valid_hash(nh)||r<0||r>2||!isfinite(reward)) goto invalid;
        cache_key(e,sh,ak,(rollout_regime_t)r,expected);if(strcmp(expected,key)) goto invalid;
        if(cache_find(e,key)) { json_view_close(v);continue; }
        transition_t *t=calloc(1,sizeof(*t));if(!t) { e->out->cache_errors++;json_view_close(v);break; }
        t->result.next_state=e->env.deserialize_state_fn(state,e->env.user_data);
        char actual[HASH_CAP];
        if(!t->result.next_state||!state_hash(e,t->result.next_state,actual)||strcmp(actual,nh)) {
            free_state(e,t->result.next_state);free(t);goto invalid;
        }
        const char *obs=json_view_str(v,"observation");
        if(obs&&!(t->result.observation_json=strdup(obs))) { free_state(e,t->result.next_state);free(t);goto invalid; }
        snprintf(t->key,sizeof(t->key),"%s",key);snprintf(t->state_hash,HASH_CAP,"%s",sh);snprintf(t->action_key,HASH_CAP,"%s",ak);
        snprintf(t->next_hash,HASH_CAP,"%s",nh);t->regime=(rollout_regime_t)r;t->result.reward=reward;cache_add(e,t);
        json_view_close(v);continue;
invalid:e->out->cache_errors++;json_view_close(v);
    }
    if(ferror(f)) e->out->cache_errors++;
    free(line);fclose(f);
}
static void cache_save(engine_t *e,transition_t *t) {
    if(e->cache_fd<0) return;
    char *state=e->env.serialize_state_fn(t->result.next_state,e->env.user_data);
    if(!state||strlen(state)>CACHE_RECORD_CAP/8) { free(state);e->out->cache_errors++;return; }
    jbuf_t b;jbuf_init(&b,512);jbuf_append(&b,"{\"schema\":\"dsco.rollout.v1\",\"namespace\":");jbuf_append_json_str(&b,e->env.cache_namespace);
#define FIELD(name,value) do { jbuf_append(&b,",\"" name "\":");jbuf_append_json_str(&b,value); } while(0)
    FIELD("key",t->key);FIELD("state_hash",t->state_hash);FIELD("action_key",t->action_key);FIELD("next_hash",t->next_hash);
    FIELD("state",state);FIELD("observation",t->result.observation_json);
#undef FIELD
    jbuf_appendf(&b,",\"regime\":%d,\"reward\":%.17g}\n",t->regime,t->result.reward);free(state);
    off_t start=lseek(e->cache_fd,0,SEEK_END);
    if(!b.data||b.len>CACHE_RECORD_CAP||start<0||(uint64_t)start+b.len>CACHE_FILE_CAP) goto fail;
    size_t at=0;
    while(at<b.len) { ssize_t n=write(e->cache_fd,b.data+at,b.len-at);if(n<0&&errno==EINTR) continue;if(n<=0) goto rollback;at+=(size_t)n; }
    if(fsync(e->cache_fd)) goto rollback;
    jbuf_free(&b);return;
rollback:if(ftruncate(e->cache_fd,start)) { close(e->cache_fd);e->cache_fd=-1; }
fail:e->out->cache_errors++;jbuf_free(&b);
}
static bool authorized(engine_t *e,rollout_state_t s,edge_t *a) {
    if(a->regime!=ROLLOUT_UMBRA) return true;
    if(!e->env.authorize_fn||!e->env.authorize_fn(s,a->action,a->regime,false,e->env.user_data)) {
        e->out->status=ROLLOUT_STATUS_DENIED;return false;
    }return true;
}
static transition_t *transition(engine_t *e,node_t *n,edge_t *a) {
    if(cancelled(e)) return NULL;
    if(e->env.regime_fn(a->action,e->env.user_data)!=a->regime) goto bad;
    if(!authorized(e,n->state,a)||cancelled(e)) return NULL;
    if(e->env.regime_fn(a->action,e->env.user_data)!=a->regime) goto bad;
    char sh[HASH_CAP],key[65];if(!state_hash(e,n->state,sh)) goto bad;
    cache_key(e,sh,a->key,a->regime,key);transition_t *t=cache_find(e,key);
    if(t) {
        e->out->cache_hits++;
        if(a->regime==ROLLOUT_UMBRA&&e->env.action_cost_fn) {
            double cost=e->env.action_cost_fn(a->action,e->env.user_data);if(isfinite(cost)&&cost>0) e->out->cost_saved+=cost;
        }return t;
    }
    t=calloc(1,sizeof(*t));if(!t) return NULL;
    snprintf(t->key,sizeof(t->key),"%s",key);snprintf(t->state_hash,HASH_CAP,"%s",sh);snprintf(t->action_key,HASH_CAP,"%s",a->key);t->regime=a->regime;
    /* Regime is captured at enumeration, but must still agree at execution. */
    if(e->env.regime_fn(a->action,e->env.user_data)!=a->regime) { free(t);goto bad; }
    if(cancelled(e)) { free(t);return NULL; }
    rollout_state_t input=e->env.clone_state_fn?e->env.clone_state_fn(n->state,e->env.user_data):n->state;
    if(!input) { free(t);return NULL; }
    if(a->regime==ROLLOUT_UMBRA) {
        t->result=e->env.execute_fn(input,a->action,e->env.user_data);e->out->real_executions++;
        if(e->env.action_cost_fn) { double c=e->env.action_cost_fn(a->action,e->env.user_data);if(isfinite(c)&&c>0) e->out->cost_paid+=c; }
    } else { t->result=e->env.model_fn(input,a->action,e->env.user_data);e->out->simulated_executions++; }
    if(e->env.clone_state_fn&&t->result.next_state!=input) free_state(e,input);
    e->out->cache_misses++;
    if(!t->result.next_state||!isfinite(t->result.reward)||!state_hash(e,t->result.next_state,t->next_hash)) {
        free_state(e,t->result.next_state);free(t->result.observation_json);free(t);goto bad;
    }
    cache_add(e,t);cache_save(e,t);return t;
bad:e->out->status=ROLLOUT_STATUS_CALLBACK_ERROR;return NULL;
}
static node_t *new_node(engine_t *e,rollout_state_t state) {
    node_t *n=calloc(1,sizeof(*n));if(!n) return NULL;n->state=state;n->next_allocated=e->nodes;e->nodes=n;return n;
}
static bool expand(engine_t *e,node_t *n) {
    if(n->expanded) return true;
    if(e->env.is_terminal_fn(n->state,e->env.user_data)) { n->expanded=true;return true; }
    rollout_action_t *actions=calloc((size_t)e->config.max_actions,sizeof(*actions));if(!actions) return false;
    int count=e->env.actions_fn(n->state,actions,e->config.max_actions,e->env.user_data);
    if(count<0||count>e->config.max_actions) { free(actions);e->out->status=ROLLOUT_STATUS_CALLBACK_ERROR;return false; }
    n->edges=calloc((size_t)(count?count:1),sizeof(*n->edges));if(!n->edges) { free(actions);return false; }
    n->expanded=true;
    for(int i=0;i<count;i++) {
        edge_t *a=&n->edges[n->count];
        a->action=e->env.clone_action_fn?e->env.clone_action_fn(actions[i],e->env.user_data):actions[i];
        if(!a->action) { free(actions);return false; }n->count++;
        a->regime=e->env.regime_fn(a->action,e->env.user_data);
        e->env.action_key_fn(a->action,a->key,HASH_CAP,e->env.user_data);
        if(a->regime<0||a->regime>2||!valid_hash(a->key)) { free(actions);e->out->status=ROLLOUT_STATUS_CALLBACK_ERROR;return false; }
        for(int j=0;j<i;j++) if(!strcmp(a->key,n->edges[j].key)) { free(actions);e->out->status=ROLLOUT_STATUS_CALLBACK_ERROR;return false; }
    }
    free(actions);return true;
}
static edge_t *choose(engine_t *e,node_t *n,bool final) {
    edge_t *best=NULL;double value=-INFINITY;int unvisited=0;
    if(!final) for(int i=0;i<n->count;i++) if(!n->edges[i].visits) unvisited++;
    if(unvisited) {
        int selected=(int)(next_random(e)%(uint64_t)unvisited);
        for(int i=0;i<n->count;i++) if(!n->edges[i].visits&&selected--==0) return &n->edges[i];
    }
    for(int i=0;i<n->count;i++) {
        edge_t *a=&n->edges[i];if(!a->visits) continue;
        double score=a->value/a->visits;
        if(!final) score+=e->config.c_uct*sqrt(log((double)n->visits+1)/a->visits);
        if(!best||score>value) { best=a;value=score; }
    }return best;
}
static void engine_free(engine_t *e) {
    for(node_t *n=e->nodes;n;) {
        node_t *next=n->next_allocated;
        if(e->env.free_action_fn) for(int i=0;i<n->count;i++) e->env.free_action_fn(n->edges[i].action,e->env.user_data);
        free(n->edges);free(n);n=next;
    }
    for(int i=0;i<1024;i++) for(transition_t *t=e->cache[i];t;) {
        transition_t *next=t->next;free_state(e,t->result.next_state);free(t->result.observation_json);free(t);t=next;
    }
    if(e->cache_fd>=0) close(e->cache_fd);
}
rollout_result_t *rollout_search(const rollout_env_t *env,const rollout_config_t *cfg) {
    rollout_config_t defaults;rollout_config_defaults(&defaults);if(!cfg) cfg=&defaults;
    if(!env||!env->initial_state||!env->actions_fn||!env->execute_fn||!env->model_fn||!env->regime_fn||!env->is_terminal_fn||!env->state_hash_fn||!env->action_key_fn||
       (!!env->clone_action_fn!=!!env->free_action_fn)|| (!!env->clone_state_fn!=!!env->free_state_fn)||
       cfg->iterations<1||cfg->rollout_depth<1||cfg->rollout_depth>128||cfg->iterations>65536/cfg->rollout_depth||cfg->max_actions<1||cfg->max_actions>256||
       !isfinite(cfg->c_uct)||cfg->c_uct<0||!isfinite(cfg->gamma)||cfg->gamma<0||cfg->gamma>1||
       (cfg->cache_path&&(!env->serialize_state_fn||!env->deserialize_state_fn||!env->cache_namespace||!env->cache_namespace[0]||strlen(env->cache_namespace)>1024))) { errno=EINVAL;return NULL; }
    rollout_result_t *out=calloc(1,sizeof(*out));if(!out) return NULL;
    engine_t e={.env=*env,.config=*cfg,.out=out,.rng=cfg->rng_seed?cfg->rng_seed:UINT64_C(0x9e3779b97f4a7c15),.cache_fd=-1};
    if(cfg->cache_path) { e.cache_fd=cache_open(&e);if(e.cache_fd<0) out->cache_errors++;else cache_load(&e); }
    node_t *root=new_node(&e,env->initial_state);if(!root) goto oom;
    for(int it=0;it<cfg->iterations;it++) {
        if(cancelled(&e)) break;
        node_t *n=root,*path[128];edge_t *edges[128];double rewards[128];int depth=0;
        while(depth<cfg->rollout_depth) {
            if(cancelled(&e)) break;
            if(!expand(&e,n)) { if(out->status==ROLLOUT_STATUS_OK) goto oom;break; }
            if(!n->count) break;
            edge_t *a=choose(&e,n,false);if(!a) break;
            transition_t *t=transition(&e,n,a);if(!t) { if(out->status==ROLLOUT_STATUS_OK) goto oom;break; }
            a->transition=t;path[depth]=n;edges[depth]=a;rewards[depth++]=t->result.reward;
            if(!a->child) { a->child=new_node(&e,t->result.next_state);if(!a->child) goto oom; }
            n=a->child;
        }
        double value=0;for(int i=depth-1;i>=0;i--) {
            value=rewards[i]+cfg->gamma*value;
            if(!isfinite(value)||!isfinite(edges[i]->value+value)) { out->status=ROLLOUT_STATUS_CALLBACK_ERROR;break; }
            edges[i]->value+=value;edges[i]->visits++;path[i]->visits++;
        }
        out->iterations_done++;
        if(out->status!=ROLLOUT_STATUS_OK) break;
    }
    result_owner_t *owner=calloc(1,sizeof(*owner));if(!owner) goto oom;
    out->engine_private=owner;owner->free_action=env->free_action_fn;owner->ud=env->user_data;
    owner->keys=calloc((size_t)cfg->rollout_depth,sizeof(*owner->keys));
    out->steps=calloc((size_t)cfg->rollout_depth,sizeof(*out->steps));
    out->needs_realization=calloc((size_t)cfg->rollout_depth,sizeof(int));out->needs_authority=calloc((size_t)cfg->rollout_depth,sizeof(int));
    if(!owner->keys||!out->steps||!out->needs_realization||!out->needs_authority) goto oom;
    node_t *n=root;double discount=1;
    for(int i=0;i<cfg->rollout_depth;i++) {
        edge_t *a=choose(&e,n,true);if(!a||!a->transition) break;
        rollout_action_t action=env->clone_action_fn?env->clone_action_fn(a->action,env->user_data):a->action;if(!action) goto oom;
        out->steps[i]=(rollout_traj_step_t){.action=action,.regime=a->regime,.reward=a->transition->result.reward,
            .visits=a->visits,.q_value=a->value/a->visits,.realized=a->regime==ROLLOUT_UMBRA};
        snprintf(owner->keys[i],HASH_CAP,"%s",a->key);out->step_count++;
        out->total_reward+=discount*out->steps[i].reward;discount*=cfg->gamma;
        if(a->regime==ROLLOUT_PENUMBRA) out->needs_realization[out->needs_realization_count++]=i;
        else if(a->regime==ROLLOUT_ANTUMBRA) out->needs_authority[out->needs_authority_count++]=i;
        if(!a->child) break;n=a->child;
    }
    engine_free(&e);return out;
oom:engine_free(&e);rollout_result_free(out);errno=ENOMEM;return NULL;
}
void rollout_result_free(rollout_result_t *r) {
    if(!r) return;result_owner_t *owner=r->engine_private;
    if(owner) {
        if(owner->free_action) for(int i=0;i<r->step_count;i++) owner->free_action(r->steps[i].action,owner->ud);
        free(owner->keys);free(owner);
    }
    free(r->steps);free(r->needs_realization);free(r->needs_authority);free(r);
}
char *rollout_result_to_plan_json(const rollout_result_t *r,const char *task) {
    if(!r||!task||r->step_count<0||(r->step_count&&!r->steps)) { errno=EINVAL;return NULL; }
    result_owner_t *owner=r->engine_private;jbuf_t b;jbuf_init(&b,512);
    jbuf_append(&b,"{\"schema\":\"dsco.rollout.plan.v1\",\"task\":");jbuf_append_json_str(&b,task);
    jbuf_appendf(&b,",\"status\":%d,\"total_reward\":%.17g,\"steps\":[",r->status,r->total_reward);
    for(int i=0;i<r->step_count;i++) {
        const rollout_traj_step_t *s=&r->steps[i];if(i) jbuf_append_char(&b,',');
        jbuf_appendf(&b,"{\"index\":%d,\"action_key\":",i);jbuf_append_json_str(&b,owner?owner->keys[i]:NULL);
        jbuf_appendf(&b,",\"regime\":%d,\"realized\":%s,\"requires_authority\":%s,\"reward\":%.17g,\"visits\":%d,\"q_value\":%.17g}",
            s->regime,s->realized?"true":"false",s->regime==ROLLOUT_ANTUMBRA?"true":"false",s->reward,s->visits,s->q_value);
    }
    jbuf_append(&b,"]}");return b.data;
}
rollout_step_result_t rollout_realize_step(const rollout_env_t *env,rollout_state_t s,rollout_action_t a) {
    rollout_step_result_t zero={0};
    if(!env||!s||!a||!env->regime_fn||!env->execute_fn||!env->authorize_fn) { errno=EINVAL;return zero; }
    if(env->cancelled_fn&&env->cancelled_fn(env->user_data)) { errno=ECANCELED;return zero; }
    rollout_regime_t regime=env->regime_fn(a,env->user_data);
    if(regime!=ROLLOUT_PENUMBRA||!env->authorize_fn(s,a,regime,true,env->user_data)) { errno=EPERM;return zero; }
    if(env->cancelled_fn&&env->cancelled_fn(env->user_data)) { errno=ECANCELED;return zero; }
    if(env->regime_fn(a,env->user_data)!=ROLLOUT_PENUMBRA) { errno=EPERM;return zero; }
    rollout_step_result_t result=env->execute_fn(s,a,env->user_data);
    if(!result.next_state||!isfinite(result.reward)) {
        if(result.next_state&&env->free_state_fn) env->free_state_fn(result.next_state,env->user_data);
        free(result.observation_json);errno=EIO;return zero;
    }return result;
}
