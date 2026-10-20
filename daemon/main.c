#define _POSIX_C_SOURCE 200809L
#include "lndb_config.h"
#include "lndb_protocol.h"
#include "lndb_version.h"
#include "gpio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static const char *config_path = "/etc/lndbd/lndbd.conf";
static pid_t clients[128];
static size_t client_count;
static int blink_queue_fd = -1;
static int blink_slots_fd = -1;
static volatile sig_atomic_t blink_worker_stopping;

#define BLINK_QUEUE_CAPACITY 16U
#define BLINK_REQUEST_SIZE 8U

static void reap_clients(void)
{
    size_t i = 0;
    while (i < client_count) {
        pid_t r = waitpid(clients[i], NULL, WNOHANG);
        if (r == clients[i] || (r < 0 && errno == ECHILD)) {
            clients[i] = clients[--client_count];
        } else i++;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static void on_worker_signal(int sig)
{
    (void)sig;
    blink_worker_stopping = 1;
}

static int set_fd_flag(int fd, int command, int flag)
{
    int value = fcntl(fd, command == F_GETFL ? F_GETFL : F_GETFD);
    int set_command = command == F_GETFL ? F_SETFL : F_SETFD;
    if (value < 0) return -1;
    return fcntl(fd, set_command, value | flag);
}

static int read_pipe_record(int fd, uint8_t record[BLINK_REQUEST_SIZE])
{
    size_t used = 0;
    while (used < BLINK_REQUEST_SIZE) {
        ssize_t n = read(fd, record + used, BLINK_REQUEST_SIZE - used);
        if (n > 0) { used += (size_t)n; continue; }
        if (n == 0) return used == 0 ? 0 : -1;
        if (errno == EINTR) {
            if (blink_worker_stopping) return 0;
            continue;
        }
        return -1;
    }
    return 1;
}

static void blink_worker(int request_fd, int slot_fd)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_worker_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    for (;;) {
        uint8_t request[BLINK_REQUEST_SIZE];
        int status = read_pipe_record(request_fd, request);
        if (status <= 0 || blink_worker_stopping) break;
        uint32_t interval_ms = lndb_get_u32(request);
        uint32_t times = lndb_get_u32(request + 4);
        char error[160];
        if (lndbd_gpio_pon_blink(interval_ms, times, error, sizeof(error)) < 0)
            fprintf(stderr, "lndbd: queued PON LED blink failed: %s\n",
                    error[0] ? error : "GPIO15 operation failed");
        /* Free one admission slot after the request has finished. */
        uint8_t slot = 1;
        while (write(slot_fd, &slot, sizeof(slot)) < 0 && errno == EINTR) {}
        if (blink_worker_stopping) break;
    }
    close(request_fd);
    close(slot_fd);
    _exit(0);
}

static int send_error(int fd, const char *message)
{
    return lndb_send_frame(fd, LNDB_MSG_ERROR, message, (uint32_t)strlen(message));
}

static int authenticate(int fd, const char *key)
{
    uint8_t type, *payload = NULL;
    uint32_t len;
    if (lndb_send_frame(fd, LNDB_MSG_HELLO, NULL, 0) < 0 ||
        lndb_recv_frame(fd, &type, &payload, &len) < 0)
        return -1;
    int ok = type == LNDB_MSG_AUTH && len == strlen(key) &&
             memcmp(payload, key, len) == 0;
    free(payload);
    if (!ok) {
        send_error(fd, "unauthorized");
        return -1;
    }
    return lndb_send_frame(fd, LNDB_MSG_AUTH_OK, NULL, 0);
}

static void serve_client(int fd, const char *key, int queue_fd, int slots_fd);

static int listen_socket(const struct lndb_server_config *cfg)
{
    struct addrinfo hints, *res = NULL, *it;
    char port[8];
    int fd = -1, one = 1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port, sizeof(port), "%u", (unsigned)cfg->port);
    if (getaddrinfo(cfg->bind, port, &hints, &res) != 0)
        return -1;
    for (it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0 && listen(fd, 16) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char **argv)
{
    struct lndb_server_config cfg;
    char err[256];
    int i, server, request_pipe[2] = {-1, -1}, slot_pipe[2] = {-1, -1};
    pid_t worker_pid;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            puts("lndbd " LNDB_VERSION);
            return 0;
        }
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--version] [--config PATH]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "usage: %s [--version] [--config PATH]\n", argv[0]);
            return 2;
        }
    }
    if (lndb_server_config_load(config_path, &cfg, err, sizeof(err)) < 0) {
        fprintf(stderr, "lndbd: %s\n", err);
        return 1;
    }
    server = listen_socket(&cfg);
    if (server < 0) {
        perror("lndbd: listen");
        return 1;
    }
    if (pipe(request_pipe) < 0 || pipe(slot_pipe) < 0) {
        perror("lndbd: create PON LED queue");
        close(server);
        if (request_pipe[0] >= 0) close(request_pipe[0]);
        if (request_pipe[1] >= 0) close(request_pipe[1]);
        if (slot_pipe[0] >= 0) close(slot_pipe[0]);
        if (slot_pipe[1] >= 0) close(slot_pipe[1]);
        return 1;
    }
    if (set_fd_flag(request_pipe[0], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(request_pipe[1], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(slot_pipe[0], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(slot_pipe[1], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(request_pipe[1], F_GETFL, O_NONBLOCK) < 0 ||
        set_fd_flag(slot_pipe[0], F_GETFL, O_NONBLOCK) < 0) {
        perror("lndbd: configure PON LED queue");
        close(server); close(request_pipe[0]); close(request_pipe[1]);
        close(slot_pipe[0]); close(slot_pipe[1]);
        return 1;
    }
    {
        uint8_t slots[BLINK_QUEUE_CAPACITY];
        memset(slots, 1, sizeof(slots));
        if (lndb_write_full(slot_pipe[1], slots, sizeof(slots)) < 0) {
            perror("lndbd: initialize PON LED queue");
            close(server); close(request_pipe[0]); close(request_pipe[1]);
            close(slot_pipe[0]); close(slot_pipe[1]);
            return 1;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    worker_pid = fork();
    if (worker_pid < 0) {
        perror("lndbd: start PON LED worker");
        close(server); close(request_pipe[0]); close(request_pipe[1]);
        close(slot_pipe[0]); close(slot_pipe[1]);
        return 1;
    }
    if (worker_pid == 0) {
        close(server);
        close(request_pipe[1]);
        close(slot_pipe[0]);
        signal(SIGCHLD, SIG_DFL);
        blink_worker(request_pipe[0], slot_pipe[1]);
    }
    close(request_pipe[0]);
    close(slot_pipe[1]);
    blink_queue_fd = request_pipe[1];
    blink_slots_fd = slot_pipe[0];
    while (!stopping) {
        reap_clients();
        if (worker_pid > 0) {
            pid_t waited = waitpid(worker_pid, NULL, WNOHANG);
            if (waited == worker_pid || (waited < 0 && errno == ECHILD))
                worker_pid = 0;
        }
        struct pollfd waitfd = { server, POLLIN, 0 };
        int ready = poll(&waitfd, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("lndbd: poll");
            continue;
        }
        if (ready == 0) continue;
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (stopping) break;
            perror("lndbd: accept");
            continue;
        }
        pid_t child = fork();
        if (child == 0) {
            close(server);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGPIPE, SIG_IGN);
            serve_client(fd, cfg.api_key, blink_queue_fd, blink_slots_fd);
            close(fd);
            _exit(0);
        }
        close(fd);
        if (child > 0 && client_count < sizeof(clients) / sizeof(clients[0]))
            clients[client_count++] = child;
        else if (child > 0) {
            kill(child, SIGTERM);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    close(server);
    for (size_t i = 0; i < client_count; i++) kill(clients[i], SIGTERM);
    for (size_t i = 0; i < client_count; i++)
        while (waitpid(clients[i], NULL, 0) < 0 && errno == EINTR) {}
    close(blink_queue_fd);
    /* The worker completes its active pulse (restoring GPIO state), then
     * discards queued requests while shutting down. */
    if (worker_pid > 0) {
        kill(worker_pid, SIGTERM);
        while (waitpid(worker_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    close(blink_slots_fd);
    return 0;
}

static void serve_client(int fd, const char *key, int queue_fd, int slots_fd)
{
    uint8_t type, *payload = NULL;
    uint32_t len;
    if (authenticate(fd, key) < 0) return;
    if (lndb_recv_frame(fd, &type, &payload, &len) < 0) return;
    if (type == LNDB_MSG_CLOSE) { free(payload); return; }
    if (type == LNDB_MSG_SHELL_REQUEST) {
        extern int lndbd_shell(int, const uint8_t *, uint32_t);
        lndbd_shell(fd, payload, len);
    } else if (type == LNDB_MSG_FILE_REQUEST) {
        extern int lndbd_transfer(int, const uint8_t *, uint32_t);
        lndbd_transfer(fd, payload, len);
    } else if (type == LNDB_MSG_PON_BLINK) {
        if (len != 8) {
            send_error(fd, "invalid PON LED blink request");
        } else {
            uint32_t interval_ms = lndb_get_u32(payload);
            uint32_t times = lndb_get_u32(payload + 4);
            if (interval_ms < LNDB_PON_BLINK_MIN_INTERVAL_MS ||
                interval_ms > LNDB_PON_BLINK_MAX_INTERVAL_MS ||
                times < LNDB_PON_BLINK_MIN_TIMES || times > LNDB_PON_BLINK_MAX_TIMES ||
                (uint64_t)2 * interval_ms * times > LNDB_PON_BLINK_MAX_DURATION_MS) {
                send_error(fd, "invalid PON LED blink parameters");
            } else {
                uint8_t slot;
                uint8_t response[BLINK_REQUEST_SIZE];
                ssize_t n;
                do { n = read(slots_fd, &slot, sizeof(slot)); }
                while (n < 0 && errno == EINTR);
                if (n != (ssize_t)sizeof(slot)) {
                    send_error(fd, "PON LED blink queue is full");
                } else {
                    lndb_put_u32(response, interval_ms);
                    lndb_put_u32(response + 4, times);
                    do { n = write(queue_fd, response, sizeof(response)); }
                    while (n < 0 && errno == EINTR);
                    if (n != (ssize_t)sizeof(response)) {
                        int saved = errno;
                        while (write(slots_fd, &slot, sizeof(slot)) < 0 && errno == EINTR) {}
                        errno = saved;
                        send_error(fd, "PON LED blink queue is full");
                    } else {
                        lndb_send_frame(fd, LNDB_MSG_PON_BLINK_QUEUED, NULL, 0);
                    }
                }
            }
        }
    } else {
        send_error(fd, "unsupported operation");
    }
    free(payload);
}
