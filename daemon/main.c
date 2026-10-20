#define _POSIX_C_SOURCE 200809L
#include "lndb_config.h"
#include "lndb_version.h"
#include "lndb_features.h"
#if LNDB_ENABLE_PON_BLINK
#include "blink_queue.h"
#endif
#include "session.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static const char *config_path = "/etc/lndbd/lndbd.conf";
static pid_t clients[128];
static size_t client_count;

#define CLIENT_LIMIT (sizeof(clients) / sizeof(clients[0]))

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
    int i, server;
#if LNDB_ENABLE_PON_BLINK
    struct lndbd_blink_queue blink_queue = {0, -1, -1, -1};
#endif
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
#if LNDB_ENABLE_PON_BLINK
    if (lndbd_blink_queue_start(&blink_queue, server) < 0) {
        perror("lndbd: start PON LED queue");
        close(server);
        lndbd_blink_queue_stop(&blink_queue);
        return 1;
    }
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#if LNDB_ENABLE_PON_BLINK
    /* Pipe writes from blink admission must report EPIPE if its worker exits. */
    signal(SIGPIPE, SIG_IGN);
#endif
    while (!stopping) {
        reap_clients();
#if LNDB_ENABLE_PON_BLINK
        lndbd_blink_queue_reap(&blink_queue);
#endif
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
        /* Reap before admission; a full table must not fork an untracked
         * process.  Closing the accepted socket is the overload response. */
        reap_clients();
        if (client_count >= CLIENT_LIMIT) {
            close(fd);
            continue;
        }
        pid_t child = fork();
        if (child == 0) {
            close(server);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGPIPE, SIG_IGN);
#if LNDB_ENABLE_PON_BLINK
            lndbd_session_serve(fd, cfg.api_key, &blink_queue);
#else
            lndbd_session_serve(fd, cfg.api_key);
#endif
            close(fd);
            _exit(0);
        }
        close(fd);
        if (child > 0) clients[client_count++] = child;
    }
    close(server);
    for (size_t i = 0; i < client_count; i++) kill(clients[i], SIGTERM);
    for (size_t i = 0; i < client_count; i++)
        while (waitpid(clients[i], NULL, 0) < 0 && errno == EINTR) {}
#if LNDB_ENABLE_PON_BLINK
    lndbd_blink_queue_stop(&blink_queue);
#endif
    return 0;
}
