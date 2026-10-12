#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "provider_admission.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ADMISSION_MAX_WAITERS 512
#define ADMISSION_MAGIC 0x44534332u
#define ADMISSION_AGE_MS 5000

typedef struct { unsigned magic, foreground; uint64_t session, enqueued_ms; } ticket_t;
typedef struct { unsigned magic, foreground_streak; uint64_t last_session; } state_t;
static uint64_t clock_ms(void) {
    struct timespec t; if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static uint64_t hash(const char *s) {
    uint64_t h=1469598103934665603ULL;
    while (*s) h=(h^(unsigned char)*s++)*1099511628211ULL;
    return h;
}
static const char *session(void) {
    const char *s=getenv("DSCO_ADMISSION_SESSION");
    if (!s || !*s) s=getenv("DSCO_PARENT_INSTANCE_ID");
    if (!s || !*s) s=getenv("DSCO_INSTANCE_ID");
    return s;
}
void provider_admission_inherit_session(void) {
    const char *s=session();
    if (s && *s) { char copy[256]; snprintf(copy,sizeof(copy),"%s",s); setenv("DSCO_ADMISSION_SESSION",copy,1); }
    else { char id[64]; snprintf(id,sizeof(id),"process-%ld",(long)getppid()); setenv("DSCO_ADMISSION_SESSION",id,1); }
}
static bool owned(int fd) {
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)) { errno=EACCES; return false; }
    return true;
}
static void unlock(provider_admission_t *q) {
    if (q->locked) { flock(q->state_fd,LOCK_UN); q->locked=false; }
}
bool provider_admission_join(provider_admission_t *q,const char *base) {
    if (q->ticket_fd>=0) return true;
    if (snprintf(q->directory,sizeof(q->directory),"%s.waiters",base)>=(int)sizeof(q->directory)) { errno=ENAMETOOLONG;return false; }
    if (mkdir(q->directory,0700) && errno!=EEXIST) return false;
    struct stat st;
    if (lstat(q->directory,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&077)) {errno=EACCES;return false;}
    char path[1500];snprintf(path,sizeof(path),"%s/state",q->directory);
    q->state_fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if (q->state_fd<0) return false;
    if (!owned(q->state_fd)) goto fail;
    if (flock(q->state_fd,LOCK_EX|LOCK_NB)) goto fail;
    q->locked=true;
    state_t state={0};ssize_t n=pread(q->state_fd,&state,sizeof(state),0);
    if (n==0) {state.magic=ADMISSION_MAGIC;if(pwrite(q->state_fd,&state,sizeof(state),0)!=(ssize_t)sizeof(state))goto fail;}
    else if(n!=(ssize_t)sizeof(state)||state.magic!=ADMISSION_MAGIC) {errno=EPROTO;goto fail;}
    const char *s=session();char fallback[64];snprintf(fallback,sizeof(fallback),"process-%ld",(long)getpid());
    q->session=hash(s&&*s?s:fallback);
    const char *worker=getenv("DSCO_WORKER"),*depth=getenv("DSCO_SWARM_DEPTH");
    q->foreground=!(worker&&*worker&&strcmp(worker,"0")) && !(depth&&atoi(depth)>0);
    snprintf(q->ticket,sizeof(q->ticket),"%s/t-XXXXXX",q->directory);
    q->ticket_fd=mkstemp(q->ticket);
    if(q->ticket_fd<0)goto fail;
    fcntl(q->ticket_fd,F_SETFD,FD_CLOEXEC);
    if(flock(q->ticket_fd,LOCK_EX|LOCK_NB))goto fail;
    ticket_t row={ADMISSION_MAGIC,q->foreground,q->session,clock_ms()};
    if(pwrite(q->ticket_fd,&row,sizeof(row),0)!=(ssize_t)sizeof(row))goto fail;
    unlock(q);return true;
fail: {int error=errno;provider_admission_leave(q);errno=error;return false;}
}
int provider_admission_choose(provider_admission_t *q, unsigned window) {
    if(flock(q->state_fd,LOCK_EX|LOCK_NB))return errno==EWOULDBLOCK||errno==EAGAIN?2:-1;
    q->locked=true;state_t state;
    if(pread(q->state_fd,&state,sizeof(state),0)!=(ssize_t)sizeof(state)||state.magic!=ADMISSION_MAGIC){errno=EPROTO;unlock(q);return -1;}
    DIR *dir=opendir(q->directory);if(!dir){unlock(q);return -1;}
    ticket_t rows[ADMISSION_MAX_WAITERS];char names[ADMISSION_MAX_WAITERS][64];int count=0,error=0;
    struct dirent *e;
    while((e=readdir(dir))) {
        if(strncmp(e->d_name,"t-",2))continue;
        if(strlen(e->d_name)>=sizeof(names[0])){error=EPROTO;break;}
        char path[1500];snprintf(path,sizeof(path),"%s/%s",q->directory,e->d_name);
        bool mine=!strcmp(path,q->ticket);int fd=mine?q->ticket_fd:open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){if(errno==ENOENT)continue;error=errno;break;}
        if(!owned(fd)){error=errno;if(!mine)close(fd);break;}
        if(!mine && flock(fd,LOCK_EX|LOCK_NB)==0){unlink(path);close(fd);continue;}
        if(!mine && errno!=EWOULDBLOCK && errno!=EAGAIN){error=errno;close(fd);break;}
        ticket_t row;ssize_t n=pread(fd,&row,sizeof(row),0);if(!mine)close(fd);
        if(n!=(ssize_t)sizeof(row)||row.magic!=ADMISSION_MAGIC||row.foreground>1){error=EPROTO;break;}
        if(count==ADMISSION_MAX_WAITERS){error=ENOSPC;break;}
        rows[count]=row;snprintf(names[count],sizeof(names[count]),"%s",e->d_name);count++;
    }
    closedir(dir);
    if(error||!count){errno=error?error:EPROTO;unlock(q);return -1;}
    /* Rank a cohort using the same foreground/session policy as the original
     * single-winner scheduler. This preserves the policy order while allowing
     * enough independent contenders to fill a multi-permit downstream lane in
     * one polling generation. The metadata lock is still released before any
     * provider transport begins. */
    if(window<1)window=1;
    if(window>ADMISSION_MAX_WAITERS)window=ADMISSION_MAX_WAITERS;
    bool ranked[ADMISSION_MAX_WAITERS]={0};bool mine_selected=false;state_t simulated=state;
    uint64_t now=clock_ms();
    for(unsigned rank=0;rank<window&&rank<(unsigned)count;rank++) {
        int aged=-1;bool background=false,foreground=false;
        for(int i=0;i<count;i++) {
            if(ranked[i])continue;
            background|=!rows[i].foreground;foreground|=rows[i].foreground;
            if(now>=rows[i].enqueued_ms && now-rows[i].enqueued_ms>=ADMISSION_AGE_MS &&
               (aged<0||rows[i].enqueued_ms<rows[aged].enqueued_ms ||
                (rows[i].enqueued_ms==rows[aged].enqueued_ms&&strcmp(names[i],names[aged])<0)))aged=i;
        }
        int selected=aged;
        if(selected<0) {
            bool pick_fg=foreground && (!background||simulated.foreground_streak<3);
            bool alternative=false;
            for(int i=0;i<count;i++)if(!ranked[i]&&rows[i].foreground==pick_fg&&rows[i].session!=simulated.last_session)alternative=true;
            for(int i=0;i<count;i++) {
                if(ranked[i]||rows[i].foreground!=pick_fg || (alternative&&rows[i].session==simulated.last_session))continue;
                if(selected<0||rows[i].enqueued_ms<rows[selected].enqueued_ms || (rows[i].enqueued_ms==rows[selected].enqueued_ms&&strcmp(names[i],names[selected])<0))selected=i;
            }
        }
        if(selected<0)break;
        ranked[selected]=true;
        simulated.last_session=rows[selected].session;
        simulated.foreground_streak=rows[selected].foreground?(simulated.foreground_streak<3?simulated.foreground_streak+1:3):0;
        const char *mine=strrchr(q->ticket,'/');
        if(mine&&!strcmp(names[selected],mine+1)){mine_selected=true;break;}
    }
    if(mine_selected)return 1;
    unlock(q);errno=0;return 0;
}
void provider_admission_decide(provider_admission_t *q,bool admitted) {
    if(admitted&&q->locked) {
        state_t s={0};
        if(pread(q->state_fd,&s,sizeof(s),0)==(ssize_t)sizeof(s)&&s.magic==ADMISSION_MAGIC) {
            s.last_session=q->session;
            s.foreground_streak=q->foreground?(s.foreground_streak<3?s.foreground_streak+1:3):0;
            /* Failure affects subsequent admission (invalid metadata fails
             * closed), never replays a request already admitted. */
            (void)pwrite(q->state_fd,&s,sizeof(s),0);
        }
    }
    unlock(q);
    if(admitted)provider_admission_leave(q);
}
void provider_admission_leave(provider_admission_t *q) {
    if(!q)return;
    unlock(q);
    if(q->ticket_fd>=0){if(q->ticket[0])unlink(q->ticket);close(q->ticket_fd);}
    if(q->state_fd>=0)close(q->state_fd);
    q->state_fd=q->ticket_fd=-1;q->ticket[0]='\0';
}
