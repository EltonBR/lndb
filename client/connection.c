#define _POSIX_C_SOURCE 200809L
#include "connection.h"
#include "lndb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
            do rc = poll(&p, 1, (int)timeout_ms); while (rc < 0 && errno == EINTR);
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
                             int io_timeout, int *tcp_connected,
                             int *identified)
{
    if (!host || !cfg || !tcp_connected || !identified) return -1;
    *tcp_connected = 0;
    *identified = 0;
    for (size_t k = 0; k < cfg->api_key_count; ++k) {
        int fd = lndb_client_connect_host(host, cfg->port,
                                          cfg->connect_timeout_ms);
        if (fd < 0) return -1;
        *tcp_connected = 1;
        if (io_timeout) {
            struct timeval tv;
            tv.tv_sec = (time_t)(cfg->connect_timeout_ms / 1000);
            tv.tv_usec = (suseconds_t)((cfg->connect_timeout_ms % 1000) * 1000);
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        uint8_t type, *payload = NULL;
        uint32_t len = 0;
        if (lndb_recv_frame(fd, &type, &payload, &len) < 0 ||
            type != LNDB_MSG_HELLO || len != 0) {
            free(payload);
            close(fd);
            return -1;
        }
        free(payload);
        *identified = 1;
        size_t keylen = strlen(cfg->api_keys[k]);
        if (keylen > LNDB_FRAME_MAX_PAYLOAD ||
            lndb_send_frame(fd, LNDB_MSG_AUTH, cfg->api_keys[k],
                            (uint32_t)keylen) < 0) {
            close(fd);
            return -1;
        }
        if (lndb_recv_frame(fd, &type, &payload, &len) < 0) {
            close(fd);
            continue;
        }
        free(payload);
        if (type == LNDB_MSG_AUTH_OK && len == 0) return fd;
        close(fd);
    }
    return -1;
}
