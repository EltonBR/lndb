#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "discovery.h"
#include "connection.h"
#include "lndb_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int parse_ipv4(const char *text, uint32_t *host_order)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, text, &addr) != 1) return -1;
    *host_order = ntohl(addr.s_addr);
    return 0;
}

int lndb_client_ip_range(const char *spec, uint32_t *first_ip, uint32_t *last_ip)
{
    char buf[64]; size_t n = strlen(spec); if (!n || n >= sizeof(buf)) return -1;
    memcpy(buf, spec, n + 1);
    char *dash = strchr(buf, '-');
    if (dash) {
        if (strchr(dash + 1, '-')) return -1;
        *dash++ = '\0';
        if (parse_ipv4(buf, first_ip) < 0 || parse_ipv4(dash, last_ip) < 0 || *first_ip > *last_ip)
            return -1;
        return 0;
    }
    char *slash = strchr(buf, '/');
    if (slash) {
        *slash++ = '\0'; char *end;
        if (!*slash) return -1;
        long bits = strtol(slash, &end, 10);
        uint32_t host;
        if (*end || bits < 0 || bits > 32 || parse_ipv4(buf, &host) < 0) return -1;
        uint32_t mask = bits == 0 ? 0 : (bits == 32 ? UINT32_MAX : UINT32_MAX << (32 - (unsigned)bits));
        uint32_t net = host & mask, broadcast = net | ~mask;
        if (bits < 31) { *first_ip = net + 1; *last_ip = broadcast - 1; }
        else { *first_ip = net; *last_ip = broadcast; }
        return 0;
    }
    char *x = strrchr(buf, '.');
    if (!x || (strcmp(x + 1, "*") && strcmp(x + 1, "x"))) return -1;
    x[1] = '0'; x[2] = '\0'; uint32_t h;
    if (parse_ipv4(buf, &h) < 0 || (h & 255u) != 0) return -1;
    *first_ip = h + 1; *last_ip = h + 254;
    return 0;
}

static int probe(const char *host, const struct lndb_client_config *cfg)
{
    if (cfg->api_key_count == 0) {
        int fd = lndb_client_connect_host(host, cfg->port, cfg->connect_timeout_ms);
        if (fd < 0) return -1;
        uint8_t type, *payload = NULL; uint32_t len = 0;
        int ok = lndb_recv_frame(fd, &type, &payload, &len) == 0 &&
                 type == LNDB_MSG_HELLO && len == 0;
        free(payload); close(fd); return ok ? 0 : -1;
    }
    int tcp_connected = 0, identified = 0;
    int fd = lndb_client_connect_auth(host, cfg, 1, &tcp_connected, &identified);
    if (fd >= 0) { close(fd); return 1; }
    return identified ? 0 : -1;
}

static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    time_t sec = deadline->tv_sec - now.tv_sec;
    long nsec = deadline->tv_nsec - now.tv_nsec;
    if (nsec < 0) { --sec; nsec += 1000000000L; }
    if (sec < 0 || (sec == 0 && nsec <= 0)) return 0;
    if (sec > INT_MAX / 1000) return INT_MAX;
    long long ms = (long long)sec * 1000 + (nsec + 999999L) / 1000000L;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}

static char *device_map_path(const char *config_path)
{
    size_t n = strlen(config_path);
    char *path = malloc(n + sizeof(".devices"));
    if (path) snprintf(path, n + sizeof(".devices"), "%s.devices", config_path);
    return path;
}

/* Returns 1 when endpoint is an ID and resolves, 0 for a normal endpoint,
 * and -1 for a malformed or unknown ID. */
int lndb_client_resolve_device_id(const char *endpoint, const char *config_path,
                             char **host_out, uint16_t *port_out)
{
    const char *digits = endpoint;
    if (endpoint[0] == '#') digits++;
    else {
        int numeric = endpoint[0] != '\0';
        for (const char *p = endpoint; *p; ++p)
            if (*p < '0' || *p > '9') { numeric = 0; break; }
        if (!numeric) return 0;
    }
    if (!*digits) return -1;
    unsigned long wanted = 0;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9' || wanted > (ULONG_MAX - 9) / 10) return -1;
        wanted = wanted * 10 + (unsigned)(*p - '0');
    }
    if (!wanted || wanted > UINT_MAX) return -1;
    char *path = device_map_path(config_path);
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return -1;
    char line[128], address[INET_ADDRSTRLEN];
    unsigned id, port;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%u %15s %u", &id, address, &port) != 3) continue;
        if (id == (unsigned)wanted && port > 0 && port <= 65535) {
            *host_out = strdup(address);
            if (*host_out) { *port_out = (uint16_t)port; found = 1; }
            break;
        }
    }
    fclose(f);
    return found ? 1 : -1;
}

int lndb_client_devices(const struct lndb_client_config *cfg, const char *config_path)
{
    uint32_t first, last;
    if (lndb_client_ip_range(cfg->network, &first, &last) < 0) { fprintf(stderr, "invalid discovery network: %s\n", cfg->network); return 1; }
    char *map_path = device_map_path(config_path);
    if (!map_path) { perror("lndb: device map path"); return 1; }
    size_t map_len = strlen(map_path);
    char *tmp_path = malloc(map_len + sizeof(".tmp.XXXXXX"));
    if (!tmp_path) { free(map_path); perror("lndb: device map path"); return 1; }
    memcpy(tmp_path, map_path, map_len);
    memcpy(tmp_path + map_len, ".tmp.XXXXXX", sizeof(".tmp.XXXXXX"));
    int map_fd = mkstemp(tmp_path);
    if (map_fd < 0 || fchmod(map_fd, 0600) < 0) {
        int saved = errno;
        if (map_fd >= 0) close(map_fd);
        free(tmp_path); free(map_path); errno = saved;
        perror("lndb: creating device map");
        return 1;
    }
    FILE *map = fdopen(map_fd, "w");
    if (!map) {
        int saved = errno; close(map_fd); unlink(tmp_path);
        free(tmp_path); free(map_path); errno = saved;
        perror("lndb: creating device map");
        return 1;
    }
    printf("List of LNDB devices\n\n");
    unsigned device_id = 0;
    unsigned concurrency = cfg->concurrency ? cfg->concurrency : 1;
    if (concurrency > 256) concurrency = 256;
    for (uint64_t start = first; start <= (uint64_t)last; ) {
        uint64_t remaining = (uint64_t)last - start + 1;
        unsigned count = remaining > concurrency ? concurrency : (unsigned)remaining;
        int fds[256], pending[256], results[256];
        char hosts[256][INET_ADDRSTRLEN];
        struct pollfd pfds[256];
        struct timespec deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) {
            perror("lndb: discovery clock");
            fclose(map); unlink(tmp_path); free(tmp_path); free(map_path);
            return 1;
        }
        deadline.tv_sec += (time_t)(cfg->connect_timeout_ms / 1000);
        deadline.tv_nsec += (long)(cfg->connect_timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
        for (unsigned i = 0; i < count; ++i) {
            struct in_addr a; a.s_addr = htonl((uint32_t)(start + i));
            inet_ntop(AF_INET, &a, hosts[i], sizeof(hosts[i]));
            results[i] = -1;
            fds[i] = socket(AF_INET, SOCK_STREAM, 0);
            pending[i] = 0;
            pfds[i].fd = fds[i]; pfds[i].events = POLLOUT; pfds[i].revents = 0;
            if (fds[i] >= 0) {
                int fl = fcntl(fds[i], F_GETFL, 0);
                if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) < 0) {
                    close(fds[i]); fds[i] = pfds[i].fd = -1;
                    continue;
                }
                struct sockaddr_in sa; memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons(cfg->port); sa.sin_addr = a;
                if (connect(fds[i], (struct sockaddr *)&sa, sizeof(sa)) == 0) {
                    results[i] = 0;
                } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
                    pending[i] = 1;
                } else {
                    close(fds[i]); fds[i] = pfds[i].fd = -1;
                }
            }
        }
        for (;;) {
            int active = 0;
            for (unsigned i = 0; i < count; ++i) if (pending[i]) ++active;
            if (!active) break;
            int wait_ms = remaining_ms(&deadline);
            int pr = poll(pfds, count, wait_ms);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (!pr) break;
            for (unsigned i = 0; i < count; ++i) {
                if (!pending[i] || !pfds[i].revents) continue;
                int err = 0; socklen_t el = sizeof(err);
                if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                    results[i] = 0;
                pending[i] = 0;
                pfds[i].fd = -1;
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            int status = -1;
            if (fds[i] >= 0) {
                if (results[i] == 0) {
                    close(fds[i]); fds[i] = -1;
                    status = probe(hosts[i], cfg);
                } else {
                    close(fds[i]); fds[i] = -1;
                }
            }
            if (status == 1 || status == 0) {
                ++device_id;
                printf("%u    %s    %s\n", device_id, hosts[i],
                       status == 1 ? "device" : "unauthorized");
                if (fprintf(map, "%u %s %u\n", device_id, hosts[i],
                            (unsigned)cfg->port) < 0) {
                    perror("lndb: writing device map");
                    fclose(map); unlink(tmp_path); free(tmp_path); free(map_path);
                    return 1;
                }
            }
        }
        start += count;
    }
    int map_error = fflush(map) < 0 || fsync(fileno(map)) < 0;
    int saved = errno;
    if (fclose(map) < 0) { map_error = 1; saved = errno; }
    if (!map_error && rename(tmp_path, map_path) < 0) { map_error = 1; saved = errno; }
    if (map_error) {
        unlink(tmp_path); errno = saved; perror("lndb: saving device map");
    }
    free(tmp_path); free(map_path);
    if (map_error) return 1;
    return 0;
}

