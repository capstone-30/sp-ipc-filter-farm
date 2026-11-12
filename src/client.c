#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/app.h"
#include "common/ppm.h"
#include "common/timer.h"
#include "common/resource.h"

atomic_bool g_stop = false;
static options_t opt;
static ipc_channel_t *channel;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress;
static sem_t inflight;
static atomic_int error;
static atomic_bool sender_done, receiver_done;
static uint8_t *input, *output;
static uint64_t *sent, *rtt, first_send, last_recv;
static uint32_t completed, total, input_checksum;
static size_t bytes;
static resource_t resources;

static void fail(int e) { app_error(&error, e); }

static int take_lock(void)
{
    int e = pthread_mutex_lock(&lock);
    if (e) { fail(e); return -1; }
    return 0;
}

static void unlock(void)
{
    int e = pthread_mutex_unlock(&lock);
    if (e) fail(e);
}

/* lock를 잡은 호출자만 사용한다. 모든 조건 대기는 stop을 100ms마다 확인한다. */
static int wait_progress(void)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) { fail(errno); return -1; }
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    int e = pthread_cond_timedwait(&progress, &lock, &deadline);
    if (e && e != ETIMEDOUT) { fail(e); return -1; }
    return 0;
}

static void *send_requests(void *arg)
{
    (void)arg;
    msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = opt.id,
        .filter = opt.filter, .width = opt.ipc.size, .height = opt.ipc.size,
        .channels = 3, .payload_len = (uint32_t)bytes, .checksum = input_checksum };
    for (uint32_t seq = 0; seq < total && !atomic_load(&g_stop); seq++) {
        if (seq == opt.warmup) {
            if (take_lock() != 0) return NULL;
            while (completed < opt.warmup && !atomic_load(&g_stop))
                if (wait_progress() != 0) break;
            unlock();
            if (atomic_load(&g_stop)) return NULL;
            if (!opt.warmup && resource_start(&resources) < 0) { fail(errno); return NULL; }
        }
        if (app_acquire(&inflight) != 0) { fail(errno); return NULL; }
        h.seq = seq;
        if (take_lock() != 0) return NULL;
        h.t_send_ns = sent[seq] = now_ns();
        if (seq == opt.warmup) first_send = h.t_send_ns;
        unlock();
        if (opt.backend->send(channel, &h, input) != 0) { fail(errno); return NULL; }
    }
    if (take_lock() != 0) return NULL;
    while (completed < total && !atomic_load(&g_stop))
        if (wait_progress() != 0) break;
    int success = completed == total && !atomic_load(&g_stop);
    unlock();
    if (success) {
        h.type = MSG_BYE; h.payload_len = 0; h.checksum = fnv1a32(NULL, 0);
        if (opt.backend->send(channel, &h, NULL) != 0) { fail(errno); return NULL; }
        atomic_store(&sender_done, true);
    }
    return NULL;
}

static void *receive_responses(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        msg_hdr_t h;
        if (opt.backend->recv(channel, &h, output, bytes) != 0) { fail(errno); break; }
        uint32_t checksum = fnv1a32(output, h.payload_len);
        if (!app_valid_payload(&h, MSG_RESP, &opt) || h.client_id != opt.id ||
            h.filter != opt.filter || h.seq >= total ||
            checksum != h.checksum || (opt.ipc.payload_bytes && checksum != input_checksum)) {
            fail(EPROTO); break;
        }
        if (take_lock() != 0) break;
        if (sent[h.seq] == 0 || h.t_send_ns != sent[h.seq]) {
            unlock(); fail(EPROTO); break;
        }
        uint64_t finished = now_ns();
        if (finished < sent[h.seq]) { unlock(); fail(EPROTO); break; }
        if (h.seq >= opt.warmup) rtt[h.seq - opt.warmup] = finished - sent[h.seq];
        sent[h.seq] = 0; /* 완료한 seq는 다시 수신할 수 없다. */
        if (h.seq >= opt.warmup) last_recv = finished;
        completed++;
        int all = completed == total;
        if (opt.warmup && completed == opt.warmup && resource_start(&resources) < 0) fail(errno);
        if (all && resource_end(&resources) < 0) fail(errno);
        /* 체크섬과 seq 검증 후에만 송신 허용량을 돌려준다. */
        if (sem_post(&inflight) == -1) fail(errno);
        int e = pthread_cond_broadcast(&progress);
        if (e) fail(e);
        unlock();
        if (all) { atomic_store(&receiver_done, true); break; }
    }
    return NULL;
}

static void *report_progress(void *arg)
{
    (void)arg;
    uint64_t next = now_ns() + 1000000000ull;
    if (take_lock() != 0) return NULL;
    while (!atomic_load(&g_stop) && completed < total) {
        if (wait_progress() != 0) break;
        if (!opt.quiet && now_ns() >= next) {
            uint32_t count = completed > opt.warmup ? completed - opt.warmup : 0;
            unlock();
            if (printf("client %u: %u/%u\n", opt.id, count, opt.count) < 0) fail(EIO);
            if (take_lock() != 0) return NULL;
            next = now_ns() + 1000000000ull;
        }
    }
    unlock();
    return NULL;
}

static int save_results(void)
{
    if (opt.csv) {
        FILE *f = fopen(opt.csv, "w");
        if (!f) return -1;
        int saved = 0;
        if (fprintf(f, "client_id,seq,rtt_ns,payload_bytes\n") < 0) saved = errno;
        for (uint64_t i = 0; i < opt.count && !saved; i++)
            if (fprintf(f, "%u,%" PRIu64 ",%" PRIu64 ",%zu\n", opt.id, i + opt.warmup, rtt[i], bytes) < 0)
                saved = errno ? errno : EIO;
        if (fclose(f) != 0 && !saved) saved = errno;
        if (saved) { errno = saved; return -1; }
        char path[4096];
        int n = snprintf(path, sizeof path, "%s.cpu.csv", opt.csv);
        if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; return -1; }
        if (resource_save(&resources, path, first_send, last_recv) < 0) return -1;
    }
    if (!opt.no_save) {
        /* DECISION: 합성 입력 재사용이므로 마지막 응답과 입력 한 쌍만 저장한다. */
        const char *names[] = {"input", "output"};
        const uint8_t *images[] = {input, output};
        for (unsigned i = 0; i < 2; i++) {
            char path[64];
            int n = snprintf(path, sizeof path, "client-%u-%s.ppm", opt.id, names[i]);
            if (n < 0 || (size_t)n >= sizeof path) { errno = EOVERFLOW; return -1; }
            if (ppm_save(path, images[i], opt.ipc.size, opt.ipc.size) != 0) return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = app_options(argc, argv, 0, &opt);
    if (rc > 0) return 0;
    if (rc < 0) { perror("client options"); return 1; }
    total = opt.count + opt.warmup; /* app_options에서 uint32_t 범위를 확인했다. */
    sigset_t signals;
    if (app_signals(&signals) != 0) { perror("client signals"); return 1; }
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc) goto finish;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_cond_init(&progress, &attr);
    int e = pthread_condattr_destroy(&attr);
    if (rc) goto finish;
    if (e) { rc = e; goto free_cond; }
    if (sem_init(&inflight, 0, opt.ipc.depth) == -1) { rc = errno; goto free_cond; }
    bytes = ipc_payload_size(&opt.ipc);
    input = opt.ipc.payload_bytes ? malloc(bytes) : ppm_gen(opt.ipc.size, opt.ipc.size);
    output = malloc(bytes);
    sent = calloc(total, sizeof *sent);
    rtt = calloc(opt.count, sizeof *rtt);
    if (!input || !output || !sent || !rtt) { rc = ENOMEM; goto free_buffers; }
    if (opt.ipc.payload_bytes)
        for (size_t i = 0; i < bytes; i++) input[i] = (uint8_t)(i * 131u + opt.id);
    input_checksum = fnv1a32(input, bytes);
    channel = opt.backend->client_open(opt.key, opt.id, &opt.ipc);
    if (!channel) { rc = errno; goto free_buffers; }
    pthread_t threads[3];
    void *(*routines[])(void *) = {receive_responses, report_progress, send_requests};
    size_t started = 0;
    while (started < 3 && !atomic_load(&g_stop)) {
        e = pthread_create(&threads[started], NULL, routines[started], NULL);
        if (e) fail(e); else started++;
    }
    while (!atomic_load(&g_stop) && !(atomic_load(&sender_done) && atomic_load(&receiver_done))) {
        int signal = app_tick(&signals);
        if (signal < 0) fail(errno);
        else if (signal > 0) atomic_store(&g_stop, true);
    }
    atomic_store(&g_stop, true);
    for (size_t i = 0; i < started; i++) { e = pthread_join(threads[i], NULL); if (e) fail(e); }
    opt.backend->close(channel);
    rc = atomic_load(&error);
    if (!rc && atomic_load(&sender_done) && atomic_load(&receiver_done)) {
        if (save_results() != 0) rc = errno;
        else {
            samples_t samples = { .v = rtt, .n = opt.count, .cap = opt.count };
            stats_report(&samples, (uint64_t)bytes * opt.count, (last_recv - first_send) / 1e9);
            if (opt.ipc.payload_bytes && printf("basic: messages/s=%.6f bytes/s=%.6f control_bytes/direction=48\n",
                opt.count * 1e9 / (last_recv - first_send), (double)bytes * opt.count * 1e9 / (last_recv - first_send)) < 0) rc = EIO;
            if (printf("client %u: matched=%u mismatches=0 warmup=%u\n", opt.id, opt.count, opt.warmup) < 0) rc = EIO;
            if (fflush(stdout) != 0 || ferror(stdout)) rc = EIO;
        }
    } else if (!rc && printf("client %u: interrupted, matched=%u/%u\n", opt.id,
        completed > opt.warmup ? completed - opt.warmup : 0, opt.count) < 0)
        rc = EIO;
free_buffers:
    free(input); free(output); free(sent); free(rtt);
    if (sem_destroy(&inflight) == -1 && !rc) rc = errno;
free_cond:
    e = pthread_cond_destroy(&progress); if (e && !rc) rc = e;
finish:
    e = pthread_mutex_destroy(&lock); if (e && !rc) rc = e;
    if (rc) { errno = rc; perror("client"); return 1; }
    return 0;
}
