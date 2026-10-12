#include "rollout.h"
#include "json_util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
typedef struct { int depth; } state_t;
typedef struct { int id; rollout_regime_t regime; double reward; } action_t;
typedef struct { int real[3],sim[3],allocated,freed,actions_cloned,actions_freed; bool deny,cancel,cancel_on_auth,flip_on_auth; int horizon; } fixture_t;
static action_t actions[3]={{1,ROLLOUT_UMBRA,1},{2,ROLLOUT_PENUMBRA,5},{3,ROLLOUT_ANTUMBRA,10}};
static int list(rollout_state_t s,rollout_action_t *out,int max,void *ud) { (void)s;(void)ud;assert(max>=3);for(int i=0;i<3;i++)out[i]=&actions[i];return 3; }
static rollout_step_result_t step(rollout_state_t s,rollout_action_t a,void *ud,bool real) {
    fixture_t *f=ud;action_t *ac=a;state_t *next=malloc(sizeof(*next));assert(next);next->depth=((state_t *)s)->depth+1;f->allocated++;
    if(real) { assert(ac->regime!=ROLLOUT_ANTUMBRA);f->real[ac->regime]++; }else { assert(ac->regime!=ROLLOUT_UMBRA);f->sim[ac->regime]++; }
    return (rollout_step_result_t){next,ac->reward,strdup("{\"offline\":true}")};
}
static rollout_step_result_t execute(rollout_state_t s,rollout_action_t a,void *u) { return step(s,a,u,true); }
static rollout_step_result_t model(rollout_state_t s,rollout_action_t a,void *u) { return step(s,a,u,false); }
static rollout_regime_t regime(rollout_action_t a,void *u) { (void)u;return ((action_t *)a)->regime; }
static bool terminal(rollout_state_t s,void *u) { return ((state_t *)s)->depth>=((fixture_t *)u)->horizon; }
static void shash(rollout_state_t s,char *out,size_t cap,void *u) { (void)u;snprintf(out,cap,"%08x",((state_t *)s)->depth); }
static void akey(rollout_action_t a,char *out,size_t cap,void *u) { (void)u;snprintf(out,cap,"%08x",((action_t *)a)->id); }
static bool auth(rollout_state_t s,rollout_action_t a,rollout_regime_t r,bool realizing,void *u) { (void)s;(void)a;assert(r==ROLLOUT_UMBRA||(r==ROLLOUT_PENUMBRA&&realizing));fixture_t *f=u;if(f->cancel_on_auth) f->cancel=true;
    if(f->flip_on_auth) ((action_t *)a)->regime=ROLLOUT_ANTUMBRA;
    return !f->deny; }
static bool cancel(void *u) { return ((fixture_t *)u)->cancel; }
static double cost(rollout_action_t a,void *u) { (void)a;(void)u;return 2; }
static void *clone_state(rollout_state_t s,void *u) { state_t *p=malloc(sizeof(*p));assert(p);*p=*(state_t *)s;((fixture_t *)u)->allocated++;return p; }
static void release_state(rollout_state_t s,void *u) { ((fixture_t *)u)->freed++;free(s); }
static void *clone_action(rollout_action_t a,void *u) { action_t *p=malloc(sizeof(*p));assert(p);*p=*(action_t *)a;((fixture_t *)u)->actions_cloned++;return p; }
static void release_action(rollout_action_t a,void *u) { ((fixture_t *)u)->actions_freed++;free(a); }
static char *serialize(rollout_state_t s,void *u) { (void)u;char out[64];snprintf(out,sizeof(out),"{\"depth\":%d}",((state_t *)s)->depth);return strdup(out); }
static rollout_state_t deserialize(const char *json,void *u) { int n=json_get_int(json,"depth",-1);if(n<0)return NULL;state_t *s=malloc(sizeof(*s));assert(s);s->depth=n;((fixture_t *)u)->allocated++;return s; }
static void reset(fixture_t *f) { assert(f->allocated==f->freed);assert(f->actions_cloned==f->actions_freed);memset(f,0,sizeof(*f));f->horizon=3; }
int main(void) {
    fixture_t f={.horizon=3};state_t start={0};rollout_env_t env={.initial_state=&start,.actions_fn=list,.execute_fn=execute,.model_fn=model,
        .regime_fn=regime,.is_terminal_fn=terminal,.state_hash_fn=shash,.action_key_fn=akey,.action_cost_fn=cost,
        .clone_state_fn=clone_state,.free_state_fn=release_state,.clone_action_fn=clone_action,.free_action_fn=release_action,
        .user_data=&f,.authorize_fn=auth,.cancelled_fn=cancel,.serialize_state_fn=serialize,.deserialize_state_fn=deserialize,.cache_namespace="offline-fixture:operator:v1"};
    rollout_config_t c;rollout_config_defaults(&c);c.iterations=300;c.gamma=1;c.rollout_depth=3;
    char path[]="/private/tmp/dsco-rollout-test.XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);c.cache_path=path;
    rollout_result_t *r=rollout_search(&env,&c);assert(r);assert(r->status==ROLLOUT_STATUS_OK&&r->step_count==3);assert(r->total_reward==30);
    assert(r->needs_authority_count==3&&r->needs_realization_count==0);assert(r->real_executions==3&&r->simulated_executions==6);
    assert(f.real[ROLLOUT_PENUMBRA]==0&&f.real[ROLLOUT_ANTUMBRA]==0);assert(r->cache_hits>0&&r->cache_misses==9&&r->cache_errors==0);
    char *json=rollout_result_to_plan_json(r,"quotes \" and newline\n");assert(json&&json_is_valid_container(json));assert(strstr(json,"requires_authority\":true"));assert(!strstr(json,"0x"));free(json);rollout_result_free(r);reset(&f);
    /* Reopen the cache: restore all next states without any callback replay. */
    r=rollout_search(&env,&c);assert(r&&r->total_reward==30&&r->cache_misses==0&&r->real_executions==0&&r->simulated_executions==0&&r->cache_errors==0);rollout_result_free(r);reset(&f);
    /* Changing the principal/environment namespace prevents all stale hits. */
    env.cache_namespace="offline-fixture:different-principal:v1";r=rollout_search(&env,&c);assert(r&&r->cache_misses==9);rollout_result_free(r);reset(&f);
    env.cache_namespace="offline-fixture:operator:v1";
    f.deny=true;r=rollout_search(&env,&c);assert(r&&r->status==ROLLOUT_STATUS_DENIED&&r->real_executions==0);rollout_result_free(r);reset(&f);
    f.cancel=true;r=rollout_search(&env,&c);assert(r&&r->status==ROLLOUT_STATUS_CANCELLED&&r->real_executions==0&&r->simulated_executions==0);rollout_result_free(r);reset(&f);
    assert(!rollout_realize_step(&env,&start,&actions[2]).next_state&&errno==EPERM);
    assert(!rollout_realize_step(&env,&start,&actions[0]).next_state&&errno==EPERM);
    f.deny=true;assert(!rollout_realize_step(&env,&start,&actions[1]).next_state&&errno==EPERM);f.deny=false;
    f.cancel=true;assert(!rollout_realize_step(&env,&start,&actions[1]).next_state&&errno==ECANCELED);f.cancel=false;
    rollout_step_result_t realized=rollout_realize_step(&env,&start,&actions[1]);assert(realized.next_state&&f.real[1]==1);release_state(realized.next_state,&f);free(realized.observation_json);reset(&f);
    env.serialize_state_fn=NULL;assert(!rollout_search(&env,&c)&&errno==EINVAL);env.serialize_state_fn=serialize;
    c.gamma=2;assert(!rollout_search(&env,&c)&&errno==EINVAL);c.gamma=1;
    c.cache_path=NULL;f.horizon=0;r=rollout_search(&env,&c);assert(r&&r->step_count==0);rollout_result_free(r);reset(&f);
    c.cache_path=path;fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0);assert(write(fd,"broken trailing row\n",20)==20);close(fd);
    r=rollout_search(&env,&c);assert(r&&r->cache_errors>=1&&r->total_reward==30);rollout_result_free(r);reset(&f);
    /* Crash residue is truncated before any future append. */
    fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0);assert(write(fd,"{unfinished",11)==11);close(fd);
    r=rollout_search(&env,&c);assert(r&&r->cache_errors>=1&&r->cache_misses==0);rollout_result_free(r);reset(&f);
    r=rollout_search(&env,&c);assert(r&&r->cache_errors==1); /* prior complete bad row, no residual partial row */
    rollout_result_free(r);reset(&f);
    char link[128];snprintf(link,sizeof(link),"%s.link",path);assert(!symlink(path,link));c.cache_path=link;
    r=rollout_search(&env,&c);assert(r&&r->cache_errors==1&&r->cache_misses==9);rollout_result_free(r);reset(&f);unlink(link);
    c.cache_path=path;assert(!chmod(path,0644));r=rollout_search(&env,&c);assert(r&&r->cache_errors==1&&r->cache_misses==9);rollout_result_free(r);reset(&f);assert(!chmod(path,0600));
    c.cache_path=NULL;f.cancel_on_auth=true;r=rollout_search(&env,&c);assert(r&&r->status==ROLLOUT_STATUS_CANCELLED&&r->real_executions==0);rollout_result_free(r);reset(&f);
    f.flip_on_auth=true;r=rollout_search(&env,&c);assert(r&&r->status==ROLLOUT_STATUS_CALLBACK_ERROR&&r->real_executions==0);rollout_result_free(r);reset(&f);
    f.cancel_on_auth=true;assert(!rollout_realize_step(&env,&start,&actions[1]).next_state&&errno==ECANCELED);reset(&f);
    unlink(path);rollout_result_free(NULL);assert(!rollout_result_to_plan_json(NULL,"x"));
    puts("PASS rollout: optimum, memoization, durable warm start, scoped cache, regime isolation, denial, cancellation, lifecycle, corruption and plan JSON");return 0;
}
