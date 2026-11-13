#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/app.h"
#include "common/bqueue.h"
#include "common/filter.h"
#include "common/resource.h"

atomic_bool g_stop = false;
static options_t opt;
static ipc_channel_t *channel;
static bqueue_t jobs, done;
static sem_t permits;
static atomic_int error;
static atomic_bool receiver_done;
static atomic_uint_fast64_t responses;
static resource_t resources;

static void fail(int e)
{
    app_error(&error, e);
    bq_close(&jobs);
    bq_close(&done);
}

static void release(void)
{
    if (sem_post(&permits) == -1) fail(errno);
}

static void *receive_requests(void *arg)
{
    (void)arg;
    unsigned bye = 0;
    uint64_t received[SLOT_COUNT] = {0};
    size_t bytes = ipc_payload_size(&opt.ipc);
    while (!atomic_load(&g_stop)) {
        if (app_acquire(&permits) != 0) { fail(errno); break; }
        job_t job = { .payload = malloc(bytes) };
        if (!job.payload) { release(); fail(errno); break; }
        if (opt.backend->recv(channel, &job.hdr, job.payload, bytes) != 0) {
            int e = errno; free(job.payload); release(); fail(e); break;
        }
        msg_hdr_t *h = &job.hdr;
        if (h->type == MSG_BYE && h->payload_len == 0) {
            free(job.payload);
            release();
            if (h->checksum != fnv1a32(NULL, 0) ||
                (opt.clients && h->client_id > opt.clients)) { fail(EPROTO); break; }
            if (opt.count && received[h->client_id - 1] != (uint64_t)opt.count + opt.warmup) { fail(EPROTO); break; }
            bye |= 1u << (h->client_id - 1);
            if (opt.clients && (bye & ((1u << opt.clients) - 1)) == (1u << opt.clients) - 1) {
                atomic_store(&receiver_done, true);
                break;
            }
            continue;
        }
        int valid = app_valid_payload(h, MSG_REQ, &opt) &&
            (!opt.clients || h->client_id <= opt.clients) &&
            (!opt.count || (uint64_t)h->seq < (uint64_t)opt.count + opt.warmup) &&
            !(bye & (1u << (h->client_id - 1)));
        uint32_t checksum = fnv1a32(job.payload, h->payload_len);
        if (!valid || checksum != h->checksum) {
            free(job.payload); job.payload = NULL;
            h->type = MSG_ERR; h->payload_len = 0; h->checksum = fnv1a32(NULL, 0);
            if (bq_push(&done, &job) != 0) { release(); fail(errno); break; }
        } else {
            received[h->client_id - 1]++;
            if (bq_push(&jobs, &job) != 0) {
                int e = errno; free(job.payload); release(); fail(e); break;
            }
        }
    }
    return NULL;
}

static void *work(void *arg)
{
    (void)arg;
    job_t job;
    while (!atomic_load(&g_stop) && bq_pop(&jobs, &job) == 0) {
        int reuse = opt.ipc.payload_bytes || (opt.inplace_gray && job.hdr.filter == F_GRAY);
        uint8_t *out = reuse ? job.payload : malloc(job.hdr.payload_len);
        if (!out) { int e = errno; free(job.payload); release(); fail(e); break; }
        uint32_t size = job.hdr.width;
        if (!opt.ipc.payload_bytes) switch (job.hdr.filter) {
        case F_GRAY: filter_gray(job.payload, out, size, size); break;
        case F_BLUR: filter_blur(job.payload, out, size, size); break;
        case F_EDGE: filter_edge(job.payload, out, size, size); break;
        }
        if (!reuse) free(job.payload);
        job.payload = out;
        job.hdr.type = MSG_RESP;
        job.hdr.checksum = fnv1a32(out, job.hdr.payload_len);
        if (bq_push(&done, &job) != 0) {
            int e = errno; free(out); release(); fail(e); break;
        }
    }
    return NULL;
}

static void *respond(void *arg)
{
    (void)arg;
    job_t job;
    job_t held[SLOT_COUNT] = {0};
    uint32_t warmed[SLOT_COUNT] = {0}, ready = 0;
    while (!atomic_load(&g_stop) && bq_pop(&done, &job) == 0) {
        if (opt.cpu_csv && job.hdr.seq < opt.warmup && job.hdr.type == MSG_RESP &&
            ++warmed[job.hdr.client_id - 1] == opt.warmup) {
            held[job.hdr.client_id - 1] = job;
            if (++ready < opt.clients) continue;
            if (resource_start(&resources) < 0) { fail(errno); break; }
            for (uint32_t i = 0; i < opt.clients; i++) {
                int sent = opt.backend->send(channel, &held[i].hdr, held[i].payload);
                int saved = errno;
                free(held[i].payload); held[i].payload = NULL; release();
                if (sent < 0) { fail(saved); break; }
            }
            continue;
        }
        int rc = opt.backend->send(channel, &job.hdr, job.payload);
        int e = errno;
        free(job.payload);
        release();
        if (rc != 0) { if (e != EPIPE) { fail(e); break; } }
        else if (job.hdr.seq >= opt.warmup && job.hdr.type == MSG_RESP) {
            uint64_t n = atomic_fetch_add(&responses, 1) + 1;
            if (opt.cpu_csv && n == (uint64_t)opt.clients * opt.count && resource_end(&resources) < 0)
                fail(errno);
        }
    }
    for (unsigned i = 0; i < SLOT_COUNT; i++) free(held[i].payload);
    return NULL;
}

int main(int argc, char **argv)
{
    int rc = app_options(argc, argv, 1, &opt);
    if (rc > 0) return 0;
    if (rc < 0) { perror("server options"); return 1; }
    sigset_t signals;
    if (app_signals(&signals) != 0) { perror("server signals"); return 1; }
    unsigned capacity = SLOT_COUNT * opt.ipc.depth;
    if (bq_init(&jobs, capacity) != 0) { perror("server queue"); return 1; }
    if (bq_init(&done, capacity) != 0) { rc = errno; goto free_jobs; }
    if (sem_init(&permits, 0, capacity) == -1) { rc = errno; goto free_done; }
    channel = opt.backend->server_open(opt.key, &opt.ipc);
    if (!channel) { rc = errno; goto free_sem; }
    /* DECISION: worker 배열은 CLI 값에 맞춰 할당하고 생성 실패 시 생성분만 join한다. */
    pthread_t *workers = calloc(opt.workers, sizeof *workers), receiver, responder;
    uint32_t started = 0;
    int have_receiver = 0, have_responder = 0;
    if (!workers) fail(errno);
    if (!atomic_load(&g_stop)) {
        int e = pthread_create(&responder, NULL, respond, NULL);
        if (e) fail(e); else have_responder = 1;
    }
    while (started < opt.workers && !atomic_load(&g_stop)) {
        int e = pthread_create(&workers[started], NULL, work, NULL);
        if (e) fail(e); else started++;
    }
    if (!atomic_load(&g_stop)) {
        int e = pthread_create(&receiver, NULL, receive_requests, NULL);
        if (e) fail(e); else have_receiver = 1;
    }
    while (!atomic_load(&g_stop) && !atomic_load(&receiver_done)) {
        int signal = app_tick(&signals);
        if (signal < 0) fail(errno);
        else if (signal > 0) atomic_store(&g_stop, true);
    }
    atomic_store(&g_stop, true);
    bq_close(&jobs); bq_close(&done);
    if (have_receiver) { int e = pthread_join(receiver, NULL); if (e) app_error(&error, e); }
    for (uint32_t i = 0; i < started; i++) {
        int e = pthread_join(workers[i], NULL); if (e) app_error(&error, e);
    }
    if (have_responder) { int e = pthread_join(responder, NULL); if (e) app_error(&error, e); }
    free(workers);
    opt.backend->destroy(channel);
    opt.backend->close(channel);
    rc = atomic_load(&error);
    if (!rc && opt.cpu_csv && resource_save(&resources, opt.cpu_csv, 0, 0) < 0) rc = errno;
    if (!rc && printf("server: responses=%" PRIuFAST64 "\n", atomic_load(&responses)) < 0) rc = EIO;
free_sem:
    if (sem_destroy(&permits) == -1 && !rc) rc = errno;
free_done:
    bq_free(&done);
free_jobs:
    bq_free(&jobs);
    if (rc) { errno = rc; perror("server"); return 1; }
    return 0;
}
