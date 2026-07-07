/* swarm_cost_props.c — property harness for the swarm cost model.
 *
 * Reuses the proof-substrate architecture (splitmix64 determinism + FNV-1a
 * distinctness + mutation-kill gate) and points it at swarm_cost_model.h.
 *
 * Properties (each is also a Lean goal in swarm_cost_props.lean):
 *   P1 cache_monotone : cached prefix cost <= uncached          (∀ inputs)
 *   P2 cache_bound    : cached total <= 0.15×prefix + rest       (the -85% claim)
 *   P3 route_optimal  : router returns the true argmin over capable models
 *   P4 route_capable  : routed model always clears the capability bar
 *   P5 race_gate      : never race when waste >= value           (no wasteful race)
 *
 * Each property is falsifiable: a mutant model must make it FAIL (kill power).
 *
 * Build: cc -O2 -std=c11 -o swarm_cost_props swarm_cost_props.c
 * Run:   ./swarm_cost_props [N_per_property]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "swarm_cost_model.h"

/* ---- determinism: splitmix64 (same as prop_harness.c) ---- */
static uint64_t rng_state;
static uint64_t rng_next(void){
    uint64_t z=(rng_state+=0x9E3779B97F4A7C15ULL);
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;
    z=(z^(z>>27))*0x94D049BB133111EBULL;
    return z^(z>>31);
}
/* ---- distinctness: FNV-1a open-addressing set ---- */
#define HSET_BITS 22
#define HSET_SIZE (1u<<HSET_BITS)
static uint64_t *g_seen; static size_t g_distinct=0,g_dup=0;
static uint64_t fnv1a(const void *p,size_t n){
    const uint8_t *b=p; uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;} return h?h:1;
}
static bool seen_insert(uint64_t h){
    size_t m=HSET_SIZE-1,i=h&m;
    for(;;){ if(!g_seen[i]){g_seen[i]=h;g_distinct++;return true;}
             if(g_seen[i]==h){g_dup++;return false;} i=(i+1)&m; }
}

/* ---- a randomly generated swarm subtask ---- */
typedef struct { long prefix, uniq, out; int cap; } scase_t;
static scase_t gen_case(void){
    scase_t c;
    c.prefix = 500 + (long)(rng_next()%120000);
    c.uniq   = 50  + (long)(rng_next()%8000);
    c.out    = 50  + (long)(rng_next()%16000);
    c.cap    = 55  + (int)(rng_next()%41);   /* 55..95 */
    return c;
}

/* ---- MUTANTS: broken models a correct suite MUST catch ---- */
/* mutant router A: ignores capability bar (returns global cheapest).
   Kills P4 (capability guarantee) — routes below the bar. */
static const scm_model_t *route_MUTANT_ignorecap(long p,long u,long o,bool cached){
    const scm_model_t *best=NULL; double bc=1e18;
    for(int i=0;i<SCM_NMODELS;i++){
        double c=scm_turn_cost(&SCM_CATALOG[i],p,u,o,cached);
        if(c<bc){bc=c;best=&SCM_CATALOG[i];}
    }
    return best;
}
/* mutant router B: first CAPABLE model, not the cheapest capable.
   Kills P3 (optimality) without violating the capability bar. */
static const scm_model_t *route_MUTANT_firstcapable(int cap,long p,long u,long o,bool cached){
    (void)p;(void)u;(void)o;(void)cached;
    for(int i=0;i<SCM_NMODELS;i++)
        if(SCM_CATALOG[i].capability>=cap) return &SCM_CATALOG[i];
    return NULL;
}
/* mutant cost: caching accidentally *increases* prefix cost (sign flip) */
static double turn_MUTANT_badcache(const scm_model_t *m,long p,long u,long o,bool cached){
    double in_p=m->in_per_m/1e6,out_p=m->out_per_m/1e6;
    double prefix=cached ? p*in_p*(2.0-m->cache_read_mult) : p*in_p; /* WRONG */
    return prefix+u*in_p+o*out_p;
}

/* ---- property runners: return failures, track distinct inputs ---- */
static size_t P1_cache_monotone(size_t n, bool mutant){
    size_t fail=0;
    for(size_t k=0;k<n;k++){
        scase_t c=gen_case();
        seen_insert(fnv1a(&c,sizeof c));
        const scm_model_t *m=&SCM_CATALOG[rng_next()%SCM_NMODELS];
        double cached  = mutant? turn_MUTANT_badcache(m,c.prefix,c.uniq,c.out,true)
                               : scm_turn_cost(m,c.prefix,c.uniq,c.out,true);
        double uncached= mutant? turn_MUTANT_badcache(m,c.prefix,c.uniq,c.out,false)
                               : scm_turn_cost(m,c.prefix,c.uniq,c.out,false);
        if(!(cached <= uncached + 1e-12)) fail++;
    }
    return fail;
}
static size_t P2_cache_bound(size_t n, bool mutant){
    size_t fail=0;
    for(size_t k=0;k<n;k++){
        scase_t c=gen_case(); seen_insert(fnv1a(&c,sizeof c));
        const scm_model_t *m=&SCM_CATALOG[rng_next()%SCM_NMODELS];
        double in_p=m->in_per_m/1e6,out_p=m->out_per_m/1e6;
        double cached = mutant? turn_MUTANT_badcache(m,c.prefix,c.uniq,c.out,true)
                              : scm_turn_cost(m,c.prefix,c.uniq,c.out,true);
        /* bound: cached prefix portion <= 0.15 × full prefix price */
        double bound = 0.15*c.prefix*in_p + c.uniq*in_p + c.out*out_p;
        if(!(cached <= bound + 1e-12)) fail++;
    }
    return fail;
}
static size_t P3_route_optimal(size_t n, bool mutant){
    size_t fail=0;
    for(size_t k=0;k<n;k++){
        scase_t c=gen_case(); seen_insert(fnv1a(&c,sizeof c));
        const scm_model_t *r = mutant
            ? route_MUTANT_firstcapable(c.cap,c.prefix,c.uniq,c.out,true)
            : scm_route_cheapest(c.cap,c.prefix,c.uniq,c.out,true);
        if(!r) continue; /* no capable model: vacuous */
        /* verify r is the true argmin among CAPABLE models */
        double rc=scm_turn_cost(r,c.prefix,c.uniq,c.out,true), best=1e18;
        for(int i=0;i<SCM_NMODELS;i++){
            if(SCM_CATALOG[i].capability < c.cap) continue;
            double cc=scm_turn_cost(&SCM_CATALOG[i],c.prefix,c.uniq,c.out,true);
            if(cc<best) best=cc;
        }
        if(!(rc <= best + 1e-9)) fail++;
    }
    return fail;
}
static size_t P4_route_capable(size_t n, bool mutant){
    size_t fail=0;
    for(size_t k=0;k<n;k++){
        scase_t c=gen_case(); seen_insert(fnv1a(&c,sizeof c));
        const scm_model_t *r = mutant
            ? route_MUTANT_ignorecap(c.prefix,c.uniq,c.out,true)
            : scm_route_cheapest(c.cap,c.prefix,c.uniq,c.out,true);
        if(!r) continue;
        if(r->capability < c.cap) fail++;   /* routed below the bar = bug */
    }
    return fail;
}
static size_t P5_race_gate(size_t n, bool mutant){
    size_t fail=0;
    for(size_t k=0;k<n;k++){
        double p_fail=(rng_next()%1000)/1000.0;
        double latv =(rng_next()%500)/100.0;
        double lane =(rng_next()%200)/100.0 + 0.01;
        int lanes = 1 + (int)(rng_next()%6);
        scase_t seed={.prefix=(long)(latv*1000),.uniq=(long)(lane*1000),
                      .out=lanes,.cap=(int)(p_fail*100)};
        seen_insert(fnv1a(&seed,sizeof seed));
        bool race = mutant ? (lanes>1) /* mutant: always race */
                           : scm_should_race(p_fail,latv,lane,lanes);
        double value = latv*(1.0-p_fail), waste=(lanes-1)*lane;
        /* property: if we chose to race, value MUST exceed waste */
        if(race && !(value > waste)) fail++;
    }
    return fail;
}

typedef size_t (*prop_fn)(size_t,bool);
typedef struct { const char *name; prop_fn f; } prop_t;

int main(int argc,char**argv){
    size_t N=(argc>1)?strtoull(argv[1],0,10):100000;
    g_seen=calloc(HSET_SIZE,sizeof(uint64_t));
    if(!g_seen){fprintf(stderr,"oom\n");return 2;}

    prop_t props[]={
        {"P1 cache_monotone", P1_cache_monotone},
        {"P2 cache_bound",    P2_cache_bound},
        {"P3 route_optimal",  P3_route_optimal},
        {"P4 route_capable",  P4_route_capable},
        {"P5 race_gate",      P5_race_gate},
    };
    int NP=(int)(sizeof(props)/sizeof(props[0]));

    printf("=== swarm-cost property harness ===\n");
    printf("properties: %d   executions/property: %zu   total: %zu\n\n",
           NP, N, (size_t)NP*N);

    bool all_ok=true; size_t total_exec=0;
    for(int i=0;i<NP;i++){
        rng_state=0xC05700D + (uint64_t)i*0x100;  /* deterministic per-prop seed */
        size_t d0=g_distinct;
        size_t fref=props[i].f(N,false);          /* correct impl: must pass */
        size_t dref=g_distinct-d0;
        rng_state=0xC05700D + (uint64_t)i*0x100;   /* same corpus for mutant */
        size_t fmut=props[i].f(N,true);            /* mutant: must fail */
        total_exec += N;
        bool sound = (fref==0), kills = (fmut>0);
        bool ok = sound && kills;
        all_ok &= ok;
        printf("[%-18s] exec=%zu distinct=%zu(%.0f%%) fail_ref=%zu kill=%zu(%.1f%%) %s\n",
               props[i].name, N, dref, 100.0*dref/N, fref, fmut,
               100.0*fmut/N, ok?"OK":"BROKEN");
    }

    printf("\n=== verdict ===\n");
    printf("total real tests : %zu\n", total_exec);
    printf("distinct inputs  : %zu\n", g_distinct);
    printf("all sound+killing: %s\n", all_ok?"yes":"NO");
    printf("status           : %s\n", all_ok?"REAL (not theater)":"BROKEN");
    free(g_seen);
    return all_ok?0:1;
}
