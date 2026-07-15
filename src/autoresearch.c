#include "autoresearch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Keep this module deliberately narrow. Karpathy/autoresearch's useful core is
 * a fixed budget, a fixed evaluator, isolated one-file edits, and retaining
 * only measured improvements. The proposing agent is supplied by the command
 * so DSCO remains provider-agnostic and never grants an ambient shell. */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s autoresearch start --name NAME --target FILE --eval COMMAND --propose COMMAND [--iterations N] [--workdir DIR]\n"
        "  %s autoresearch worker --manifest PATH\n"
        "  %s autoresearch status --name NAME [--workdir DIR]\n"
        "  %s autoresearch stop --name NAME [--workdir DIR]\n"
        "\n"
        "Commands execute via /bin/sh -c. The proposer must edit only --target;\n"
        "the evaluator's stdout must end with a numeric score (higher is better).\n",
        prog, prog, prog, prog);
}

static const char *opt(int argc, char **argv, const char *name) {
    for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
static long opt_long(int argc, char **argv, const char *name, long fallback) {
    const char *v = opt(argc, argv, name); char *end = NULL;
    long n = v ? strtol(v, &end, 10) : fallback;
    return (!v || !end || *end || n < 1 || n > 1000000) ? fallback : n;
}
static int mkdir_p(const char *path) {
    char p[PATH_MAX]; size_t n = strlen(path);
    if (!n || n >= sizeof(p)) return -1;
    memcpy(p, path, n + 1);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(p, 0700) && errno != EEXIST) return -1; *s = '/'; }
    return mkdir(p, 0700) && errno != EEXIST ? -1 : 0;
}
static bool safe_name(const char *s) {
    if (!s || !*s || strlen(s) > 80) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}
static int command_score(const char *cmd, double *score) {
    char tmp[] = "/tmp/dsco-autoresearch-score.XXXXXX"; int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    pid_t p = fork();
    if (!p) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
    close(fd); int status = 0; if (p < 0 || waitpid(p, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) { unlink(tmp); return -1; }
    FILE *f = fopen(tmp, "r"); char line[512], last[512] = "";
    if (!f) { unlink(tmp); return -1; }
    while (fgets(line, sizeof(line), f)) snprintf(last, sizeof(last), "%s", line);
    fclose(f); unlink(tmp); char *end = NULL; errno = 0; double v = strtod(last, &end);
    while (end && (*end == ' ' || *end == '\n' || *end == '\t')) end++;
    if (errno || end == last || (end && *end) || !isfinite(v)) return -1;
    *score = v; return 0;
}
static void shell_quote(FILE *f, const char *s) { fputc('\'', f); for (; *s; s++) { if (*s == '\'') fputs("'\\''", f); else fputc(*s, f); } fputc('\'', f); }
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192]; size_t n; int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) != 0) if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    if (ferror(in) || fclose(out)) rc = -1;
    fclose(in); return rc;
}
static int worker(const char *manifest) {
    FILE *f = fopen(manifest, "r"); char target[PATH_MAX] = "", eval[8192] = "", propose[8192] = "", run[PATH_MAX] = ""; long max = 0;
    if (!f) { perror("autoresearch manifest"); return 1; }
    char line[9000]; while (fgets(line, sizeof(line), f)) { char *v = strchr(line, '='); if (!v) continue; *v++ = 0; v[strcspn(v, "\n")] = 0;
        if (!strcmp(line,"target")) snprintf(target,sizeof(target),"%s",v); else if (!strcmp(line,"eval")) snprintf(eval,sizeof(eval),"%s",v); else if (!strcmp(line,"propose")) snprintf(propose,sizeof(propose),"%s",v); else if (!strcmp(line,"run")) snprintf(run,sizeof(run),"%s",v); else if (!strcmp(line,"iterations")) max=strtol(v,NULL,10); }
    fclose(f); if (!target[0] || !eval[0] || !propose[0] || !run[0] || max < 1 || max > 1000000) return 1;
    char log[PATH_MAX], state[PATH_MAX], best[PATH_MAX], candidate[PATH_MAX], control[PATH_MAX];
    snprintf(log,sizeof(log),"%s/experiments.tsv",run); snprintf(state,sizeof(state),"%s/status",run); snprintf(best,sizeof(best),"%s/best",run); snprintf(candidate,sizeof(candidate),"%s/candidate",run); snprintf(control,sizeof(control),"%s/stop",run);
    double baseline; if (command_score(eval,&baseline)) { fprintf(stderr,"autoresearch: baseline evaluator failed or did not end in numeric score\n"); return 1; }
    FILE *lf=fopen(log,"a"); if (!lf) return 1;
    if (fseek(lf, 0, SEEK_END) == 0 && ftell(lf) == 0) fprintf(lf,"iteration\tscore\tbaseline\tdecision\n");
    fflush(lf);
    FILE *sf=fopen(state,"w"); if(sf){fprintf(sf,"running pid=%d baseline=%.17g\n",getpid(),baseline);fclose(sf);}
    for (long i=1;i<=max && access(control,F_OK);i++) {
        if (copy_file(target,best)) { fprintf(lf,"%ld\t\t%.17g\terror:backup\n",i,baseline); break; }
        char cmd[18000]; snprintf(cmd,sizeof(cmd),"DSCO_AUTORESEARCH_TARGET=");
        FILE *cf=tmpfile(); if (!cf) break; shell_quote(cf,target); fputs(" DSCO_AUTORESEARCH_ITERATION=",cf); fprintf(cf,"%ld ",i); shell_quote(cf,propose); rewind(cf); size_t nr=fread(cmd+strlen(cmd),1,sizeof(cmd)-strlen(cmd)-1,cf); cmd[strlen(cmd)+nr]=0; fclose(cf);
        int ps=system(cmd); if (ps != 0) { unlink(target); if (rename(best,target)) { fprintf(lf,"%ld\t\t%.17g\tROLLBACK_FAILED\n",i,baseline); break; } fprintf(lf,"%ld\t\t%.17g\tproposer_failed_reverted\n",i,baseline); fflush(lf); continue; }
        if (access(target, R_OK)) { if (rename(best,target)) fprintf(lf,"%ld\t\t%.17g\tROLLBACK_FAILED\n",i,baseline); else fprintf(lf,"%ld\t\t%.17g\ttarget_missing_reverted\n",i,baseline); fflush(lf); break; }
        double score; int er=command_score(eval,&score); bool win=!er && score>baseline;
        if (win) { baseline=score; fprintf(lf,"%ld\t%.17g\t%.17g\tkeep\n",i,score,baseline); }
        else { unlink(target); if (rename(best,target)) { fprintf(lf,"%ld\t\t%.17g\tROLLBACK_FAILED\n",i,baseline); break; } fprintf(lf,"%ld\t%s\t%.17g\trevert\n",i,er?"eval_failed":"",baseline); }
        fflush(lf);
    }
    fclose(lf); unlink(best); unlink(candidate); sf=fopen(state,"w"); if(sf){fprintf(sf,"completed pid=%d best=%.17g\n",getpid(),baseline);fclose(sf);} return 0;
}
int autoresearch_cli(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "--help") || !strcmp(argv[2], "-h")) {
        usage(argv[0]); return argc < 3 ? 1 : 0;
    }
    if (!strcmp(argv[2],"worker")) { const char *m=opt(argc,argv,"--manifest"); return m?worker(m):1; }
    const char *name=opt(argc,argv,"--name"), *root=opt(argc,argv,"--workdir"); char base[PATH_MAX], run[PATH_MAX];
    if (!root) { const char *home=getenv("HOME"); snprintf(base,sizeof(base),"%s/.dsco/autoresearch",home?home:"."); } else snprintf(base,sizeof(base),"%s",root);
    if (!safe_name(name)) { fprintf(stderr,"autoresearch: --name must be alphanumeric, _ or -\n"); return 1; }
    snprintf(run,sizeof(run),"%s/%s",base,name);
    if (!strcmp(argv[2],"status")) { char p[PATH_MAX]; snprintf(p,sizeof(p),"%s/status",run); FILE*f=fopen(p,"r"); if(!f){perror(p);return 1;} char b[512];while(fgets(b,sizeof(b),f))fputs(b,stdout);fclose(f);return 0; }
    if (!strcmp(argv[2],"stop")) { if(mkdir_p(run)){perror(run);return 1;} char p[PATH_MAX];snprintf(p,sizeof(p),"%s/stop",run);int fd=open(p,O_CREAT|O_WRONLY,0600);if(fd<0){perror(p);return 1;}close(fd);puts("stop requested");return 0; }
    if (strcmp(argv[2],"start")) { usage(argv[0]); return 1; }
    const char *target=opt(argc,argv,"--target"), *eval=opt(argc,argv,"--eval"), *propose=opt(argc,argv,"--propose");
    if (!target||!eval||!propose||access(target,R_OK)) { fprintf(stderr,"autoresearch: start requires readable --target, --eval and --propose\n"); return 1; }
    if (mkdir_p(run)) { perror(run); return 1; } char manifest[PATH_MAX], stop[PATH_MAX], status[PATH_MAX]; snprintf(manifest,sizeof(manifest),"%s/manifest",run); snprintf(stop,sizeof(stop),"%s/stop",run); snprintf(status,sizeof(status),"%s/status",run); unlink(stop);
    /* Do not silently launch two workers against one target/run record. */
    FILE *old_status = fopen(status, "r");
    if (old_status) { char state[128] = ""; fgets(state, sizeof(state), old_status); fclose(old_status);
        long old_pid = 0;
        if ((sscanf(state, "running pid=%ld", &old_pid) == 1 ||
             sscanf(state, "starting pid=%ld", &old_pid) == 1) &&
            old_pid > 0 && kill((pid_t)old_pid, 0) == 0) {
            fprintf(stderr, "autoresearch: run '%s' is already active (pid %ld)\n", name, old_pid); return 1;
        }
    }
    FILE*f=fopen(manifest,"w"); if(!f)return 1; fprintf(f,"target=%s\neval=%s\npropose=%s\nrun=%s\niterations=%ld\n",target,eval,propose,run,opt_long(argc,argv,"--iterations",10)); fclose(f);
    pid_t p=fork(); if(p<0)return 1; if(!p){setsid(); int fd=open("/dev/null",O_RDONLY);dup2(fd,0); char lp[PATH_MAX];snprintf(lp,sizeof(lp),"%s/worker.log",run);int lf=open(lp,O_CREAT|O_WRONLY|O_APPEND,0600);dup2(lf,1);dup2(lf,2); execl(argv[0],argv[0],"autoresearch","worker","--manifest",manifest,(char*)NULL); _exit(127);}
    FILE *sf; char sp[PATH_MAX];snprintf(sp,sizeof(sp),"%s/status",run);sf=fopen(sp,"w");if(sf){fprintf(sf,"starting pid=%d\n",p);fclose(sf);} printf("{\"ok\":true,\"name\":\"%s\",\"pid\":%d,\"run\":\"%s\"}\n",name,p,run); return 0;
}
