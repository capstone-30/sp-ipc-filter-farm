/* 역순 응답은 seq와 링 위치가 독립임을 검증한다. */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"
#include "../src/ipc/ipc_names.h"
#include "../src/ipc/ipc_sysv_shm.h"

atomic_bool g_stop = false;
static const ipc_backend_t *b;
static void reap(pid_t p) { int s; assert(waitpid(p, &s, 0) == p); assert(WIFEXITED(s) && !WEXITSTATUS(s)); }
static msg_hdr_t header(unsigned id, unsigned seq)
{
    return (msg_hdr_t){.magic=IPC_MAGIC, .type=MSG_REQ, .client_id=id, .seq=seq,
        .filter=id == 1 ? F_GRAY : F_EDGE, .width=3, .height=3, .channels=3, .payload_len=27};
}
static void client(const char *key, const ipc_config_t *cfg, unsigned id)
{
    ipc_channel_t *c = b->client_open(key, id, cfg); assert(c);
    unsigned char data[27], seen[1000] = {0};
    for (unsigned base=0; base<1000; base+=cfg->depth) {
        unsigned n = 1000-base < cfg->depth ? 1000-base : cfg->depth;
        for (unsigned j=0; j<n; j++) {
            memset(data, (unsigned char)(base+j+id), sizeof data);
            msg_hdr_t h=header(id, base+j); h.checksum=fnv1a32(data,sizeof data);
            assert(b->send(c,&h,data)==0);
        }
        for (unsigned j=0; j<n; j++) {
            msg_hdr_t h; assert(b->recv(c,&h,data,sizeof data)==0);
            assert(h.type==MSG_RESP && h.client_id==id && h.seq==base+n-1-j);
            assert(h.filter==(id==1 ? F_GRAY : F_EDGE) && h.payload_len==sizeof data);
            assert(h.checksum==fnv1a32(data,sizeof data) && !seen[h.seq]++);
            for (unsigned k=0;k<sizeof data;k++) assert(data[k]==(unsigned char)(h.seq+id));
        }
    }
    for (unsigned i=0;i<1000;i++) assert(seen[i]==1);
    msg_hdr_t bye=header(id,0); bye.type=MSG_BYE; bye.payload_len=0;
    assert(b->send(c,&bye,NULL)==0); b->close(c); _exit(0);
}
static void reverse(unsigned depth)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-shm-%d-%u",getpid(),depth);
    ipc_config_t cfg={.size=3,.depth=depth, .team=1};
    pid_t p[2];
    for (unsigned i=0;i<2;i++) { p[i]=fork(); assert(p[i]>=0); if (!p[i]) client(key,&cfg,i+1); }
    ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
    msg_hdr_t batch[2][16]; unsigned char data[2][16][27];
    unsigned count[2]={0}, total[2]={0}, byes=0;
    while (byes<2) {
        msg_hdr_t h; unsigned char buf[27]; assert(b->recv(s,&h,buf,sizeof buf)==0);
        assert(h.client_id>=1 && h.client_id<=2);
        if (h.type==MSG_BYE) { byes++; continue; }
        unsigned id=h.client_id-1, j=count[id]++;
        assert(j<depth && h.seq==total[id]+j && h.checksum==fnv1a32(buf,sizeof buf));
        batch[id][j]=h; memcpy(data[id][j],buf,sizeof buf);
        unsigned n=1000-total[id]<depth ? 1000-total[id] : depth;
        if (count[id]==n) {
            for (unsigned k=n;k>0;k--) {
                batch[id][k-1].type=MSG_RESP;
                assert(b->send(s,&batch[id][k-1],data[id][k-1])==0);
            }
            total[id]+=n; count[id]=0;
        }
    }
    assert(total[0]==1000 && total[1]==1000);
    reap(p[0]); reap(p[1]); b->destroy(s); b->close(s);
    printf("SHM: 2 clients x 1000 reverse responses, depth=%u, odd stride OK\n",depth);
}
static void faults(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-shm-fault-%d",getpid());
    ipc_config_t cfg={.size=3,.depth=1, .team=1};
    ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
    int shmid=shmget(ipc_team_key(1,2),0,0), semid=semget(ipc_team_key(1,3),0,0);
    assert(shmid>=0 && semid>=0);
    int ready[2]; assert(pipe(ready)==0);
    pid_t p=fork(); assert(p>=0);
    if (!p) {
        b->close(s); assert(close(ready[0])==0);
        ipc_channel_t *c=b->client_open(key,1,&cfg); assert(c);
        /* 토큰을 얻은 직후 죽은 생산자: SEM_UNDO/리셋으로 복원하면 안 된다. */
        struct sembuf op={.sem_num=SHM_SEM(0,SEM_REQ_EMPTY),.sem_op=-1};
        assert(semop(semid,&op,1)==0); assert(write(ready[1],"r",1)==1);
        for (;;) pause();
    }
    assert(close(ready[1])==0); char mark; assert(read(ready[0],&mark,1)==1); assert(close(ready[0])==0);
    assert(kill(p,SIGKILL)==0); int status; assert(waitpid(p,&status,0)==p && WIFSIGNALED(status));
    msg_hdr_t h=header(1,0); h.type=MSG_RESP; h.payload_len=0;
    assert(b->send(s,&h,NULL)==-1 && errno==EPIPE);
    assert(semctl(semid,SHM_SEM(0,SEM_REQ_EMPTY),GETVAL)==0);
    p=fork(); assert(p>=0); if (!p) { b->close(s); client(key,&cfg,2); }
    for (unsigned i=0;i<1000;i++) {
        unsigned char data[27]; assert(b->recv(s,&h,data,sizeof data)==0);
        assert(h.client_id==2 && h.seq==i && h.checksum==fnv1a32(data,sizeof data));
        h.type=MSG_RESP; assert(b->send(s,&h,data)==0);
    }
    assert(b->recv(s,&h,NULL,0)==0 && h.type==MSG_BYE); reap(p);
    atomic_store(&g_stop,true); assert(b->recv(s,&h,NULL,0)==-1 && errno==ECANCELED);
    atomic_store(&g_stop,false); b->destroy(s); b->close(s);
    struct shmid_ds ds; assert(shmctl(shmid,IPC_STAT,&ds)==-1);
    assert(semctl(semid,0,GETVAL)==-1);
    puts("SHM: killed token holder isolated, other ID 1000 matches, stop and resource removal OK");
}
static void clean(const char *key, int success)
{
    pid_t p=fork(); assert(p>=0);
    if (!p) { execl("./clean_ipc","clean_ipc",key,(char *)NULL); _exit(127); }
    int st; assert(waitpid(p,&st,0)==p && WIFEXITED(st));
    assert((WEXITSTATUS(st)==0)==success);
}
static void server_death(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-shm-death-%d",getpid());
    ipc_config_t cfg={.size=3,.depth=1, .team=1};
    int ready[2]; assert(pipe(ready)==0);
    pid_t p=fork(); assert(p>=0);
    if (!p) {
        assert(close(ready[0])==0);
        ipc_channel_t *s=b->server_open(key,&cfg); assert(s);
        assert(write(ready[1],"r",1)==1); for (;;) pause();
    }
    assert(close(ready[1])==0); char mark;
    assert(read(ready[0],&mark,1)==1); assert(close(ready[0])==0);
    int shmid=shmget(ipc_team_key(1,2),0,0), semid=semget(ipc_team_key(1,3),0,0);
    assert(shmid>=0 && semid>=0);
    ipc_channel_t *c=b->client_open(key,1,&cfg); assert(c);
    assert(kill(p,SIGKILL)==0); int st; assert(waitpid(p,&st,0)==p && WIFSIGNALED(st));
    alarm(5); msg_hdr_t h; assert(b->recv(c,&h,NULL,0)==-1 && errno==ECONNRESET);
    clean(key,0); /* 서버가 없어도 클라 잠금은 존중한다. */
    b->close(c); clean(key,1); alarm(30);
    struct shmid_ds ds; assert(shmctl(shmid,IPC_STAT,&ds)==-1);
    assert(semctl(semid,0,GETVAL)==-1);
    puts("SHM: server death <=5 seconds, live client prevents cleanup, kernel IDs removed OK");
}
static void collision(void)
{
    char key[128]; snprintf(key,sizeof key,"/tmp/ipcff-shm-collision-%d",getpid());
    FILE *f=fopen(key,"wx"); assert(f && fclose(f)==0);
    key_t sk=ipc_team_key(1,2), ek=ipc_team_key(1,3); assert(sk!=(key_t)-1 && ek!=(key_t)-1);
    int semid=semget(ek,SHM_SEM_COUNT,IPC_CREAT|IPC_EXCL|0600); assert(semid>=0);
    ipc_config_t cfg={.size=3,.depth=1, .team=1};
    assert(!b->server_open(key,&cfg) && errno==EEXIST);
    assert(semctl(semid,0,GETVAL)==0); /* 타 세마포어는 유지, 부분 생성 SHM은 회수. */
    assert(shmget(sk,0,0)==-1 && errno==ENOENT);
    assert(semctl(semid,0,IPC_RMID)==0); assert(unlink(key)==0);
    puts("SHM: unrecorded semaphore preserved and partial SHM creation removed OK");
}
int main(int argc, char **argv)
{
    alarm(30); b=ipc_lookup(argc > 1 ? argv[1] : "sysv_shm"); assert(b);
    reverse(1); reverse(4); reverse(16);
    if (!strcmp(b->name,"sysv_shm")) { faults(); server_death(); collision(); }
    return 0;
}
