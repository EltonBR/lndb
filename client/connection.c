#define _POSIX_C_SOURCE 200809L
#include "connection.h"
#include "lndb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct deadline { struct timespec at; };

static int deadline_start(struct deadline *d, unsigned timeout_ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, &d->at) < 0) return -1;
    d->at.tv_sec += timeout_ms / 1000;
    d->at.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (d->at.tv_nsec >= 1000000000L) { d->at.tv_sec++; d->at.tv_nsec -= 1000000000L; }
    return 0;
}

static int deadline_remaining(const struct deadline *d)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    time_t sec = d->at.tv_sec - now.tv_sec;
    long nsec = d->at.tv_nsec - now.tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    if (sec < 0 || (sec == 0 && nsec <= 0)) return 0;
    if (sec > INT_MAX / 1000) return INT_MAX;
    long long ms = (long long)sec * 1000 + (nsec + 999999L) / 1000000L;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static int wait_socket(int fd, short events, const struct deadline *d)
{
    struct pollfd p = { fd, events, 0 };
    for (;;) {
        int left = deadline_remaining(d), rc;
        if (!left) { errno = ETIMEDOUT; return -1; }
        rc = poll(&p, 1, left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc == 0) { errno = ETIMEDOUT; return -1; }
        if (rc < 0) return -1;
        if (p.revents & POLLNVAL) { errno = EBADF; return -1; }
        if (p.revents & POLLERR) {
            int err = 0; socklen_t n = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && err) errno = err;
            else errno = ECONNRESET;
            return -1;
        }
        if (p.revents & events) return 0;
        if (p.revents & POLLHUP) { errno = ECONNRESET; return -1; }
    }
}

static int timed_read_full(int fd, void *buffer, size_t length,
                           const struct deadline *d)
{
    uint8_t *p = buffer;
    while (length) {
        if (wait_socket(fd, POLLIN, d) < 0) return -1;
        ssize_t n = recv(fd, p, length, 0);
        if (n > 0) { p += (size_t)n; length -= (size_t)n; }
        else if (n == 0) { errno = ECONNRESET; return -1; }
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    return 0;
}

static int timed_write_full(int fd, const void *buffer, size_t length,
                            const struct deadline *d)
{
    const uint8_t *p = buffer;
    while (length) {
        if (wait_socket(fd, POLLOUT, d) < 0) return -1;
        ssize_t n;
#ifdef MSG_NOSIGNAL
        n = send(fd, p, length, MSG_NOSIGNAL);
#else
        n = send(fd, p, length, 0);
#endif
        if (n > 0) { p += (size_t)n; length -= (size_t)n; }
        else if (n == 0) { errno = EPIPE; return -1; }
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    return 0;
}

static int recv_control_frame(int fd, uint8_t *type, uint32_t *length,
                              const struct deadline *d)
{
    uint8_t header[LNDB_FRAME_HEADER_SIZE];
    if (timed_read_full(fd, header, sizeof(header), d) < 0) return -1;
    if (memcmp(header, "LNDB", 4) || header[4] != LNDB_PROTOCOL_VERSION) {
        errno = EPROTO; return -1;
    }
    *type = header[5];
    *length = lndb_get_u32(header + 6);
    if (*length > LNDB_FRAME_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    return 0;
}

static int send_control_frame(int fd, uint8_t type, const void *payload,
                              uint32_t length, const struct deadline *d)
{
    uint8_t header[LNDB_FRAME_HEADER_SIZE];
    memcpy(header, "LNDB", 4);
    header[4] = LNDB_PROTOCOL_VERSION;
    header[5] = type;
    lndb_put_u32(header + 6, length);
    if (timed_write_full(fd, header, sizeof(header), d) < 0) return -1;
    return length ? timed_write_full(fd, payload, length, d) : 0;
}

int lndb_client_wait_hello(int fd, unsigned timeout_ms)
{
    struct deadline d;
    uint8_t type;
    uint32_t length;
    if (deadline_start(&d, timeout_ms) < 0 ||
        recv_control_frame(fd, &type, &length, &d) < 0) return -1;
    if (type != LNDB_MSG_HELLO || length != 0) { errno = EPROTO; return -1; }
    return 0;
}

int lndb_client_connect_host(const char *host, uint16_t port,
                             unsigned timeout_ms)
{
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;

    int fd = -1;
    struct deadline deadline;
    if (deadline_start(&deadline, timeout_ms) < 0) { freeaddrinfo(res); return -1; }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        if (rc < 0) {
            struct pollfd p = { fd, POLLOUT, 0 };
            for (;;) {
                int left = deadline_remaining(&deadline);
                if (!left) { rc = 0; errno = ETIMEDOUT; break; }
                rc = poll(&p, 1, left);
                if (rc < 0 && errno == EINTR) continue;
                break;
            }
            int err = 0;
            socklen_t elen = sizeof(err);
            if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        if (fcntl(fd, F_SETFL, fl) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return fd;
}

int lndb_client_parse_endpoint(const char *endpoint, uint16_t default_port,
                               char **host_out, uint16_t *port_out)
{
    if (!endpoint || !host_out || !port_out) return -1;
    const char *host_start = endpoint, *port_text = NULL;
    size_t host_len, n = strlen(endpoint);
    char *host;
    unsigned long port;
    char *end;

    if (!n) return -1;
    if (endpoint[0] == '[') {
        const char *close = strchr(endpoint + 1, ']');
        if (!close || close == endpoint + 1) return -1;
        host_start = endpoint + 1;
        host_len = (size_t)(close - host_start);
        if (close[1]) {
            if (close[1] != ':' || !close[2]) return -1;
            port_text = close + 2;
        }
    } else {
        const char *colon = strchr(endpoint, ':');
        if (colon && colon == strrchr(endpoint, ':')) {
            host_len = (size_t)(colon - endpoint);
            port_text = colon + 1;
            if (!*port_text) return -1;
        } else host_len = n;
    }
    if (!host_len) return -1;
    host = malloc(host_len + 1);
    if (!host) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (port_text) {
        for (const char *p = port_text; *p; ++p) {
            if (*p < '0' || *p > '9') { free(host); return -1; }
        }
        port = strtoul(port_text, &end, 10);
        if (*end || port == 0 || port > 65535) { free(host); return -1; }
        *port_out = (uint16_t)port;
    } else *port_out = default_port;
    *host_out = host;
    return 0;
}

int lndb_client_valid_api_key(const char *key)
{
    if (!key || strlen(key) != 64) return 0;
    for (size_t i = 0; i < 64; ++i)
        if (!((key[i] >= '0' && key[i] <= '9') ||
              (key[i] >= 'a' && key[i] <= 'f') ||
              (key[i] >= 'A' && key[i] <= 'F'))) return 0;
    return 1;
}

int lndb_client_connect_auth(const char *host,
                             const struct lndb_client_config *cfg,
                             int *tcp_connected, int *identified)
{
    if (!host || !cfg || !tcp_connected || !identified) return -1;
    struct deadline deadline;
    *tcp_connected = 0;
    *identified = 0;
    if (deadline_start(&deadline, cfg->connect_timeout_ms) < 0) return -1;
    for (size_t k = 0; k < cfg->api_key_count; ++k) {
        int left = deadline_remaining(&deadline);
        if (!left) { errno = ETIMEDOUT; return -1; }
        int fd = lndb_client_connect_host(host, cfg->port, (unsigned)left);
        if (fd < 0) return -1;
        *tcp_connected = 1;
        uint8_t type;
        uint32_t len;
        if (recv_control_frame(fd, &type, &len, &deadline) < 0) {
            close(fd);
            return -1;
        }
        if (type != LNDB_MSG_HELLO || len != 0) {
            errno = EPROTO;
            close(fd);
            return -1;
        }
        *identified = 1;
        size_t keylen = strlen(cfg->api_keys[k]);
        if (keylen > LNDB_FRAME_MAX_PAYLOAD ||
            send_control_frame(fd, LNDB_MSG_AUTH, cfg->api_keys[k],
                               (uint32_t)keylen, &deadline) < 0) {
            close(fd);
            return -1;
        }
        if (recv_control_frame(fd, &type, &len, &deadline) < 0) {
            close(fd);
            continue;
        }
        if (type == LNDB_MSG_AUTH_OK && len == 0) return fd;
        close(fd);
    }
    return -1;
}
