#define _DARWIN_C_SOURCE 1
#include "native_app.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t disk_mu=PTHREAD_MUTEX_INITIALIZER;
static int dir_fd=-1,lock_fd=-1;
static char basename_[NAME_MAX+1];
static uint64_t saved_revision;
static unsigned temp_serial;
static bool error(char *out,size_t cap,const char *s) {if(out&&cap)snprintf(out,cap,"%s",s);return false;}
static bool owned_file(int fd) {
    struct stat st;return fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && st.st_nlink==1 && !(st.st_mode&0077);
}
static void close_locked(void) {
    if(lock_fd>=0)close(lock_fd);if(dir_fd>=0)close(dir_fd);lock_fd=dir_fd=-1;basename_[0]=0;saved_revision=0;
}
bool native_app_store_open(const char *path,char *out,size_t cap) {
    if(out&&cap)out[0]=0;
    const char *deny=getenv("DSCO_ALLOW_WRITE"),*deny_read=getenv("DSCO_ALLOW_READ");
    if((deny&&!strcmp(deny,"0"))||(deny_read&&!strcmp(deny_read,"0")))return error(out,cap,"workbench_state_access_disabled");
    if(!path||path[0]!='/'||strlen(path)>=PATH_MAX)return error(out,cap,"absolute_state_path_required");
    char parent[PATH_MAX];strcpy(parent,path);char *slash=strrchr(parent,'/');
    const char *name=path+(slash-parent)+1;
    if(!*name||strlen(name)>NAME_MAX-32||!strcmp(name,".")||!strcmp(name,".."))return error(out,cap,"invalid_state_name");
    if(slash==parent)slash[1]=0;else *slash=0;
    pthread_mutex_lock(&disk_mu);
    if(dir_fd>=0){pthread_mutex_unlock(&disk_mu);return error(out,cap,"state_store_already_open");}
    bool ok=false;
    dir_fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(dir_fd<0||fstat(dir_fd,&st)!=0||st.st_uid!=geteuid()||(st.st_mode&0022)){error(out,cap,"state_parent_not_owner_controlled");goto done;}
    strcpy(basename_,name);char lockname[NAME_MAX+1];snprintf(lockname,sizeof(lockname),"%s.lock",name);
    lock_fd=openat(dir_fd,lockname,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock_fd<0||!owned_file(lock_fd)||flock(lock_fd,LOCK_EX|LOCK_NB)!=0){error(out,cap,"state_store_locked_or_unsafe");goto done;}
    int fd=openat(dir_fd,basename_,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){if(errno==ENOENT){native_apps_enable();ok=true;}else error(out,cap,"state_read_failed");goto done;}
    if(!owned_file(fd)||fstat(fd,&st)!=0||st.st_size<=0||st.st_size>2*1024*1024){close(fd);error(out,cap,"state_file_unsafe_or_oversized");goto done;}
    size_t len=(size_t)st.st_size,n=0;char *json=malloc(len+1);
    if(!json){close(fd);error(out,cap,"state_allocation_failed");goto done;}
    while(n<len){ssize_t r=read(fd,json+n,len-n);if(r<0&&errno==EINTR)continue;if(r<=0)break;n+=(size_t)r;}
    char extra;ssize_t tail=read(fd,&extra,1);close(fd);json[n]=0;
    ok=n==len&&tail==0&&native_apps_import(json,n);free(json);
    if(!ok)error(out,cap,"state_invalid_preserved");
done:
    if(!ok)close_locked();
    pthread_mutex_unlock(&disk_mu);return ok;
}
bool native_app_store_checkpoint(char *out,size_t cap) {
    if(out&&cap)out[0]=0;
    pthread_mutex_lock(&disk_mu);
    if(dir_fd<0){pthread_mutex_unlock(&disk_mu);return error(out,cap,"state_store_not_open");}
    uint64_t revision=0;char *json=native_apps_export(&revision);
    if(!json){pthread_mutex_unlock(&disk_mu);return error(out,cap,"state_encode_failed");}
    /* Reject unsafe replacements; renameat never follows the destination. */
    struct stat st;int rc=fstatat(dir_fd,basename_,&st,AT_SYMLINK_NOFOLLOW);
    bool ok=rc==0 ? S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&!(st.st_mode&0077) : errno==ENOENT;
    if(ok && rc==0 && revision==saved_revision){free(json);pthread_mutex_unlock(&disk_mu);return true;}
    char tmp[NAME_MAX+1];snprintf(tmp,sizeof(tmp),".%s.%ld.%u.tmp",basename_,(long)getpid(),++temp_serial);
    int fd=ok?openat(dir_fd,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600):-1;
    bool created=fd>=0;ok=created;size_t n=0,len=strlen(json);
    while(ok&&n<len){ssize_t w=write(fd,json+n,len-n);if(w<0&&errno==EINTR)continue;if(w<=0){ok=false;break;}n+=(size_t)w;}
    if(ok&&fsync(fd)!=0)ok=false;
    if(fd>=0&&close(fd)!=0)ok=false;
    if(ok&&renameat(dir_fd,tmp,dir_fd,basename_)!=0)ok=false;
    if(ok&&fsync(dir_fd)!=0)ok=false;
    if(!ok){if(created)unlinkat(dir_fd,tmp,0);error(out,cap,"state_checkpoint_failed_memory_retained");}
    else saved_revision=revision;
    free(json);pthread_mutex_unlock(&disk_mu);return ok;
}
void native_app_store_close(void) {pthread_mutex_lock(&disk_mu);close_locked();pthread_mutex_unlock(&disk_mu);}
