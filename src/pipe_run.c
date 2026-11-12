/* launcher는 IPC fd만 전달한다. 응용과 필터 동작은 server/client를 그대로 쓴다. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define MAX_ARGS 64
static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { interrupted = sig; }

static int number(const char *s, unsigned max, unsigned *out)
{
    char *end; errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *s == '-' || *end || errno || !n || n > max) { errno = EINVAL; return -1; }
    *out = (unsigned)n; return 0;
}

static int redirect_log(const char *dir, const char *name, const char *suffix, int target)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s.%s", dir, name, suffix);
    if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; return -1; }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    if (dup2(fd, target) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return close(fd);
}

static int child(int fds[MAX_CLIENTS][4], unsigned clients, unsigned id, unsigned slots,
                 const char *executable, char **args, const char *output)
{
    char env[512]; size_t off = 0;
    for (unsigned i = 0; i < slots; i++) {
        int rd = -1, wr = -1;
        if (i < clients) {
            if (!id) { rd = fds[i][0]; wr = fds[i][3]; }
            else if (i == id - 1) { rd = fds[i][2]; wr = fds[i][1]; }
            for (unsigned k = 0; k < 4; k++)
                if (fds[i][k] != rd && fds[i][k] != wr && close(fds[i][k]) < 0) return -1;
        }
        int n = snprintf(env + off, sizeof env - off, "%d:%d,", rd, wr);
        if (n < 0 || (size_t)n >= sizeof env - off) { errno = EOVERFLOW; return -1; }
        off += (size_t)n;
    }
    if (setenv("IPCFF_PIPE_FDS", env, 1) < 0) return -1;
    if (output) {
        char name[32];
        if (id) snprintf(name, sizeof name, "client-%u", id);
        else snprintf(name, sizeof name, "server");
        if (redirect_log(output, name, "log", STDOUT_FILENO) < 0 ||
            redirect_log(output, name, "err", STDERR_FILENO) < 0) return -1;
    }
    execv(executable, args);
    return -1;
}

int main(int argc, char **argv)
{
    unsigned clients = 0, basic = 0;
    const char *output = NULL, *filters = "gray", *warmup = "0";
    char *server_args[MAX_ARGS] = {0}, *client_args[MAX_ARGS] = {0};
    int ns = 0, nc = 0, have_key = 0;
    char key[128], server_path[PATH_MAX], client_path[PATH_MAX];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help")) {
            puts("usage: pipe_run [--basic] --clients=N [--output=dir] [--filter=gray,edge]"
                 " --count=N [--warmup=N] [--size=N|--bytes=N] [--depth=N] [--team=N]"
                 " [--quiet] [--no-save] [--workers=N] [--inplace-gray] [--key=path]");
            return 0;
        }
        if (!strcmp(a, "--basic")) { basic = 1; continue; }
        if (!strncmp(a, "--clients=", 10)) { if (clients || number(a + 10, 8, &clients) < 0) goto bad; continue; }
        if (!strncmp(a, "--output=", 9)) { if (output || !a[9]) goto bad; output = a + 9; continue; }
        if (!strncmp(a, "--filter=", 9)) { filters = a + 9; continue; }
        if (ns >= MAX_ARGS - 10 || nc >= MAX_ARGS - 10) goto bad;
        if (!strcmp(a, "--no-save")) { client_args[++nc] = argv[i]; continue; }
        if (!strncmp(a, "--workers=", 10) || !strcmp(a, "--inplace-gray") || !strncmp(a, "--cpu-csv=", 10)) {
            server_args[++ns] = argv[i]; continue;
        }
        if (strncmp(a, "--count=", 8) && strncmp(a, "--warmup=", 9) && strncmp(a, "--team=", 7) &&
            strncmp(a, "--size=", 7) && strncmp(a, "--bytes=", 8) && strncmp(a, "--depth=", 8) &&
            strncmp(a, "--key=", 6) && strcmp(a, "--quiet")) goto bad;
        if (!strncmp(a, "--key=", 6)) have_key = 1;
        if (!strncmp(a, "--warmup=", 9)) warmup = a + 9;
        server_args[++ns] = argv[i]; client_args[++nc] = argv[i];
    }
    if (!clients || clients > (basic ? 8u : 4u)) goto bad;
    if (!have_key) {
        snprintf(key, sizeof key, "--key=/tmp/ipcff-pipe-%ld", (long)getpid());
        server_args[++ns] = key; client_args[++nc] = key;
    }
    const char *slash = strrchr(argv[0], '/');
    int prefix = slash ? (int)(slash - argv[0] + 1) : 0;
    int n = snprintf(server_path, sizeof server_path, "%.*s%sserver", prefix, argv[0], basic ? "basic_" : "");
    if (n < 0 || (size_t)n >= sizeof server_path) goto bad;
    n = snprintf(client_path, sizeof client_path, "%.*s%sclient", prefix, argv[0], basic ? "basic_" : "");
    if (n < 0 || (size_t)n >= sizeof client_path) goto bad;
    server_args[0] = server_path; client_args[0] = client_path;
    server_args[++ns] = "--ipc=pipe"; client_args[++nc] = "--ipc=pipe";
    char clients_arg[32], cpu_arg[PATH_MAX];
    snprintf(clients_arg, sizeof clients_arg, "--clients=%u", clients);
    server_args[++ns] = clients_arg;
    if (output) {
        if (mkdir(output, 0700) < 0 && errno != EEXIST) { perror("pipe_run output"); return 1; }
        if (strcmp(warmup, "0")) {
            n = snprintf(cpu_arg, sizeof cpu_arg, "--cpu-csv=%s/server.cpu.csv", output);
            if (n < 0 || (size_t)n >= sizeof cpu_arg) goto bad;
            server_args[++ns] = cpu_arg;
        }
    }
    char *filter_copy = strdup(filters), *filter[MAX_CLIENTS];
    if (!filter_copy) { perror("pipe_run"); return 1; }
    unsigned nf = 0;
    for (char *s = strtok(filter_copy, ","); s; s = strtok(NULL, ",")) {
        if (nf == MAX_CLIENTS || (strcmp(s, "gray") && strcmp(s, "blur") && strcmp(s, "edge"))) { free(filter_copy); goto bad; }
        filter[nf++] = s;
    }
    if (!nf || (nf != 1 && nf != clients)) { free(filter_copy); goto bad; }
    struct sigaction sa = {.sa_handler = on_signal};
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        free(filter_copy); perror("pipe_run signal"); return 1;
    }
    int fds[MAX_CLIENTS][4];
    for (unsigned i = 0; i < MAX_CLIENTS; i++) for (unsigned k = 0; k < 4; k++) fds[i][k] = -1;
    unsigned pipes = 0, children = 0;
    pid_t pids[MAX_CLIENTS + 1] = {0};
    int failed = 0;
    for (; pipes < clients; pipes++) {
        if (pipe(&fds[pipes][0]) < 0 || pipe(&fds[pipes][2]) < 0) { failed = 1; break; }
    }
    if (!failed) for (unsigned id = 0; id <= clients; id++) {
        if (interrupted) { failed = 1; break; }
        pid_t pid = fork();
        if (pid < 0) { failed = 1; break; }
        if (!pid) {
            char id_arg[32], filter_arg[64], csv_arg[PATH_MAX];
            char **args = id ? client_args : server_args;
            if (id) {
                int k = nc;
                snprintf(id_arg, sizeof id_arg, "--id=%u", id);
                snprintf(filter_arg, sizeof filter_arg, "--filter=%s", filter[nf == 1 ? 0 : id - 1]);
                args[++k] = id_arg; args[++k] = filter_arg;
                if (output) {
                    n = snprintf(csv_arg, sizeof csv_arg, "--csv=%s/client-%u.csv", output, id);
                    if (n < 0 || (size_t)n >= sizeof csv_arg) { errno = ENAMETOOLONG; perror("pipe_run"); _exit(127); }
                    args[++k] = csv_arg;
                }
                args[k + 1] = NULL;
            }
            if (child(fds, clients, id, basic ? 8 : 4, id ? client_path : server_path, args, output) < 0)
                perror("pipe_run child");
            _exit(127);
        }
        pids[children++] = pid;
    }
    for (unsigned i = 0; i < clients; i++) for (unsigned k = 0; k < 4; k++)
        if (fds[i][k] >= 0 && close(fds[i][k]) < 0) failed = 1;
    free(filter_copy);
    if (failed) perror("pipe_run setup");
    unsigned remaining = children, stopping = 0;
    struct timespec nap = {.tv_nsec = 10000000};
    while (remaining) {
        if ((failed || interrupted) && !stopping) {
            for (unsigned i = 0; i < children; i++) if (pids[i] > 0 && kill(pids[i], SIGINT) < 0 && errno != ESRCH) failed = 1;
            stopping = 1;
        }
        for (unsigned i = 0; i < children; i++) {
            if (pids[i] <= 0) continue;
            int status;
            pid_t got = waitpid(pids[i], &status, WNOHANG);
            if (got > 0) {
                if (!WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
                pids[i] = 0; remaining--;
            } else if (got < 0 && errno != EINTR) { failed = 1; pids[i] = 0; remaining--; }
        }
        if (stopping && ++stopping == 501)
            for (unsigned i = 0; i < children; i++) if (pids[i] > 0 && kill(pids[i], SIGKILL) < 0 && errno != ESRCH) failed = 1;
        if (remaining && nanosleep(&nap, NULL) < 0 && errno != EINTR) failed = 1;
    }
    return failed || interrupted ? 1 : 0;
bad:
    fprintf(stderr, "pipe_run: invalid options (see --help)\n"); return 1;
}
