#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200809L

#include "connection.h"
#include "discovery.h"
#include "shell.h"
#include "lndb_config.h"
#include "lndb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

enum peer_mode { PEER_SILENT, PEER_CLOSE_AFTER_AUTH, PEER_SILENT_AUTH, PEER_SLOW_HELLO };

static int make_listener(const char *address, uint16_t requested_port,
                         uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(requested_port);
    if (inet_pton(AF_INET, address, &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    *port = ntohs(sa.sin_port);
    return fd;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void serve_one(int listener, enum peer_mode mode)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) _exit(2);
    if (mode == PEER_SILENT) {
        sleep_ms(1200);
    } else if (mode == PEER_CLOSE_AFTER_AUTH || mode == PEER_SILENT_AUTH) {
        uint8_t type, *payload = NULL;
        uint32_t length = 0;
        if (lndb_send_frame(fd, LNDB_MSG_HELLO, NULL, 0) < 0 ||
            lndb_recv_frame(fd, &type, &payload, &length) < 0 ||
            type != LNDB_MSG_AUTH) {
            free(payload);
            close(fd);
            _exit(3);
        }
        free(payload);
        if (mode == PEER_SILENT_AUTH) sleep_ms(1200);
    } else {
        uint8_t hello[LNDB_FRAME_HEADER_SIZE] = { 'L', 'N', 'D', 'B', LNDB_PROTOCOL_VERSION, LNDB_MSG_HELLO };
        signal(SIGPIPE, SIG_IGN);
        for (size_t i = 0; i < sizeof(hello); ++i) {
            if (write(fd, hello + i, 1) != 1) break;
            sleep_ms(100);
        }
    }
    close(fd);
    close(listener);
    _exit(0);
}

static void serve_discovery_peer(int listener, int silent)
{
    for (int i = 0; i < 2; ++i) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) _exit(2);
        if (silent) {
            if (i == 1) sleep_ms(1200);
        } else if (i == 0) {
            /* First connection is the discovery TCP-only reachability probe. */
        } else {
            if (lndb_send_frame(fd, LNDB_MSG_HELLO, NULL, 0) < 0) {
                close(fd);
                _exit(3);
            }
            sleep_ms(100);
        }
        close(fd);
    }
    close(listener);
    _exit(0);
}

static void serve_slow_discovery_peer(int listener)
{
    for (int i = 0; i < 2; ++i) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) _exit(2);
        if (i == 1) {
            uint8_t hello[LNDB_FRAME_HEADER_SIZE] = { 'L', 'N', 'D', 'B', LNDB_PROTOCOL_VERSION, LNDB_MSG_HELLO };
            signal(SIGPIPE, SIG_IGN);
            for (size_t j = 0; j < sizeof(hello); ++j) {
                if (write(fd, hello + j, 1) != 1) break;
                sleep_ms(10);
            }
        }
        close(fd);
    }
    close(listener);
    _exit(0);
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int test_connect_auth_peer(enum peer_mode mode)
{
    struct lndb_client_config cfg;
    uint16_t port;
    int listener = make_listener("127.0.0.1", 0, &port);
    if (listener < 0) return -1;
    pid_t child = fork();
    if (child < 0) { close(listener); return -1; }
    if (child == 0) serve_one(listener, mode);
    close(listener);

    memset(&cfg, 0, sizeof(cfg));
    cfg.port = port;
    cfg.connect_timeout_ms = 250;
    cfg.api_keys = (char *[]){ (char *)TEST_KEY };
    cfg.api_key_count = 1;
    int connected = 0, identified = 0;
    long long started = monotonic_ms();
    int fd = lndb_client_connect_auth("127.0.0.1", &cfg,
                                      &connected, &identified);
    long long elapsed = monotonic_ms() - started;
    int expected_identified = mode == PEER_CLOSE_AFTER_AUTH || mode == PEER_SILENT_AUTH;
    int timing_ok = (mode == PEER_SILENT || mode == PEER_SILENT_AUTH || mode == PEER_SLOW_HELLO) ?
                    elapsed >= 150 && elapsed < 1000 : elapsed < 1500;
    int ok = fd < 0 && connected && identified == expected_identified && timing_ok;
    if (fd >= 0) close(fd);
    if (wait_child(child) < 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "client_test: connect_auth mode %d: fd=%d tcp=%d identified=%d elapsed=%lldms errno=%d\n",
                mode, fd, connected, identified, elapsed, errno);
        return -1;
    }
    return 0;
}

static int test_interactive_shell_disconnect(void)
{
    int sv[2], master, slave;
    char *slave_name;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        !(slave_name = ptsname(master)) || (slave = open(slave_name, O_RDWR | O_NOCTTY)) < 0) {
        if (master >= 0) close(master);
        close(sv[0]); close(sv[1]);
        return -1;
    }
    pid_t server = fork();
    if (server < 0) { close(master); close(slave); close(sv[0]); close(sv[1]); return -1; }
    if (server == 0) {
        uint8_t type, *payload = NULL; uint32_t length = 0;
        close(sv[0]); close(master); close(slave);
        if (lndb_recv_frame(sv[1], &type, &payload, &length) < 0 ||
            type != LNDB_MSG_SHELL_REQUEST) _exit(2);
        free(payload);
        close(sv[1]);
        _exit(0);
    }
    pid_t client = fork();
    if (client < 0) {
        close(master); close(slave); close(sv[0]); close(sv[1]);
        (void)kill(server, SIGTERM); (void)wait_child(server);
        return -1;
    }
    if (client == 0) {
        close(sv[1]); close(master);
        if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) _exit(3);
        if (slave > STDERR_FILENO) close(slave);
        int rc = lndb_client_shell_run(sv[0], 0, NULL);
        _exit(rc == 0 ? 4 : 0);
    }
    close(sv[0]); close(sv[1]); close(master); close(slave);
    int client_ok = wait_child(client) == 0;
    int server_ok = wait_child(server) == 0;
    return client_ok && server_ok ? 0 : -1;
}

static int test_discovery_slow_peers(void)
{
    uint16_t silent_port, hello_port, slow_port;
    int silent_listener = make_listener("127.0.0.1", 0, &silent_port);
    int hello_listener = silent_listener < 0 ? -1 :
                         make_listener("127.0.0.2", silent_port, &hello_port);
    int slow_listener = hello_listener < 0 ? -1 :
                        make_listener("127.0.0.3", silent_port, &slow_port);
    if (silent_listener < 0 || hello_listener < 0 || slow_listener < 0 ||
        silent_port != hello_port || silent_port != slow_port) {
        if (silent_listener >= 0) close(silent_listener);
        if (hello_listener >= 0) close(hello_listener);
        if (slow_listener >= 0) close(slow_listener);
        fprintf(stderr, "client_test: cannot reserve same discovery port on loopback aliases\n");
        return -1;
    }
    pid_t silent_child = fork();
    if (silent_child < 0) { close(silent_listener); close(hello_listener); return -1; }
    if (silent_child == 0) serve_discovery_peer(silent_listener, 1);
    close(silent_listener);
    pid_t hello_child = fork();
    if (hello_child < 0) {
        close(hello_listener);
        (void)kill(silent_child, SIGTERM);
        (void)wait_child(silent_child);
        return -1;
    }
    if (hello_child == 0) serve_discovery_peer(hello_listener, 0);
    close(hello_listener);
    pid_t slow_child = fork();
    if (slow_child < 0) {
        close(slow_listener);
        (void)kill(silent_child, SIGTERM);
        (void)kill(hello_child, SIGTERM);
        (void)wait_child(silent_child);
        (void)wait_child(hello_child);
        return -1;
    }
    if (slow_child == 0) serve_slow_discovery_peer(slow_listener);
    close(slow_listener);

    struct lndb_client_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = silent_port;
    cfg.connect_timeout_ms = 250;
    cfg.concurrency = 3;
    strcpy(cfg.network, "127.0.0.1-127.0.0.3");

    char config_path[] = "/tmp/lndb-client-test-XXXXXX";
    int config_fd = mkstemp(config_path);
    if (config_fd < 0) return -1;
    close(config_fd);
    long long started = monotonic_ms();
    int rc = lndb_client_devices(&cfg, config_path);
    long long elapsed = monotonic_ms() - started;

    char map_path[sizeof(config_path) + sizeof(".devices")];
    snprintf(map_path, sizeof(map_path), "%s.devices", config_path);
    FILE *map = fopen(map_path, "r");
    char line[128];
    int found_hello = 0, found_slow = 0, found_silent = 0;
    if (map) {
        while (fgets(line, sizeof(line), map)) {
            if (strstr(line, "127.0.0.2")) found_hello = 1;
            if (strstr(line, "127.0.0.3")) found_slow = 1;
            if (strstr(line, "127.0.0.1")) found_silent = 1;
        }
        fclose(map);
    }
    unlink(config_path);
    unlink(map_path);
    int children_ok = wait_child(silent_child) == 0 && wait_child(hello_child) == 0 && wait_child(slow_child) == 0;
    if (rc != 0 || !found_hello || !found_slow || found_silent || elapsed < 150 || elapsed >= 1500 || !children_ok) {
        fprintf(stderr, "client_test: discovery rc=%d hello=%d slow=%d silent=%d elapsed=%lldms children=%d\n",
                rc, found_hello, found_slow, found_silent, elapsed, children_ok);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_connect_auth_peer(PEER_SILENT) < 0 ||
        test_connect_auth_peer(PEER_SLOW_HELLO) < 0 ||
        test_connect_auth_peer(PEER_SILENT_AUTH) < 0 ||
        test_connect_auth_peer(PEER_CLOSE_AFTER_AUTH) < 0 ||
        test_interactive_shell_disconnect() < 0 ||
        test_discovery_slow_peers() < 0)
        return 1;
    puts("client regression tests passed");
    return 0;
}
