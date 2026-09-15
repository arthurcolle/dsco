#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "buffer_store.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_LIMIT (1024u * 1024u)
#define BUFFER_COUNT 256
#define REQUEST_COUNT 4096
typedef struct {
    char id[37], name[161], kind[12], source[PATH_MAX], source_revision[65];
    bool closed, sensitive;
} buffer_t;
typedef struct {
    char base[PATH_MAX], dir[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db;
    int lock;
    bool missing;
    const char *error;
    char detail[256];
} store_t;
typedef struct { char *data; size_t size; char revision[65]; struct stat stat; } content_t;
typedef struct { yyjson_doc *doc; yyjson_val *root; const char *action, *workspace; bool mutation; } request_t;
static pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *const fields[] = {"action","workspace","buffer_id","name","new_name","kind","content",
    "source_path","path","expected_revision","expected_source_revision","request_id","sensitive","offset","max_bytes",NULL};

static bool same(const char *a, const char *b) { return a && b && !strcmp(a,b); }
static bool problem(store_t *s, const char *code, const char *detail) {
    s->error = code; snprintf(s->detail, sizeof(s->detail), "%s", detail ? detail : ""); return false;
}
static bool output_error(char *out, size_t cap, const char *code, const char *detail) {
    if (!out || !cap) return false;
    jbuf_t b; jbuf_init(&b, 256); jbuf_append(&b,"{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b,code); jbuf_append(&b,",\"detail\":"); jbuf_append_json_str(&b,detail?detail:""); jbuf_append_char(&b,'}');
    if (b.len < cap) memcpy(out,b.data,b.len+1); else if(cap>=3)memcpy(out,"{}",3); else if(cap==2)memcpy(out,"0",2); else out[0]=0;
    jbuf_free(&b); return false;
}
static bool utf8(const char *text, size_t size) {
    const unsigned char *p = (const unsigned char *)text;
    for (size_t i=0;i<size;) {
        unsigned c=p[i++], cp; int extra;
        if (!c) return false;
        if (c<0x80) continue;
        if (c>=0xc2 && c<=0xdf) { cp=c&31; extra=1; }
        else if (c>=0xe0 && c<=0xef) { cp=c&15; extra=2; }
        else if (c>=0xf0 && c<=0xf4) { cp=c&7; extra=3; }
        else return false;
        if (i+(size_t)extra>size) return false;
        for (int j=0;j<extra;j++) { if ((p[i]&0xc0)!=0x80) return false; cp=(cp<<6)|(p[i++]&63); }
        if ((extra==1 && cp<0x80)||(extra==2 && cp<0x800)||(extra==3 && cp<0x10000)||
            (cp>=0xd800&&cp<=0xdfff)||cp>0x10ffff) return false;
    }
    return true;
}
static bool safe_workspace(const char *p) {
    if (!p || !*p || strlen(p)>63) return false;
    for (;*p;p++) if (!(isalnum((unsigned char)*p)||*p=='_'||*p=='-')) return false;
    return true;
}
static bool valid_name(const char *p) {
    if (!p || !*p || strlen(p)>160 || !utf8(p,strlen(p))) return false;
    for (;*p;p++) if ((unsigned char)*p<32 || (unsigned char)*p==127) return false;
    return true;
}
static bool hash_valid(const char *p) {
    if (!p || strlen(p)!=64) return false;
    for (;*p;p++) if (!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))) return false;
    return true;
}
static bool uuid_valid(const char *p) {
    if (!p || strlen(p)!=36) return false;
    for(int i=0;i<36;i++) {
        if(i==8||i==13||i==18||i==23) { if(p[i]!='-')return false; }
        else if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f'))) return false;
    }
    return true;
}
static bool new_id(char id[37]) {
    unsigned char bytes[16]; if(!crypto_random_bytes(bytes,sizeof(bytes)))return false;
    bytes[6]=(bytes[6]&15)|64; bytes[8]=(bytes[8]&63)|128;
    snprintf(id,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]); return true;
}
static const char *str(request_t *r,const char *key) { return yyjson_get_str(yyjson_obj_get(r->root,key)); }
static bool parse_request(const char *input,request_t *r,store_t *s) {
    memset(r,0,sizeof(*r));
    if(!input || strlen(input)>BUFFER_LIMIT*6u+32768u)return problem(s,"invalid_input","input exceeds request limit");
    r->doc=yyjson_read(input,strlen(input),0); r->root=r->doc?yyjson_doc_get_root(r->doc):NULL;
    if(!yyjson_is_obj(r->root))return problem(s,"invalid_input","expected a JSON object");
    unsigned seen=0; size_t i,n; yyjson_val *key,*value;
    yyjson_obj_foreach(r->root,i,n,key,value) {
        const char *name=yyjson_get_str(key); int field=-1;
        if(strlen(name)!=yyjson_get_len(key))return problem(s,"invalid_input","NUL in field name");
        for(int j=0;fields[j];j++)if(same(name,fields[j]))field=j;
        if(field<0 || (seen&(1u<<field)))return problem(s,"invalid_input","unknown or duplicate field");
        seen|=1u<<field;
        if(same(name,"sensitive")) { if(!yyjson_is_bool(value))return problem(s,"invalid_input","sensitive must be boolean"); }
        else if(same(name,"offset")||same(name,"max_bytes")) {
            long long v=yyjson_get_sint(value);
            if(!yyjson_is_int(value)||v<0||v>(same(name,"offset")?BUFFER_LIMIT:32768)|| (same(name,"max_bytes")&&!v))
                return problem(s,"invalid_input","invalid byte range");
        } else {
            const char *v=yyjson_get_str(value); size_t len=yyjson_get_len(value);
            size_t max=same(name,"content")?BUFFER_LIMIT:(same(name,"path")||same(name,"source_path"))?PATH_MAX-1:160;
            if(!v||len>max||strlen(v)!=len||!utf8(v,len))return problem(s,"invalid_input","invalid, oversized or NUL-containing string");
        }
    }
    r->action=str(r,"action"); r->workspace=str(r,"workspace"); if(!r->workspace)r->workspace="main";
    if(!safe_workspace(r->workspace)||!r->action)return problem(s,"invalid_input","action and valid workspace required");
    static const char *const actions[]={"create","list","inspect","read","write","append","rename","fork","close","reopen","save",NULL};
    bool known=false; for(int i=0;actions[i];i++)if(same(r->action,actions[i]))known=true;
    if(!known)return problem(s,"invalid_action","unknown buffer action");
    r->mutation=!(same(r->action,"list")||same(r->action,"inspect")||same(r->action,"read"));
    const char *name=str(r,"name"),*new_name=str(r,"new_name"),*id=str(r,"buffer_id"),*kind=str(r,"kind"),*rid=str(r,"request_id");
    if((name&&!valid_name(name))||(new_name&&!valid_name(new_name))||(id&&!uuid_valid(id)))return problem(s,"invalid_input","invalid name or buffer_id");
    if(kind&&!(same(kind,"scratch")||same(kind,"file")||same(kind,"log")))return problem(s,"invalid_input","invalid buffer kind");
    if((str(r,"expected_revision")&&!hash_valid(str(r,"expected_revision")))||
       (str(r,"expected_source_revision")&&!hash_valid(str(r,"expected_source_revision"))))return problem(s,"invalid_input","revision must be lowercase SHA256");
    if(rid&&(!*rid||strlen(rid)>80))return problem(s,"invalid_input","request_id must contain 1 to 80 bytes");
    if(same(r->action,"create")&&!name)return problem(s,"invalid_input","create requires name");
    if(!(same(r->action,"create")||same(r->action,"list"))&&!name&&!id)return problem(s,"invalid_input","buffer_id or name required");
    if((same(r->action,"rename")||same(r->action,"fork"))&&!new_name)return problem(s,"invalid_input","new_name required");
    if((same(r->action,"write")||same(r->action,"append"))&&!str(r,"content"))return problem(s,"invalid_input","content required");
    if(same(r->action,"write")&&!str(r,"expected_revision"))return problem(s,"revision_required","write requires expected_revision");
    if(rid&&!(same(r->action,"create")||same(r->action,"fork")||same(r->action,"write")||same(r->action,"append")))
        return problem(s,"invalid_input","request_id is supported for create, fork, write and append");
    const char *allowed="action workspace buffer_id name expected_revision";
    if(same(r->action,"create"))allowed="action workspace name kind content source_path sensitive request_id";
    else if(same(r->action,"list"))allowed="action workspace";
    else if(same(r->action,"read"))allowed="action workspace buffer_id name offset max_bytes";
    else if(same(r->action,"inspect"))allowed="action workspace buffer_id name";
    else if(same(r->action,"write")||same(r->action,"append"))allowed="action workspace buffer_id name expected_revision content request_id";
    else if(same(r->action,"rename"))allowed="action workspace buffer_id name expected_revision new_name";
    else if(same(r->action,"fork"))allowed="action workspace buffer_id name expected_revision new_name request_id";
    else if(same(r->action,"save"))allowed="action workspace buffer_id name expected_revision path expected_source_revision";
    for(int j=0;fields[j];j++)if(seen&(1u<<j)) {
        const char *at=strstr(allowed,fields[j]);size_t len=strlen(fields[j]);
        if(!at||(at!=allowed&&at[-1]!=' ')||(at[len]&&at[len]!=' '))return problem(s,"invalid_input","field is not accepted for this action");
    }
    if(same(r->action,"create")) {
        if(same(kind,"file")) { if(!str(r,"source_path")||str(r,"content"))return problem(s,"invalid_input","file creation requires source_path and rejects content"); }
        else if(str(r,"source_path"))return problem(s,"invalid_input","source_path requires kind=file");
    }
    return true;
}
static bool fsync_dir(const char *path) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if(fd<0)return false;
    bool ok=!fsync(fd); close(fd); return ok;
}
static bool parent_of(const char *path,char parent[PATH_MAX]) {
    if(strlen(path)>=PATH_MAX)return false; strcpy(parent,path);
    char *slash=strrchr(parent,'/'); if(!slash)return false;
    if(slash==parent)slash[1]=0; else *slash=0; return true;
}
static bool private_dir(const char *path,bool create,bool strict) {
    bool made=false; if(create) { if(!mkdir(path,0700))made=true; else if(errno!=EEXIST)return false; }
    struct stat st;
    if(lstat(path,&st))return false;
    if(!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(strict&&(st.st_mode&0077))){errno=EACCES;return false;}
    char parent[PATH_MAX]; return !made||(parent_of(path,parent)&&fsync_dir(parent));
}
static bool store_paths(store_t *s,const char *workspace,bool create) {
    const char *base=getenv("DSCO_BUFFER_DIR"); char local[PATH_MAX];
    if(!base||!*base) {
        const char *home=getenv("HOME");
        if(!home||snprintf(local,sizeof(local),"%s/.dsco",home)>=(int)sizeof(local))return problem(s,"store_unavailable","state parent unavailable");
        if(!private_dir(local,create,false)){s->missing=!create&&errno==ENOENT;return problem(s,"store_unavailable","private state parent unavailable");}
        if(snprintf(local,sizeof(local),"%s/.dsco/buffers",home)>=(int)sizeof(local))return problem(s,"invalid_path","state path too long");
        base=local;
    }
    if(!private_dir(base,create,true)){s->missing=!create&&errno==ENOENT;return problem(s,"store_unavailable","buffer root must be an owned private directory");}
    if(!realpath(base,s->base))return problem(s,"store_unavailable","cannot resolve buffer root");
    if(snprintf(s->dir,sizeof(s->dir),"%s/%s",s->base,workspace)>=(int)sizeof(s->dir))return problem(s,"invalid_path","workspace path too long");
    if(!private_dir(s->dir,create,true)){s->missing=!create&&errno==ENOENT;return problem(s,"store_unavailable","workspace not created or not private");}
    if(snprintf(s->dbpath,sizeof(s->dbpath),"%s/index.sqlite",s->dir)>=(int)sizeof(s->dbpath))return problem(s,"invalid_path","state path too long");
    return true;
}
static bool sql(store_t *s,const char *query) {
    if(sqlite3_exec(s->db,query,NULL,NULL,NULL)!=SQLITE_OK)return problem(s,"metadata_error",sqlite3_errmsg(s->db)); return true;
}
static sqlite3_stmt *prepare(store_t *s,const char *query) {
    sqlite3_stmt *p=NULL;
    if(sqlite3_prepare_v2(s->db,query,-1,&p,NULL)!=SQLITE_OK)problem(s,"metadata_error",sqlite3_errmsg(s->db)); return p;
}
static void bind_text(sqlite3_stmt *p,int n,const char *text) { sqlite3_bind_text(p,n,text?text:"",-1,SQLITE_TRANSIENT); }
static bool step_done(store_t *s,sqlite3_stmt *p) {
    int rc=sqlite3_step(p); sqlite3_finalize(p);
    return rc==SQLITE_DONE?true:problem(s,rc==SQLITE_CONSTRAINT?"name_conflict":"metadata_error",sqlite3_errmsg(s->db));
}
static bool open_store(store_t *s,const char *workspace,bool create) {
    s->lock=-1; if(!store_paths(s,workspace,create))return false;
    char path[PATH_MAX]; if(snprintf(path,sizeof(path),"%s/lock",s->dir)>=(int)sizeof(path))return problem(s,"invalid_path","lock path too long");
    s->lock=open(path,(create?O_CREAT|O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    struct stat st;
    if(s->lock<0||fstat(s->lock,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077))return problem(s,"store_unavailable","private workspace lock unavailable");
    struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
    while(flock(s->lock,(create?LOCK_EX:LOCK_SH)|LOCK_NB)) {
        clock_gettime(CLOCK_MONOTONIC,&now);
        if(errno!=EWOULDBLOCK||now.tv_sec-start.tv_sec>=3)return problem(s,"busy","workspace lock deadline exceeded");
        struct timespec pause={.tv_nsec=10000000}; nanosleep(&pause,NULL);
    }
    bool fresh=false;
    if(lstat(s->dbpath,&st)) {
        if(errno!=ENOENT||!create)return problem(s,"store_unavailable","buffer index does not exist");
        int fd=open(s->dbpath,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return problem(s,"store_unavailable","cannot create private index"); close(fd); fresh=true;
    } else if(!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)||st.st_size==0)
        return problem(s,"metadata_corrupt","index must be an owned private nonempty regular file");
    int flags=(create?SQLITE_OPEN_READWRITE:SQLITE_OPEN_READONLY)|SQLITE_OPEN_NOFOLLOW;
    if(sqlite3_open_v2(s->dbpath,&s->db,flags,NULL)!=SQLITE_OK)return problem(s,"metadata_corrupt","cannot open buffer index");
    sqlite3_busy_timeout(s->db,2000);
    if(fresh) {
        if(!sql(s,"PRAGMA synchronous=FULL; BEGIN IMMEDIATE; CREATE TABLE buffers(id TEXT PRIMARY KEY,name TEXT UNIQUE NOT NULL,kind TEXT NOT NULL,source TEXT NOT NULL,source_revision TEXT NOT NULL,closed INTEGER NOT NULL,sensitive INTEGER NOT NULL); CREATE TABLE requests(id TEXT PRIMARY KEY,fingerprint TEXT NOT NULL,buffer_id TEXT NOT NULL,action TEXT NOT NULL,before_revision TEXT NOT NULL,after_revision TEXT NOT NULL,stage TEXT NOT NULL,pending INTEGER NOT NULL); PRAGMA user_version=1; COMMIT;")||!fsync_dir(s->dir))return problem(s,"metadata_error","new index persistence failed");
    }
    sqlite3_stmt *p=prepare(s,"PRAGMA user_version");
    bool version=p&&sqlite3_step(p)==SQLITE_ROW&&sqlite3_column_int(p,0)==1; if(p)sqlite3_finalize(p);
    if(!version)return problem(s,"metadata_corrupt","unsupported or missing buffer index version");
    p=prepare(s,"PRAGMA quick_check");
    bool integrity=p&&sqlite3_step(p)==SQLITE_ROW&&same((const char*)sqlite3_column_text(p,0),"ok"); if(p)sqlite3_finalize(p);
    return integrity?true:problem(s,"metadata_corrupt","buffer index integrity check failed");
}
static void close_store(store_t *s) {
    if(s->db)sqlite3_close(s->db);
    if(s->lock>=0){flock(s->lock,LOCK_UN);close(s->lock);}
}
static bool content_path(store_t *s,const char *id,char out[PATH_MAX]) {
    return uuid_valid(id)&&snprintf(out,PATH_MAX,"%s/%s.txt",s->dir,id)<PATH_MAX;
}
static bool read_content(store_t *s,const char *path,bool owned,content_t *c,bool missing_ok) {
    memset(c,0,sizeof(*c));struct stat first;
    if(lstat(path,&first)){if(missing_ok&&errno==ENOENT)return true;return problem(s,"content_unavailable","cannot inspect regular content file");}
    if(!S_ISREG(first.st_mode))return problem(s,"invalid_content","content must be a regular file");
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) { if(missing_ok&&errno==ENOENT)return true; return problem(s,"content_unavailable","cannot read regular content file"); }
    if(fstat(fd,&c->stat)||!S_ISREG(c->stat.st_mode)||c->stat.st_size<0||c->stat.st_size>BUFFER_LIMIT||
       (owned&&(c->stat.st_uid!=getuid()||(c->stat.st_mode&0077)))) {close(fd);return problem(s,"invalid_content","content must be a regular UTF-8 file <=1 MiB; owned files must stay private");}
    c->size=(size_t)c->stat.st_size; c->data=calloc(c->size+1,1);
    if(!c->data){close(fd);return problem(s,"out_of_memory","content allocation failed");}
    size_t used=0; while(used<c->size) {
        ssize_t n=read(fd,c->data+used,c->size-used); if(n<0&&errno==EINTR)continue;
        if(n<=0){free(c->data);c->data=NULL;close(fd);return problem(s,"content_changed","file changed while reading");} used+=(size_t)n;
    }
    char extra; struct stat after, current; bool stable=read(fd,&extra,1)==0&&!fstat(fd,&after)&&!lstat(path,&current)&&
        current.st_dev==after.st_dev&&current.st_ino==after.st_ino&&after.st_size==c->stat.st_size;
#ifdef __APPLE__
    stable=stable&&after.st_mtimespec.tv_sec==c->stat.st_mtimespec.tv_sec&&after.st_mtimespec.tv_nsec==c->stat.st_mtimespec.tv_nsec&&
        after.st_ctimespec.tv_sec==c->stat.st_ctimespec.tv_sec&&after.st_ctimespec.tv_nsec==c->stat.st_ctimespec.tv_nsec;
#else
    stable=stable&&after.st_mtim.tv_sec==c->stat.st_mtim.tv_sec&&after.st_mtim.tv_nsec==c->stat.st_mtim.tv_nsec&&
        after.st_ctim.tv_sec==c->stat.st_ctim.tv_sec&&after.st_ctim.tv_nsec==c->stat.st_ctim.tv_nsec;
#endif
    close(fd);
    if(!stable||!utf8(c->data,c->size)){free(c->data);c->data=NULL;return problem(s,stable?"invalid_utf8":"content_changed","content is invalid UTF-8, contains NUL, or changed while reading");}
    sha256_hex((const uint8_t*)c->data,c->size,c->revision); return true;
}
static bool decode_buffer(store_t *s,sqlite3_stmt *p,buffer_t *b) {
    memset(b,0,sizeof(*b)); char *dest[]={b->id,b->name,b->kind,b->source,b->source_revision};
    size_t caps[]={sizeof(b->id),sizeof(b->name),sizeof(b->kind),sizeof(b->source),sizeof(b->source_revision)};
    for(int i=0;i<5;i++) {
        const char *v=(const char*)sqlite3_column_text(p,i); int n=sqlite3_column_bytes(p,i);
        if(sqlite3_column_type(p,i)!=SQLITE_TEXT||!v||n<0||(size_t)n>=caps[i]||strlen(v)!=(size_t)n||!utf8(v,(size_t)n))return problem(s,"metadata_corrupt","invalid buffer metadata string");
        memcpy(dest[i],v,(size_t)n+1);
    }
    int closed=sqlite3_column_int(p,5), sensitive=sqlite3_column_int(p,6);
    if(sqlite3_column_type(p,5)!=SQLITE_INTEGER||sqlite3_column_type(p,6)!=SQLITE_INTEGER||!uuid_valid(b->id)||!valid_name(b->name)||!(same(b->kind,"scratch")||same(b->kind,"file")||same(b->kind,"log"))||
       (closed!=0&&closed!=1)||(sensitive!=0&&sensitive!=1)||(*b->source&&b->source[0]!='/')||
       (*b->source_revision&&!hash_valid(b->source_revision))||(!*b->source!=!*b->source_revision))return problem(s,"metadata_corrupt","invalid buffer identity or flags");
    b->closed=closed;b->sensitive=sensitive;return true;
}
static bool find_buffer(store_t *s,const char *id,const char *name,buffer_t *b) {
    sqlite3_stmt *p=prepare(s,id?"SELECT id,name,kind,source,source_revision,closed,sensitive FROM buffers WHERE id=?":"SELECT id,name,kind,source,source_revision,closed,sensitive FROM buffers WHERE name=?");
    if(!p)return false;bind_text(p,1,id?id:name);int rc=sqlite3_step(p);
    bool ok=rc==SQLITE_ROW&&decode_buffer(s,p,b);sqlite3_finalize(p);
    if(rc!=SQLITE_ROW)return problem(s,rc==SQLITE_DONE?"not_found":"metadata_error","buffer not found");
    if(ok&&id&&name&&!same(name,b->name))return problem(s,"selector_conflict","buffer_id and name identify different buffers");return ok;
}
static bool update_buffer(store_t *s,buffer_t *b,bool insert) {
    sqlite3_stmt *p=prepare(s,insert?"INSERT INTO buffers(id,name,kind,source,source_revision,closed,sensitive) VALUES(?,?,?,?,?,?,?)":"UPDATE buffers SET name=?,kind=?,source=?,source_revision=?,closed=?,sensitive=? WHERE id=?");
    if(!p)return false;
    if(insert){bind_text(p,1,b->id);bind_text(p,2,b->name);bind_text(p,3,b->kind);bind_text(p,4,b->source);bind_text(p,5,b->source_revision);sqlite3_bind_int(p,6,b->closed);sqlite3_bind_int(p,7,b->sensitive);}
    else{bind_text(p,1,b->name);bind_text(p,2,b->kind);bind_text(p,3,b->source);bind_text(p,4,b->source_revision);sqlite3_bind_int(p,5,b->closed);sqlite3_bind_int(p,6,b->sensitive);bind_text(p,7,b->id);}
    return step_done(s,p);
}
static bool sensitive_path(const char *path) {
    if(!path)return false; const char *base=strrchr(path,'/');base=base?base+1:path;
    return !strcmp(base,".env")||!strncmp(base,".env.",5)||strstr(path,"/.ssh/")||strstr(path,"/.aws/")||
           strcasestr(path,"/keychains/")||strcasestr(path,".keychain")||strstr(path,"credentials")||strstr(path,"private_key")||strstr(path,"id_rsa");
}
static bool keychain_target(const char *path) { return strcasestr(path,"/keychains/")||strcasestr(path,".keychain"); }
static bool canonical_path(store_t *s,const char *input,char out[PATH_MAX],bool allow_new) {
    if(!input||input[0]!='/')return problem(s,"invalid_path","an absolute file path is required");
    struct stat st;
    if(!lstat(input,&st)) {
        if(S_ISLNK(st.st_mode))return problem(s,"invalid_path","symlink file targets are not accepted");
        if(!realpath(input,out))return problem(s,"invalid_path","cannot canonicalize file path");return true;
    }
    if(!allow_new||errno!=ENOENT)return problem(s,"invalid_path","source file does not exist");
    char parent[PATH_MAX],resolved[PATH_MAX];
    if(!parent_of(input,parent)||!realpath(parent,resolved))return problem(s,"invalid_path","target parent must exist");
    const char *base=strrchr(input,'/')+1;
    if(!*base||same(base,".")||same(base,"..")||snprintf(out,PATH_MAX,"%s/%s",resolved,base)>=PATH_MAX)return problem(s,"invalid_path","invalid target filename");return true;
}
static bool atomic_write(store_t *s,const char *path,const char *data,size_t size,mode_t mode) {
    char parent[PATH_MAX],temp[PATH_MAX];
    if(!parent_of(path,parent)||snprintf(temp,sizeof(temp),"%s/.dsco-buffer-write-XXXXXX",parent)>=(int)sizeof(temp))return problem(s,"invalid_path","target path too long");
    int fd=mkstemp(temp);if(fd<0)return problem(s,"write_failed","cannot create atomic write file");
    fcntl(fd,F_SETFD,FD_CLOEXEC);bool ok=!fchmod(fd,mode);size_t used=0;
    while(ok&&used<size){ssize_t n=write(fd,data+used,size-used);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else used+=(size_t)n;}
    if(ok)ok=!fsync(fd);if(close(fd))ok=false;
    if(ok)ok=!rename(temp,path);if(ok)ok=fsync_dir(parent);
    if(!ok){unlink(temp);return problem(s,"write_failed","atomic write or durability check failed");}return true;
}
static void fingerprint(request_t *r,char hash[65]) {
    jbuf_t b;jbuf_init(&b,512);jbuf_append_json_str(&b,r->workspace);
    for(int i=0;fields[i];i++) {
        if(same(fields[i],"workspace")||same(fields[i],"request_id"))continue;
        yyjson_val *v=yyjson_obj_get(r->root,fields[i]);if(!v)continue;
        char *encoded=yyjson_val_write(v,0,NULL);jbuf_append_json_str(&b,fields[i]);jbuf_append(&b,encoded);free(encoded);
    }
    sha256_hex((const uint8_t*)b.data,b.len,hash);jbuf_free(&b);
}
static bool count_below(store_t *s,const char *query,int limit) {
    sqlite3_stmt *p=prepare(s,query);if(!p)return false;
    bool ok=sqlite3_step(p)==SQLITE_ROW&&sqlite3_column_int(p,0)<limit;sqlite3_finalize(p);
    return ok?true:problem(s,"store_full","buffer or retry-record limit reached");
}
/* A durable staged replacement makes append retries exactly-once. On a crash,
 * matching request retry checks before/after hashes before committing the stage.
 * A different external edit is never overwritten during recovery. */
static bool finish_request(store_t *s,const char *request_id,const char *id,const char *before,const char *after,const char *stage) {
    char target[PATH_MAX],staged[PATH_MAX];content_t c={0};
    if(!content_path(s,id,target)||!uuid_valid(stage)||snprintf(staged,sizeof(staged),"%s/%s.stage",s->dir,stage)>=(int)sizeof(staged))return problem(s,"metadata_corrupt","invalid pending replacement identity");
    if(!read_content(s,target,true,&c,true))return false;
    bool already=c.data&&same(c.revision,after), prior=(!c.data&&!*before)||(c.data&&same(c.revision,before));free(c.data);
    if(!already&&!prior)return problem(s,"pending_conflict","pending replacement conflicts with external content; original request was not replayed");
    if(!already) {
        content_t payload={0};if(!read_content(s,staged,true,&payload,false))return false;
        bool matches=same(payload.revision,after);free(payload.data);
        if(!matches)return problem(s,"metadata_corrupt","pending payload revision mismatch");
        if(rename(staged,target)||!fsync_dir(s->dir))return problem(s,"pending_write","replacement outcome requires retry reconciliation");
    } else unlink(staged);
    sqlite3_stmt *p=prepare(s,"UPDATE requests SET pending=0 WHERE id=?");if(!p)return false;bind_text(p,1,request_id);return step_done(s,p);
}
static int retry_request(store_t *s,request_t *r,buffer_t *b,char applied[65]) {
    const char *rid=str(r,"request_id");if(!rid)return 0;
    sqlite3_stmt *p=prepare(s,"SELECT fingerprint,buffer_id,before_revision,after_revision,stage,pending FROM requests WHERE id=?");if(!p)return -1;
    bind_text(p,1,rid);int rc=sqlite3_step(p);if(rc==SQLITE_DONE){sqlite3_finalize(p);return 0;}
    if(rc!=SQLITE_ROW){sqlite3_finalize(p);problem(s,"metadata_error","cannot inspect request record");return -1;}
    char fp[65],id[37],before[65],after[65],stage[37];char *dest[]={fp,id,before,after,stage};size_t caps[]={65,37,65,65,37};bool valid=true;
    for(int i=0;i<5;i++){const char *v=(const char*)sqlite3_column_text(p,i);int n=sqlite3_column_bytes(p,i);if(sqlite3_column_type(p,i)!=SQLITE_TEXT||!v||n<0||(size_t)n>=caps[i]||strlen(v)!=(size_t)n)valid=false;else memcpy(dest[i],v,(size_t)n+1);}
    int pending=sqlite3_column_int(p,5);if(sqlite3_column_type(p,5)!=SQLITE_INTEGER)valid=false;sqlite3_finalize(p);
    if(!valid||!hash_valid(fp)||!uuid_valid(id)||(*before&&!hash_valid(before))||!hash_valid(after)||!uuid_valid(stage)||(pending!=0&&pending!=1)){problem(s,"metadata_corrupt","invalid retry record");return -1;}
    char want[65];fingerprint(r,want);if(!same(fp,want)){problem(s,"idempotency_conflict","request_id was already used with different input");return -1;}
    if(pending&&!finish_request(s,rid,id,before,after,stage))return -1;
    if(!find_buffer(s,id,NULL,b))return -1;strcpy(applied,after);return 1;
}
static bool replace_content(store_t *s,request_t *r,buffer_t *b,const content_t *old,const char *data,size_t size,bool insert,char applied[65]) {
    if(size>BUFFER_LIMIT||!utf8(data,size))return problem(s,"invalid_content","content must be UTF-8 without NUL and at most 1 MiB");
    if(!count_below(s,"SELECT count(*) FROM requests",REQUEST_COUNT))return false;
    sqlite3_stmt *check=prepare(s,"SELECT 1 FROM requests WHERE buffer_id=? AND pending=1 LIMIT 1");if(!check)return false;
    bind_text(check,1,b->id);int pending=sqlite3_step(check);sqlite3_finalize(check);
    if(pending==SQLITE_ROW)return problem(s,"pending_write","reconcile the prior request_id before modifying this buffer");
    char rid[81],stage[37],staged[PATH_MAX],fp[65];
    if(!new_id(stage))return problem(s,"random_unavailable","cannot allocate replacement identity");
    snprintf(rid,sizeof(rid),"%s",str(r,"request_id")?str(r,"request_id"):stage);
    if(snprintf(staged,sizeof(staged),"%s/%s.stage",s->dir,stage)>=(int)sizeof(staged))return problem(s,"invalid_path","stage path too long");
    if(!atomic_write(s,staged,data,size,0600))return false;
    sha256_hex((const uint8_t*)data,size,applied);fingerprint(r,fp);
    if(!sql(s,"BEGIN IMMEDIATE")){unlink(staged);return false;}
    bool ok=!insert||update_buffer(s,b,true);
    sqlite3_stmt *p=ok?prepare(s,"INSERT INTO requests(id,fingerprint,buffer_id,action,before_revision,after_revision,stage,pending) VALUES(?,?,?,?,?,?,?,1)"):NULL;
    if(p){bind_text(p,1,rid);bind_text(p,2,fp);bind_text(p,3,b->id);bind_text(p,4,r->action);bind_text(p,5,old?old->revision:"");bind_text(p,6,applied);bind_text(p,7,stage);ok=step_done(s,p);}else ok=false;
    if(ok)ok=sql(s,"COMMIT");if(!ok){sql(s,"ROLLBACK");unlink(staged);return false;}
    return finish_request(s,rid,b->id,old?old->revision:"",applied,stage);
}
static bool append_metadata(store_t *s,jbuf_t *out,buffer_t *b,content_t *loaded) {
    char path[PATH_MAX];content_t local={0};content_t *c=loaded?loaded:&local;
    if(!content_path(s,b->id,path)||(!loaded&&!read_content(s,path,true,c,false)))return false;
    jbuf_append(out,"{\"buffer_id\":");jbuf_append_json_str(out,b->id);jbuf_append(out,",\"name\":");jbuf_append_json_str(out,b->name);
    jbuf_append(out,",\"kind\":");jbuf_append_json_str(out,b->kind);jbuf_append(out,",\"content_path\":");jbuf_append_json_str(out,path);
    jbuf_append(out,",\"source_path\":");jbuf_append_json_str(out,b->source);jbuf_append(out,",\"source_revision\":");jbuf_append_json_str(out,b->source_revision);
    jbuf_append(out,",\"revision\":");jbuf_append_json_str(out,c->revision);
    jbuf_appendf(out,",\"closed\":%s,\"sensitive\":%s,\"bytes\":%zu}",b->closed?"true":"false",b->sensitive?"true":"false",c->size);
    if(!loaded)free(local.data);return true;
}
static bool list_buffers(store_t *s,jbuf_t *out) {
    sqlite3_stmt *p=prepare(s,"SELECT id,name,kind,source,source_revision,closed,sensitive FROM buffers ORDER BY name");if(!p)return false;
    jbuf_append(out,"{\"ok\":true,\"buffers\":[");int count=0,rc;bool ok=true;
    while((rc=sqlite3_step(p))==SQLITE_ROW){buffer_t b;if(++count>BUFFER_COUNT||!decode_buffer(s,p,&b)){ok=false;break;}if(count>1)jbuf_append_char(out,',');if(!append_metadata(s,out,&b,NULL)){ok=false;break;}}
    sqlite3_finalize(p);jbuf_append(out,"]}");return ok&&rc==SQLITE_DONE;
}
static bool save_source(store_t *s,request_t *r,buffer_t *b,content_t *content) {
    char target[PATH_MAX];const char *requested=str(r,"path");if(!requested)requested=b->source;
    if(!canonical_path(s,requested,target,true))return false;
    if(keychain_target(target))return problem(s,"forbidden_target","buffer save never replaces keychains");
    size_t base_len=strlen(s->base);
    if(!strncmp(target,s->base,base_len)&&(target[base_len]=='/'||!target[base_len]))return problem(s,"forbidden_target","save cannot overwrite buffer-store internals");
    content_t external={0};if(!read_content(s,target,false,&external,true))return false;
    const char *expected=str(r,"expected_source_revision");if(!expected&&same(target,b->source))expected=b->source_revision;
    if(external.data&&(!expected||!*expected||!same(expected,external.revision))){free(external.data);return problem(s,"source_conflict","target changed or an existing target requires expected_source_revision");}
    if(!external.data&&same(target,b->source)&&*b->source_revision){return problem(s,"source_conflict","remembered source disappeared; choose an explicit new target");}
    mode_t mode=external.data?(external.stat.st_mode&0777):0600;free(external.data);
    if(!atomic_write(s,target,content->data,content->size,mode))return false;
    snprintf(b->source,sizeof(b->source),"%s",target);strcpy(b->source_revision,content->revision);b->sensitive|=sensitive_path(target);
    if(!update_buffer(s,b,false))return problem(s,"save_outcome_uncertain","target was saved but metadata update failed; inspect target before retry");return true;
}
static bool execute(store_t *s,request_t *r,jbuf_t *out,size_t output_cap) {
    if(same(r->action,"list"))return list_buffers(s,out);
    buffer_t b={0};content_t c={0};char applied[65]="";bool replayed=false;
    int retry=retry_request(s,r,&b,applied);if(retry<0)return false;replayed=retry==1;
    bool create=same(r->action,"create"),fork=same(r->action,"fork");
    if(!replayed&&(create||fork)) {
        if(!count_below(s,"SELECT count(*) FROM buffers",BUFFER_COUNT))return false;
        if(fork) {
            if(!find_buffer(s,str(r,"buffer_id"),str(r,"name"),&b))return false;
            char path[PATH_MAX];content_path(s,b.id,path);if(!read_content(s,path,true,&c,false))return false;
        } else {
            snprintf(b.kind,sizeof(b.kind),"%s",str(r,"kind")?str(r,"kind"):"scratch");
            b.sensitive=yyjson_get_bool(yyjson_obj_get(r->root,"sensitive"));
            if(same(b.kind,"file")) {
                if(str(r,"content"))return problem(s,"invalid_input","file creation imports source_path; content is not accepted");
                if(!canonical_path(s,str(r,"source_path"),b.source,false)||!read_content(s,b.source,false,&c,false))return false;
                strcpy(b.source_revision,c.revision);b.sensitive|=sensitive_path(b.source);
            } else {
                if(str(r,"source_path"))return problem(s,"invalid_input","source_path requires kind=file");
                c.data=strdup(str(r,"content")?str(r,"content"):"");if(!c.data)return problem(s,"out_of_memory","content allocation failed");c.size=strlen(c.data);
            }
        }
        if(fork&&str(r,"expected_revision")&&!same(str(r,"expected_revision"),c.revision)){free(c.data);return problem(s,"revision_conflict","source buffer differs from expected_revision");}
        if(fork){b.source[0]=b.source_revision[0]=0;strcpy(b.kind,"scratch");b.closed=false;}
        snprintf(b.name,sizeof(b.name),"%s",fork?str(r,"new_name"):str(r,"name"));
        if(!new_id(b.id)){free(c.data);return problem(s,"random_unavailable","cannot allocate buffer UUID");}
        bool ok=replace_content(s,r,&b,NULL,c.data,c.size,true,applied);free(c.data);c.data=NULL;if(!ok)return false;
    } else if(!replayed&&!find_buffer(s,str(r,"buffer_id"),str(r,"name"),&b))return false;
    char path[PATH_MAX];content_path(s,b.id,path);if(!read_content(s,path,true,&c,false))return false;
    bool ok=true;
    const char *expected=str(r,"expected_revision");
    if(!replayed&&expected&&!same(expected,c.revision))ok=problem(s,"revision_conflict","actual content differs from expected_revision");
    if(ok&&!replayed&&b.closed&&!(same(r->action,"inspect")||same(r->action,"read")||same(r->action,"close")||same(r->action,"reopen")||fork))ok=problem(s,"buffer_closed","reopen buffer before editing or saving");
    if(ok&&!replayed&&(same(r->action,"write")||same(r->action,"append"))) {
        const char *text=str(r,"content");size_t n=strlen(text);bool append=same(r->action,"append");
        if(n+(append?c.size:0)>BUFFER_LIMIT)ok=problem(s,"content_too_large","result would exceed 1 MiB");
        else {
            char *next=malloc(n+(append?c.size:0)+1);
            if(!next)ok=problem(s,"out_of_memory","replacement allocation failed");
            else {size_t prefix=append?c.size:0;if(prefix)memcpy(next,c.data,prefix);memcpy(next+prefix,text,n+1);ok=replace_content(s,r,&b,&c,next,prefix+n,false,applied);free(next);}
        }
    } else if(ok&&!replayed&&same(r->action,"rename")){strcpy(b.name,str(r,"new_name"));ok=update_buffer(s,&b,false);}
    else if(ok&&!replayed&&(same(r->action,"close")||same(r->action,"reopen"))){b.closed=same(r->action,"close");ok=update_buffer(s,&b,false);}
    else if(ok&&!replayed&&same(r->action,"save"))ok=save_source(s,r,&b,&c);
    if(!ok){free(c.data);return false;}
    if(r->mutation){free(c.data);c.data=NULL;if(!read_content(s,path,true,&c,false))return false;}
    jbuf_append(out,"{\"ok\":true,\"buffer\":");ok=append_metadata(s,out,&b,&c);
    if(ok&&same(r->action,"read")) {
        yyjson_val *offset_val=yyjson_obj_get(r->root,"offset"),*max_val=yyjson_obj_get(r->root,"max_bytes");
        size_t offset=offset_val?(size_t)yyjson_get_uint(offset_val):0, amount=max_val?(size_t)yyjson_get_uint(max_val):4096;
        size_t safe_cap=output_cap>out->len+512?(output_cap-out->len-512)/8:0;if(amount>safe_cap)amount=safe_cap;
        if(offset>c.size||(offset<c.size&&((unsigned char)c.data[offset]&0xc0)==0x80))ok=problem(s,"invalid_offset","offset must be a UTF-8 character boundary within content");
        else {
            size_t end=offset+amount;if(end>c.size)end=c.size;
            while(end>offset&&end<c.size&&((unsigned char)c.data[end]&0xc0)==0x80)end--;
            if(end==offset&&offset<c.size)ok=problem(s,"chunk_too_small","max_bytes or result capacity cannot hold the next UTF-8 character");
            else {
                size_t bytes=end-offset;char *text=strndup(c.data+offset,bytes),*b64=malloc(((bytes+2)/3)*4+1);
                if(!text||!b64)ok=problem(s,"out_of_memory","read chunk allocation failed");
                else {base64_encode((const uint8_t*)text,bytes,b64,((bytes+2)/3)*4+1);jbuf_append(out,",\"text\":");jbuf_append_json_str(out,text);jbuf_append(out,",\"base64\":");jbuf_append_json_str(out,b64);jbuf_appendf(out,",\"offset\":%zu,\"bytes\":%zu,\"next_offset\":%zu,\"truncated\":%s",offset,bytes,end,end<c.size?"true":"false");}
                free(text);free(b64);
            }
        }
    }
    if(r->mutation){jbuf_appendf(out,",\"replayed\":%s",replayed?"true":"false");if(*applied){jbuf_append(out,",\"applied_revision\":");jbuf_append_json_str(out,applied);}}
    jbuf_append_char(out,'}');free(c.data);return ok;
}
bool tool_buffer(const char *input,char *out,size_t cap) {
    store_t s={.lock=-1};request_t r={0};bool ok=parse_request(input,&r,&s);
    pthread_mutex_lock(&store_mutex);
    jbuf_t response;jbuf_init(&response,2048);
    if(ok) {
        ok=open_store(&s,r.workspace,r.mutation);
        if(!ok&&same(r.action,"list")&&s.missing){s.error=NULL;s.detail[0]=0;jbuf_append(&response,"{\"ok\":true,\"buffers\":[]}");ok=true;}
        else if(ok)ok=execute(&s,&r,&response,cap);
    }
    if(ok&&(!out||response.len>=cap))ok=problem(&s,"result_too_large","use inspect/read with a smaller byte range");
    if(ok)memcpy(out,response.data,response.len+1);else output_error(out,cap,s.error?s.error:"operation_failed",s.detail);
    close_store(&s);jbuf_free(&response);pthread_mutex_unlock(&store_mutex);if(r.doc)yyjson_doc_free(r.doc);return ok;
}
bool buffer_store_sensitive(const char *input) {
    store_t s={.lock=-1};request_t r={0};bool sensitive=true;
    if(!parse_request(input,&r,&s))goto done;
    sensitive=yyjson_get_bool(yyjson_obj_get(r.root,"sensitive"))||sensitive_path(str(&r,"source_path"))||sensitive_path(str(&r,"path"));
    char resolved[PATH_MAX];
    const char *source=str(&r,"source_path"),*target=str(&r,"path");
    if(source&&realpath(source,resolved))sensitive|=sensitive_path(resolved);
    if(target&&canonical_path(&s,target,resolved,true))sensitive|=sensitive_path(resolved);
    if(sensitive||same(r.action,"create"))goto done;
    pthread_mutex_lock(&store_mutex);
    if(!open_store(&s,r.workspace,false))sensitive=!s.missing;
    else if(same(r.action,"list")) {
        sqlite3_stmt *p=prepare(&s,"SELECT id,name,kind,source,source_revision,closed,sensitive FROM buffers");
        int rc=SQLITE_ERROR,count=0;sensitive=!p;
        while(p&&(rc=sqlite3_step(p))==SQLITE_ROW){buffer_t b;if(++count>BUFFER_COUNT||!decode_buffer(&s,p,&b)||b.sensitive){sensitive=true;break;}}
        if(rc!=SQLITE_DONE)sensitive=true;if(p)sqlite3_finalize(p);
    } else {buffer_t b;sensitive=!find_buffer(&s,str(&r,"buffer_id"),str(&r,"name"),&b)||b.sensitive;}
    close_store(&s);pthread_mutex_unlock(&store_mutex);
done:
    if(r.doc)yyjson_doc_free(r.doc);return sensitive;
}
