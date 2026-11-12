#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"
#include "../src/ipc/ipc_meta.h"
#include "../src/ipc/ipc_posix_shm.h"
#include "../src/ipc/pshm_owned.h"

atomic_bool g_stop = false;
static const ipc_backend_t *b;
static ipc_config_t cfg = {.size=3,.depth=1, .team=1};
static unsigned destroyed;
static int fail_init = -1, fail_space;
int __real_sem_init(sem_t *s, int shared, unsigned value);
int __wrap_sem_init(sem_t *s, int shared, unsigned value)
{
    if (shared && fail_init == 0) { errno = ENOSPC; return -1; }
    if (shared && fail_init > 0) fail_init--;
    return __real_sem_init(s, shared, value);
}
int __real_posix_fallocate(int fd, off_t off, off_t length);
int __wrap_posix_fallocate(int fd, off_t off, off_t length)
{
    return fail_space ? ENOSPC : __real_posix_fallocate(fd, off, length);
}
int __real_sem_destroy(sem_t *s);
int __wrap_sem_destroy(sem_t *s) { destroyed++; return __real_sem_destroy(s); }
static void reap(pid_t p) { int st; assert(waitpid(p,&st,0)==p && WIFEXITED(st) && !WEXITSTATUS(st)); }
static void killed(pid_t p) { int st; assert(kill(p,SIGKILL)==0); assert(waitpid(p,&st,0)==p && WIFSIGNALED(st)); }
static void mark(int fd) { assert(write(fd,"r",1)==1); assert(close(fd)==0); }
static void ready(int fd) { char c; assert(read(fd,&c,1)==1); assert(close(fd)==0); }
static pshm_record_t record(const char *key)
{
    char path[256]; snprintf(path,sizeof path,"%s.pshm",key);
    int fd=open(path,O_RDONLY); assert(fd>=0); pshm_record_t r;
    assert(pshm_read(fd,&r)==1); assert(close(fd)==0); return r;
}
static pshm_hdr_t *mapping(const pshm_record_t *r)
{
    int fd=shm_open(r->name,O_RDWR,0); assert(fd>=0);
    void *h=mmap(NULL,sizeof(pshm_hdr_t),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    assert(h!=MAP_FAILED && close(fd)==0); return h;
}
static void gone(const char *key, const pshm_record_t *r)
{
    assert(shm_open(r->name,O_RDONLY,0)==-1 && errno==ENOENT);
    char path[256]; const char *suffix[]={"meta","pshm"};
    for (unsigned i=0;i<2;i++) { snprintf(path,sizeof path,"%s.%s",key,suffix[i]); assert(access(path,F_OK)==-1 && errno==ENOENT); }
}
static void clean(const char *key, int success)
{
    pid_t p=fork(); assert(p>=0);
    if (!p) { execl("./clean_ipc","clean_ipc",key,(char *)NULL); _exit(127); }
    int st; assert(waitpid(p,&st,0)==p && WIFEXITED(st)); assert((WEXITSTATUS(st)==0)==success);
}
static void waiting_client(int killed_server)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-wait-%d-%d",getpid(),killed_server);
    int pipefd[2], end[2]; assert(pipe(pipefd)==0 && pipe(end)==0);
    pid_t p=fork(); assert(p>=0);
    if (!p) {
        assert(close(pipefd[0])==0 && close(end[1])==0);
        ipc_channel_t *s=b->server_open(key,&cfg); assert(s); mark(pipefd[1]);
        ready(end[0]); destroyed=0; b->destroy(s); assert(destroyed==0); b->close(s); _exit(0);
    }
    assert(close(pipefd[1])==0 && close(end[0])==0); ready(pipefd[0]);
    pshm_record_t r=record(key);
    ipc_channel_t *c=b->client_open(key,1,&cfg); assert(c);
    if (killed_server) { killed(p); assert(close(end[1])==0); }
    else { mark(end[1]); reap(p); }
    alarm(5); msg_hdr_t h;
    assert(b->recv(c,&h,NULL,0)==-1 && errno==ECONNRESET);
    if (killed_server) clean(key,0); /* 활성 클라 잠금 */
    b->close(c); if (killed_server) clean(key,1);
    gone(key,&r); alarm(30);
    puts(killed_server ? "POSIX: SIGKILL server, live client cleanup refusal, <=5s return OK" :
        "POSIX: live client teardown skips all sem_destroy calls, <=5s return OK");
}
static void token_holder(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-token-%d",getpid());
    ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
    pshm_record_t r=record(key); pshm_hdr_t *h=mapping(&r);
    int pipefd[2]; assert(pipe(pipefd)==0);
    pid_t p=fork(); assert(p>=0);
    if (!p) {
        b->close(s); assert(close(pipefd[0])==0);
        ipc_channel_t *c=b->client_open(key,1,&cfg); assert(c);
        assert(sem_trywait(&h->sems[0].req_empty)==0); mark(pipefd[1]); for (;;) pause();
    }
    assert(close(pipefd[1])==0); ready(pipefd[0]); killed(p);
    int value; assert(sem_getvalue(&h->sems[0].req_empty,&value)==0 && value==0);
    msg_hdr_t msg={.magic=IPC_MAGIC,.type=MSG_RESP,.client_id=1};
    assert(b->send(s,&msg,NULL)==-1 && errno==EPIPE);
    assert(sem_getvalue(&h->sems[0].req_empty,&value)==0 && value==0);
    assert(munmap(h,sizeof *h)==0);
    destroyed=0; b->destroy(s); assert(destroyed==16); b->close(s); gone(key,&r);
    puts("POSIX: killed token holder is retired without token reset, exclusive destroy=16 OK");
}
static void collision(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-collision-%d",getpid());
    int meta=meta_open(key); assert(meta>=0);
    pshm_record_t r={.magic=IPC_MAGIC}; assert(pshm_name(cfg.team,r.name)==0); assert(close(meta)==0);
    int fd=shm_open(r.name,O_RDWR|O_CREAT|O_EXCL,0600); assert(fd>=0);
    assert(ftruncate(fd,123)==0);
    assert(!b->server_open(key,&cfg) && errno==EEXIST);
    struct stat st; assert(fstat(fd,&st)==0 && st.st_size==123);
    int check=shm_open(r.name,O_RDONLY,0); assert(check>=0 && close(check)==0);
    assert(shm_unlink(r.name)==0 && close(fd)==0); gone(key,&r);
    puts("POSIX: unrecorded name collision preserves original object OK");
}
static void validation(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-validation-%d",getpid());
    ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
    pshm_record_t r=record(key); pshm_hdr_t *h=mapping(&r);
    h->slot_bytes++;
    pid_t p=fork(); assert(p>=0);
    if (!p) { b->close(s); assert(!b->client_open(key,1,&cfg) && errno==EINVAL); _exit(0); }
    reap(p); h->slot_bytes--;
    p=fork(); assert(p>=0);
    if (!p) {
        b->close(s); ipc_config_t wrong={.size=4,.depth=1, .team=1};
        assert(!b->client_open(key,2,&wrong) && errno==EINVAL); _exit(0);
    }
    reap(p); assert(munmap(h,sizeof *h)==0);
    b->destroy(s); b->close(s); gone(key,&r);
    puts("POSIX: shared header and CLI mismatch rejected before ring use OK");
}
static void creation_errors(void)
{
    for (unsigned space = 0; space < 2; space++) {
        char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-partial-%d-%u",getpid(),space);
        int meta=meta_open(key); assert(meta>=0);
        pshm_record_t r={.magic=IPC_MAGIC}; assert(pshm_name(cfg.team,r.name)==0 && close(meta)==0);
        destroyed=0; fail_space=(int)space; fail_init=space ? -1 : 5;
        assert(!b->server_open(key,&cfg) && errno==ENOSPC);
        assert(destroyed==(space ? 0 : 5)); gone(key,&r);
        fail_space=0; fail_init=-1;
    }
    puts("POSIX: allocation failure and partial sem_init failure clean up only initialized semaphores OK");
}
static void replacement(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-posix-replaced-%d",getpid());
    int pipefd[2]; assert(pipe(pipefd)==0); pid_t p=fork(); assert(p>=0);
    if (!p) {
        assert(close(pipefd[0])==0); ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
        mark(pipefd[1]); for (;;) pause();
    }
    assert(close(pipefd[1])==0); ready(pipefd[0]); killed(p);
    pshm_record_t r=record(key); assert(shm_unlink(r.name)==0);
    int fd=shm_open(r.name,O_RDWR|O_CREAT|O_EXCL,0600); assert(fd>=0 && ftruncate(fd,123)==0);
    clean(key,0); assert(!b->server_open(key,&cfg) && errno==EEXIST);
    pshm_record_t kept=record(key); assert(kept.inode==r.inode);
    struct stat st; assert(fstat(fd,&st)==0 && st.st_size==123);
    assert(shm_unlink(r.name)==0 && close(fd)==0); clean(key,1); gone(key,&r);
    puts("POSIX: replaced inode preserved by clean and restart, original ownership record retained OK");
}
int main(void)
{
    alarm(30); b=ipc_lookup("posix_shm"); assert(b);
    token_holder(); waiting_client(0); waiting_client(1); collision(); validation(); creation_errors(); replacement(); return 0;
}
