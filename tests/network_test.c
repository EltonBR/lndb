#define _POSIX_C_SOURCE 200809L
#include "lndb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TEST_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TEST_TIMEOUT_SECONDS 30

static volatile sig_atomic_t daemon_child = -1;

static void on_test_timeout(int sig)
{
    (void)sig;
    if (daemon_child > 0) (void)kill((pid_t)daemon_child, SIGTERM);
    _exit(124);
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, p + done, length - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int reserve_port(uint16_t *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    *port = ntohs(addr.sin_port);
    close(fd);
    return 0;
}

static int connect_local(uint16_t port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int connect_ready(uint16_t port, pid_t daemon_pid)
{
    struct timespec pause = {0, 50000000L};
    for (int i = 0; i < 100; ++i) {
        int fd = connect_local(port);
        if (fd >= 0) return fd;
        int status;
        pid_t r = waitpid(daemon_pid, &status, WNOHANG);
        if (r == daemon_pid) { errno = ECHILD; return -1; }
        (void)nanosleep(&pause, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int receive_expected(int fd, uint8_t expected, uint8_t **payload,
                            uint32_t *length)
{
    uint8_t type;
    if (lndb_recv_frame(fd, &type, payload, length) < 0) return -1;
    if (type != expected) {
        free(*payload);
        *payload = NULL;
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int contains_bytes(const uint8_t *haystack, size_t haystack_len,
                          const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) return 0;
    for (size_t i = 0; i <= haystack_len - needle_len; ++i)
        if (memcmp(haystack + i, needle, needle_len) == 0) return 1;
    return 0;
}

static int set_socket_timeout(int fd, unsigned seconds)
{
    struct timeval tv;
    tv.tv_sec = (time_t)seconds;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int authenticate(int fd, const char *key, uint8_t expected_type)
{
    uint8_t *payload = NULL;
    uint32_t length = 0;
    if (receive_expected(fd, LNDB_MSG_HELLO, &payload, &length) < 0) return -1;
    free(payload);
    if (length != 0 || lndb_send_frame(fd, LNDB_MSG_AUTH, key,
                                       (uint32_t)strlen(key)) < 0)
        return -1;
    if (receive_expected(fd, expected_type, &payload, &length) < 0) return -1;
    if (expected_type == LNDB_MSG_ERROR) {
        int valid = length == strlen("unauthorized") &&
                    memcmp(payload, "unauthorized", length) == 0;
        free(payload);
        if (!valid) { errno = EPROTO; return -1; }
    } else {
        int valid = length == 0;
        free(payload);
        if (!valid) { errno = EPROTO; return -1; }
    }
    return 0;
}

static int test_shell(uint16_t port)
{
    int fd = -1, result = -1;
    uint8_t *payload = NULL;
    uint32_t length = 0;
    int saw_stdout = 0, saw_exit = 0;
    fd = connect_local(port);
    if (fd < 0 || set_socket_timeout(fd, 5) < 0 ||
        authenticate(fd, TEST_KEY, LNDB_MSG_AUTH_OK) < 0)
        goto done;
    const char command[] = "printf 'network-shell-ok\\n'; exit 7";
    uint8_t request[sizeof(command)];
    request[0] = 1;
    memcpy(request + 1, command, sizeof(command) - 1);
    if (lndb_send_frame(fd, LNDB_MSG_SHELL_REQUEST, request,
                        (uint32_t)sizeof(request)) < 0 ||
        lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0)
        goto done;
    for (;;) {
        uint8_t type;
        if (lndb_recv_frame(fd, &type, &payload, &length) < 0) goto done;
        if (type == LNDB_MSG_SHELL_STDOUT) {
            if (contains_bytes(payload, length, "network-shell-ok")) saw_stdout = 1;
        } else if (type == LNDB_MSG_SHELL_EXIT) {
            saw_exit = length == 4 && lndb_get_u32(payload) == 7;
            free(payload);
            payload = NULL;
            break;
        } else {
            free(payload);
            payload = NULL;
            errno = EPROTO;
            goto done;
        }
        free(payload);
        payload = NULL;
    }
    if (saw_stdout && saw_exit) result = 0;
    else errno = EPROTO;
done:
    free(payload);
    if (fd >= 0) close(fd);
    return result;
}

static int test_auth_timeout(uint16_t port)
{
    struct pollfd pfd;
    int fd = connect_local(port);
    if (fd < 0 || set_socket_timeout(fd, 6) < 0) goto fail;
    uint8_t *payload = NULL;
    uint32_t length = 0;
    if (receive_expected(fd, LNDB_MSG_HELLO, &payload, &length) < 0 || length != 0) {
        free(payload);
        goto fail;
    }
    free(payload);
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;
    int ready;
    do { ready = poll(&pfd, 1, 6000); } while (ready < 0 && errno == EINTR);
    if (ready <= 0) { if (ready == 0) errno = ETIMEDOUT; goto fail; }
    char byte;
    ssize_t n;
    do { n = recv(fd, &byte, sizeof(byte), 0); } while (n < 0 && errno == EINTR);
    if (n == 0) { close(fd); return 0; }
    errno = n > 0 ? EPROTO : errno;
fail:
    if (fd >= 0) close(fd);
    return -1;
}

static void stop_daemon(pid_t pid)
{
    int status;
    if (pid <= 0) return;
    (void)kill(pid, SIGTERM);
    for (int i = 0; i < 40; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return;
        struct timespec pause = {0, 50000000L};
        (void)nanosleep(&pause, NULL);
    }
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

int main(int argc, char **argv)
{
    char temp[] = "/tmp/lndb-network-test-XXXXXX";
    char config[512] = "";
    char config_text[512];
    uint16_t port = 0;
    pid_t daemon_pid = -1;
    int rc = 1;
    int fd = -1;
    int config_fd = -1;

    (void)signal(SIGALRM, on_test_timeout);
    alarm(TEST_TIMEOUT_SECONDS);
    if (argc != 2) { fprintf(stderr, "usage: %s PATH_TO_LNDBD\n", argv[0]); return 2; }
    if (!mkdtemp(temp)) { perror("mkdtemp"); goto cleanup; }
    if (snprintf(config, sizeof(config), "%s/lndbd.conf", temp) >= (int)sizeof(config)) {
        fprintf(stderr, "temporary config path too long\n"); goto cleanup;
    }
    if (reserve_port(&port) < 0) { perror("reserve port"); goto cleanup; }
    int n = snprintf(config_text, sizeof(config_text),
                     "[server]\nbind = 127.0.0.1\nport = %u\n\n[auth]\napi_key = %s\n",
                     (unsigned)port, TEST_KEY);
    if (n < 0 || (size_t)n >= sizeof(config_text)) { fprintf(stderr, "config too large\n"); goto cleanup; }
    config_fd = open(config, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (config_fd < 0 || write_all(config_fd, config_text, (size_t)n) < 0 || close(config_fd) < 0) {
        config_fd = -1; perror("write config"); goto cleanup;
    }
    config_fd = -1;
    daemon_pid = fork();
    if (daemon_pid < 0) { perror("fork"); goto cleanup; }
    if (daemon_pid == 0) {
        alarm(0);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDOUT_FILENO);
            (void)dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
        execl(argv[1], argv[1], "--config", config, (char *)NULL);
        _exit(127);
    }
    daemon_child = (sig_atomic_t)daemon_pid;
    fd = connect_ready(port, daemon_pid);
    if (fd < 0) { perror("daemon readiness"); goto cleanup; }
    close(fd);
    fd = -1;

    fd = connect_local(port);
    if (fd < 0 || set_socket_timeout(fd, 3) < 0 ||
        authenticate(fd, TEST_KEY, LNDB_MSG_AUTH_OK) < 0) {
        perror("valid authentication"); goto cleanup;
    }
    (void)lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0);
    close(fd); fd = -1;

    fd = connect_local(port);
    if (fd < 0 || set_socket_timeout(fd, 3) < 0 ||
        authenticate(fd, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                     LNDB_MSG_ERROR) < 0) {
        perror("invalid authentication"); goto cleanup;
    }
    close(fd); fd = -1;

    if (test_shell(port) < 0) { perror("remote shell command"); goto cleanup; }
    if (test_auth_timeout(port) < 0) { perror("auth timeout"); goto cleanup; }
    puts("local TCP daemon auth, shell, and auth-timeout checks passed");
    rc = 0;
cleanup:
    if (fd >= 0) close(fd);
    if (config_fd >= 0) close(config_fd);
    stop_daemon(daemon_pid);
    daemon_child = -1;
    if (config[0]) unlink(config);
    if (temp[0]) rmdir(temp);
    alarm(0);
    return rc;
}
