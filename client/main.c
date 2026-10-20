#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "lndb_config.h"
#include "lndb_protocol.h"
#include "lndb_version.h"
#include "transfer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_winch;
static void on_winch(int sig) { (void)sig; got_winch = 1; }

static int mkdir_parents(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0700) < 0 && errno != EEXIST) { free(copy); return -1; }
        *p = '/';
    }
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) { free(copy); return -1; }
    free(copy);
    return 0;
}

static char *user_config_path(void)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    char *owned_base = NULL;
    if (!base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') { errno = ENOENT; return NULL; }
        size_t n = strlen(home) + sizeof("/.config");
        owned_base = malloc(n);
        if (!owned_base) return NULL;
        snprintf(owned_base, n, "%s/.config", home);
        base = owned_base;
    }
    size_t n = strlen(base) + sizeof("/lndb/lndb.conf");
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s/lndb/lndb.conf", base);
    free(owned_base);
    if (!path) return NULL;
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir_parents(path) < 0) { int saved = errno; free(path); errno = saved; return NULL; }
        *slash = '/';
    }
    return path;
}

static char *default_config_path(void)
{
    return user_config_path();
}

/* Create a private, per-user client config on first use. */
static int create_client_config_if_missing(const char *path)
{
    static const char contents[] =
        "[client]\n"
        "port = 5555\n"
        "connect_timeout_ms = 500\n"
        "\n"
        "[discovery]\n"
        "network = 192.168.10.*\n"
        "concurrency = 32\n"
        "\n"
        "[auth]\n"
        "api_key_list =\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno == EEXIST ? 0 : -1;
    size_t done = 0;
    while (done < sizeof(contents) - 1) {
        ssize_t n = write(fd, contents + done, sizeof(contents) - 1 - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { if (n == 0) errno = EIO; goto create_fail; }
    }
    if (fchmod(fd, 0600) < 0 || fsync(fd) < 0) {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0) {
        int saved = errno;
        unlink(path);
        errno = saved;
        return -1;
    }
    return 0;
create_fail:
    {
        int saved = errno;
        close(fd);
        unlink(path);
        errno = saved;
        return -1;
    }
}

static int connect_host(const char *host, uint16_t port, unsigned timeout_ms)
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
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { close(fd); fd = -1; continue; }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        if (rc < 0) {
            struct pollfd p = { fd, POLLOUT, 0 };
            do rc = poll(&p, 1, (int)timeout_ms); while (rc < 0 && errno == EINTR);
            int err = 0; socklen_t elen = sizeof(err);
            if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
                close(fd); fd = -1; continue;
            }
        }
        if (fcntl(fd, F_SETFL, fl) < 0) { close(fd); fd = -1; continue; }
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int parse_endpoint(const char *endpoint, uint16_t default_port,
                          char **host_out, uint16_t *port_out)
{
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

static int valid_api_key(const char *key)
{
    if (strlen(key) != 64) return 0;
    for (size_t i = 0; i < 64; ++i)
        if (!((key[i] >= '0' && key[i] <= '9') ||
              (key[i] >= 'a' && key[i] <= 'f') ||
              (key[i] >= 'A' && key[i] <= 'F'))) return 0;
    return 1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else { if (!n) errno = EIO; return -1; }
    }
    return 0;
}

/* Replace just the key-list setting, preserving all other client config text. */
static int append_config_key(const char *path, const struct lndb_client_config *cfg,
                             const char *key)
{
    struct stat st;
    FILE *in = NULL;
    char *line = NULL, *tmp = NULL;
    size_t cap = 0;
    ssize_t n;
    int out = -1, rc = -1, in_auth = 0, found = 0;

    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    in = fopen(path, "r");
    if (!in) return -1;
    size_t path_len = strlen(path);
    tmp = malloc(path_len + sizeof(".tmp.XXXXXX"));
    if (!tmp) goto done;
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp.XXXXXX", sizeof(".tmp.XXXXXX"));
    out = mkstemp(tmp);
    if (out < 0) goto done;
    if (fchmod(out, 0600) < 0) goto done;
    while ((n = getline(&line, &cap, in)) >= 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '[') {
            char *end = strchr(s, ']');
            in_auth = end && (size_t)(end - s) == 5 && !strncmp(s, "[auth", 5);
        }
        if (in_auth) {
            char *eq = strchr(s, '=');
            if (eq) {
                char *k = s;
                while (k < eq && (*k == ' ' || *k == '\t')) ++k;
                char *ke = eq;
                while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
                if ((size_t)(ke - k) == strlen("api_key_list") &&
                    !strncmp(k, "api_key_list", (size_t)(ke - k))) {
                    if (found) { errno = EINVAL; goto done; }
                    found = 1;
                    if (write_all(out, "api_key_list = ", sizeof("api_key_list = ") - 1) < 0) goto done;
                    for (size_t i = 0; i < cfg->api_key_count; ++i) {
                        if (i && write_all(out, ",", 1) < 0) goto done;
                        if (write_all(out, cfg->api_keys[i], strlen(cfg->api_keys[i])) < 0) goto done;
                    }
                    if (cfg->api_key_count && write_all(out, ",", 1) < 0) goto done;
                    if (write_all(out, key, strlen(key)) < 0 || write_all(out, "\n", 1) < 0) goto done;
                    continue;
                }
            }
        }
        if (write_all(out, line, (size_t)n) < 0) goto done;
    }
    if (ferror(in) || !found) { errno = EINVAL; goto done; }
    if (fsync(out) < 0) goto done;
    if (close(out) < 0) { out = -1; goto done; }
    out = -1;
    if (rename(tmp, path) < 0) goto done;
    rc = 0;
done:
    {
        int saved = errno;
        if (out >= 0) close(out);
        if (in) fclose(in);
        if (tmp && rc < 0) unlink(tmp);
        free(tmp); free(line); errno = saved;
    }
    return rc;
}

/* A successful return leaves an authenticated connection open. */
static int connect_auth(const char *host, const struct lndb_client_config *cfg,
                        int io_timeout, int *tcp_connected, int *identified)
{
    *tcp_connected = 0;
    *identified = 0;
    for (size_t k = 0; k < cfg->api_key_count; ++k) {
        int fd = connect_host(host, cfg->port, cfg->connect_timeout_ms);
        if (fd < 0) return -1;
        *tcp_connected = 1;
        if (io_timeout) {
            struct timeval tv;
            tv.tv_sec = (time_t)(cfg->connect_timeout_ms / 1000);
            tv.tv_usec = (suseconds_t)((cfg->connect_timeout_ms % 1000) * 1000);
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        uint8_t type, *payload = NULL; uint32_t len = 0;
        if (lndb_recv_frame(fd, &type, &payload, &len) < 0 || type != LNDB_MSG_HELLO || len != 0) {
            free(payload); close(fd); return -1;
        }
        free(payload); *identified = 1;
        size_t keylen = strlen(cfg->api_keys[k]);
        if (keylen > LNDB_FRAME_MAX_PAYLOAD ||
            lndb_send_frame(fd, LNDB_MSG_AUTH, cfg->api_keys[k], (uint32_t)keylen) < 0) { close(fd); return -1; }
        if (lndb_recv_frame(fd, &type, &payload, &len) < 0) { close(fd); continue; }
        free(payload);
        if (type == LNDB_MSG_AUTH_OK && len == 0) return fd;
        close(fd);
    }
    return -1;
}

static int set_raw(int fd, struct termios *old)
{
    if (!isatty(fd) || tcgetattr(fd, old) < 0) return -1;
    struct termios t = *old;
    t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSAFLUSH, &t);
}

static int send_resize(int fd, int tty_fd)
{
    struct winsize ws;
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) < 0) memset(&ws, 0, sizeof(ws));
    uint8_t b[8];
    b[0] = (uint8_t)(ws.ws_row >> 8); b[1] = (uint8_t)ws.ws_row;
    b[2] = (uint8_t)(ws.ws_col >> 8); b[3] = (uint8_t)ws.ws_col;
    b[4] = b[5] = b[6] = b[7] = 0;
    return lndb_send_frame(fd, LNDB_MSG_SHELL_RESIZE, b, sizeof(b));
}

static int shell_run(int fd, int argc, char **argv)
{
    int interactive = argc == 0 && isatty(STDIN_FILENO);
    int script_interactive = 0, tty_fd = -1;
    if (argc == 0 && !interactive) {
        tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty_fd >= 0 && isatty(tty_fd)) script_interactive = 1;
        else if (tty_fd >= 0) { close(tty_fd); tty_fd = -1; }
    }
    const char *default_command = "/bin/sh";
    size_t clen = (argc == 0 && !interactive && !script_interactive) ? strlen(default_command) : 0;
    for (int i = 0; i < argc; ++i) clen += strlen(argv[i]) + (i != 0);
    char *cmd = malloc(clen + 1);
    if (!cmd) { if (tty_fd >= 0) close(tty_fd); return -1; }
    size_t pos = 0;
    if (argc == 0 && !interactive && !script_interactive) { memcpy(cmd, default_command, clen); pos = clen; }
    for (int i = 0; i < argc; ++i) { if (i) cmd[pos++] = ' '; size_t n = strlen(argv[i]); memcpy(cmd + pos, argv[i], n); pos += n; }
    cmd[pos] = '\0';
    uint8_t *req = malloc(clen + 1);
    if (!req) { free(cmd); if (tty_fd >= 0) close(tty_fd); return -1; }
    req[0] = script_interactive ? 2 : (interactive ? 0 : 1); memcpy(req + 1, cmd, clen); free(cmd);
    if (lndb_send_frame(fd, LNDB_MSG_SHELL_REQUEST, req, (uint32_t)(clen + 1)) < 0) {
        free(req); if (tty_fd >= 0) close(tty_fd); return -1;
    }
    free(req);
    if (script_interactive) {
        int rc = -1;
        uint8_t b[4096];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, b, sizeof(b));
            if (n > 0) {
                if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) goto script_interactive_done;
            } else if (n == 0) break;
            else if (errno == EINTR) continue;
            else goto script_interactive_done;
        }
        if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) goto script_interactive_done;
        /* Fall through to the PTY loop, using the controlling terminal even
         * though stdin itself was redirected from the script. */
        {
            struct termios old;
            int raw = set_raw(tty_fd, &old) == 0;
            struct sigaction sa, oldsa;
            memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_winch; sigemptyset(&sa.sa_mask);
            int have_oldsa = sigaction(SIGWINCH, &sa, &oldsa) == 0;
            (void)send_resize(fd, tty_fd);
            int done = 0, status = 0;
            while (!done) {
                if (got_winch) { got_winch = 0; if (send_resize(fd, tty_fd) < 0) { status = 1; break; } }
                struct pollfd pfd[2] = {{ fd, POLLIN, 0 }, { tty_fd, POLLIN, 0 }};
                int r = poll(pfd, 2, -1);
                if (r < 0) { if (errno == EINTR) continue; status = 1; break; }
                if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                    uint8_t type, *p = NULL; uint32_t n = 0;
                    if (lndb_recv_frame(fd, &type, &p, &n) < 0) { status = 1; break; }
                    if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                        if (n && lndb_write_full(tty_fd, p, n) < 0) status = 1;
                    } else if (type == LNDB_MSG_SHELL_EXIT) {
                        if (n == 4) status = (int)lndb_get_u32(p);
                        done = 1;
                    } else if (type == LNDB_MSG_ERROR) {
                        if (n) (void)lndb_write_full(tty_fd, p, n);
                        status = 1; done = 1;
                    }
                    free(p);
                    if (status == 1 && !done) break;
                }
                if (!done && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                    ssize_t n = read(tty_fd, b, sizeof(b));
                    if (n <= 0) {
                        (void)lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0);
                        done = 1;
                    } else if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) {
                        status = 1; done = 1;
                    }
                }
            }
            if (have_oldsa) (void)sigaction(SIGWINCH, &oldsa, NULL);
            if (raw) (void)tcsetattr(tty_fd, TCSAFLUSH, &old);
            rc = status;
        }
script_interactive_done:
        close(tty_fd);
        return rc;
    }
    if (!interactive) {
        int input_open = 1, status = 255;
        for (;;) {
            struct pollfd pfd[2] = {
                { fd, POLLIN, 0 },
                { input_open ? STDIN_FILENO : -1, POLLIN, 0 }
            };
            int ready = poll(pfd, 2, -1);
            if (ready < 0) { if (errno == EINTR) continue; return -1; }

            if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                uint8_t type, *p = NULL; uint32_t n = 0;
                if (lndb_recv_frame(fd, &type, &p, &n) < 0) return status;
                if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                    int out = type == LNDB_MSG_SHELL_STDERR ? STDERR_FILENO : STDOUT_FILENO;
                    if (n && lndb_write_full(out, p, n) < 0) { free(p); return -1; }
                } else if (type == LNDB_MSG_SHELL_EXIT && n == 4) {
                    uint32_t v = lndb_get_u32(p);
                    status = v > 255 ? 255 : (int)v;
                    free(p);
                    return status;
                } else if (type == LNDB_MSG_ERROR) {
                    if (n) (void)lndb_write_full(STDERR_FILENO, p, n);
                    free(p);
                    return 1;
                }
                free(p);
            }

            if (input_open && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                uint8_t b[4096];
                ssize_t n = read(STDIN_FILENO, b, sizeof(b));
                if (n > 0) {
                    if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) return -1;
                } else if (n == 0) {
                    if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) return -1;
                    input_open = 0;
                } else if (errno != EINTR) {
                    if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) return -1;
                    input_open = 0;
                }
            }
        }
    }
    tty_fd = STDIN_FILENO;
    struct termios old; int raw = set_raw(tty_fd, &old) == 0;
    struct sigaction sa, oldsa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_winch; sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, &oldsa); (void)send_resize(fd, tty_fd);
    int done = 0, status = 0;
    while (!done) {
        if (got_winch) { got_winch = 0; if (send_resize(fd, tty_fd) < 0) break; }
        struct pollfd pfd[2] = {{ fd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 }};
        int r = poll(pfd, 2, -1); if (r < 0) { if (errno == EINTR) continue; break; }
        if (pfd[0].revents & (POLLIN | POLLHUP)) {
            uint8_t type, *p; uint32_t n;
            if (lndb_recv_frame(fd, &type, &p, &n) < 0) break;
            if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                int out = type == LNDB_MSG_SHELL_STDERR ? STDERR_FILENO : STDOUT_FILENO;
                if (n && lndb_write_full(out, p, n) < 0) { free(p); break; }
            } else if (type == LNDB_MSG_SHELL_EXIT) { if (n == 4) status = (int)lndb_get_u32(p); done = 1; }
            else if (type == LNDB_MSG_ERROR) { if (n) (void)lndb_write_full(STDERR_FILENO, p, n); done = 1; status = 1; }
            free(p);
        }
        if (!done && (pfd[1].revents & (POLLIN | POLLHUP))) {
            uint8_t b[4096]; ssize_t n = read(STDIN_FILENO, b, sizeof(b));
            if (n <= 0) { (void)lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0); done = 1; }
            else if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) break;
        }
    }
    sigaction(SIGWINCH, &oldsa, NULL); if (raw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    return status;
}

static int parse_ipv4(const char *text, uint32_t *host_order)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, text, &addr) != 1) return -1;
    *host_order = ntohl(addr.s_addr);
    return 0;
}

static int ip_range(const char *spec, uint32_t *first_ip, uint32_t *last_ip)
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
        int fd = connect_host(host, cfg->port, cfg->connect_timeout_ms);
        if (fd < 0) return -1;
        uint8_t type, *payload = NULL; uint32_t len = 0;
        int ok = lndb_recv_frame(fd, &type, &payload, &len) == 0 &&
                 type == LNDB_MSG_HELLO && len == 0;
        free(payload); close(fd); return ok ? 0 : -1;
    }
    int tcp_connected = 0, identified = 0;
    int fd = connect_auth(host, cfg, 1, &tcp_connected, &identified);
    if (fd >= 0) { close(fd); return 1; }
    return identified ? 0 : -1;
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
static int resolve_device_id(const char *endpoint, const char *config_path,
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

static int devices(const struct lndb_client_config *cfg, const char *config_path)
{
    uint32_t first, last;
    if (ip_range(cfg->network, &first, &last) < 0) { fprintf(stderr, "invalid discovery network: %s\n", cfg->network); return 1; }
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
        int fds[256]; char hosts[256][INET_ADDRSTRLEN]; struct pollfd pfds[256];
        for (unsigned i = 0; i < count; ++i) {
            struct in_addr a; a.s_addr = htonl((uint32_t)(start + i));
            inet_ntop(AF_INET, &a, hosts[i], sizeof(hosts[i]));
            fds[i] = socket(AF_INET, SOCK_STREAM, 0); pfds[i].fd = fds[i]; pfds[i].events = POLLOUT; pfds[i].revents = 0;
            if (fds[i] >= 0) { int fl = fcntl(fds[i], F_GETFL, 0); fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
                struct sockaddr_in sa; memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons(cfg->port); sa.sin_addr = a;
                if (connect(fds[i], (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) { close(fds[i]); fds[i] = pfds[i].fd = -1; }
            }
        }
        int pr = poll(pfds, count, (int)cfg->connect_timeout_ms); if (pr < 0 && errno == EINTR) pr = 0;
        for (unsigned i = 0; i < count; ++i) {
            int status = -1;
            if (fds[i] >= 0 && pfds[i].revents) {
                int err = 0; socklen_t el = sizeof(err);
                if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
                    close(fds[i]); status = probe(hosts[i], cfg);
                }
                if (fds[i] >= 0) close(fds[i]);
            } else if (fds[i] >= 0) close(fds[i]);
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

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  lndb --version\n"
            "  lndb [--config FILE] devices\n"
            "  lndb [--config FILE] add device HOST[:PORT] API_KEY\n"
            "  lndb [--config FILE] TARGET shell [COMMAND...]\n"
            "  lndb [--config FILE] TARGET push SOURCE DESTINATION\n"
            "  lndb [--config FILE] TARGET pull SOURCE DESTINATION\n"
            "  lndb [--config FILE] TARGET blink INTERVAL_MS TIMES\n"
            "      submits the blink request to the daemon queue and exits\n"
            "\n"
            "TARGET is an IP address (optionally with :PORT) or a numeric ID\n"
            "from the latest 'lndb devices' scan.\n"
            "\n"
            "Examples:\n"
            "  lndb 192.168.10.29 shell uname -a\n"
            "  lndb 1 blink 100 10\n"
            "  lndb 1 shell\n"
            "  lndb 1 push ./file /tmp/\n"
            "  lndb 1 pull /tmp/logfile.log .\n");
}

static int parse_blink_arg(const char *text, uint32_t min, uint32_t max,
                           uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (!text || !*text || *text == '-' || *text == '+') return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed < min || parsed > max) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int pon_blink(int fd, uint32_t interval_ms, uint32_t times)
{
    uint8_t payload[8];
    lndb_put_u32(payload, interval_ms);
    lndb_put_u32(payload + 4, times);
    if (lndb_send_frame(fd, LNDB_MSG_PON_BLINK, payload, sizeof(payload)) < 0) return -1;
    for (;;) {
        uint8_t type, *payload = NULL;
        uint32_t len = 0;
        if (lndb_recv_frame(fd, &type, &payload, &len) < 0) return -1;
        if (type == LNDB_MSG_ERROR) {
            if (len) (void)lndb_write_full(STDERR_FILENO, payload, len);
            (void)lndb_write_full(STDERR_FILENO, "\n", 1);
            free(payload);
            errno = EIO;
            return -1;
        }
        free(payload);
        if (type == LNDB_MSG_PON_BLINK_QUEUED && len == 0) return 0;
        errno = EPROTO;
        return -1;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL; int arg = 1;
    while (arg < argc) {
        if (!strcmp(argv[arg], "--config") && arg + 1 < argc) { config_path = argv[arg + 1]; arg += 2; }
        else break;
    }
    if (arg < argc && !strcmp(argv[arg], "--version")) {
        puts("lndb " LNDB_VERSION);
        return 0;
    }
    if (arg < argc && !strcmp(argv[arg], "--help")) {
        print_usage(stdout);
        return 0;
    }
    char *auto_path = NULL;
    if (!config_path) { auto_path = default_config_path(); config_path = auto_path; }
    struct lndb_client_config cfg; char err[256];
    if (config_path && create_client_config_if_missing(config_path) < 0) {
        fprintf(stderr, "lndb: cannot create config file %s: %s\n", config_path, strerror(errno));
        free(auto_path);
        return 2;
    }
    if (!config_path || lndb_client_config_load(config_path, &cfg, err, sizeof(err)) < 0) {
        fprintf(stderr, "lndb: %s\n", config_path ? err : "cannot locate executable directory"); free(auto_path); return 2;
    }
    int rc = 1;
    if (arg >= argc) { print_usage(stderr); goto out; }
    if (!strcmp(argv[arg], "add")) {
        if (arg + 3 >= argc || strcmp(argv[arg + 1], "device") || arg + 4 != argc) {
            fprintf(stderr, "usage: lndb [--config FILE] add device HOST[:PORT] API_KEY\n"); rc = 2; goto out;
        }
        const char *endpoint = argv[arg + 2], *key = argv[arg + 3];
        char *host = NULL;
        uint16_t port;
        if (!valid_api_key(key)) {
            fprintf(stderr, "lndb: API key must contain exactly 64 hexadecimal characters\n"); rc = 2; goto out;
        }
        if (parse_endpoint(endpoint, cfg.port, &host, &port) < 0) {
            fprintf(stderr, "lndb: invalid endpoint: %s\n", endpoint); rc = 2; goto out;
        }
        char *single_key[] = { (char *)key };
        struct lndb_client_config one = cfg;
        one.port = port; one.api_keys = single_key; one.api_key_count = 1;
        int tcp_connected = 0, identified = 0;
        int fd = connect_auth(host, &one, 1, &tcp_connected, &identified);
        free(host);
        if (fd < 0) {
            fprintf(stderr, "lndb: %s: %s\n", endpoint,
                    identified ? "API key rejected by LNDB device" :
                    tcp_connected ? "TCP connected, but the peer did not send a valid LNDB1 greeting" :
                    "could not establish a TCP connection to the device");
            rc = 1; goto out;
        }
        close(fd);
        for (size_t i = 0; i < cfg.api_key_count; ++i) {
            if (!strcmp(cfg.api_keys[i], key)) {
                printf("LNDB device authenticated; API key is already configured in %s\n", config_path);
                rc = 0; goto out;
            }
        }
        if (append_config_key(config_path, &cfg, key) < 0) {
            perror("lndb: updating client config"); rc = 1; goto out;
        }
        printf("LNDB device authenticated; API key added to %s\n", config_path);
        rc = 0;
    }
    else if (!strcmp(argv[arg], "devices")) rc = devices(&cfg, config_path);
    else if (arg + 1 < argc) {
        const char *endpoint = argv[arg++], *op = argv[arg++];
        char *host = NULL; uint16_t port; int tcp_connected = 0, identified = 0;
        uint32_t blink_interval = 0, blink_times = 0;
        if (!strcmp(op, "blink") &&
            (argc - arg != 2 ||
             parse_blink_arg(arg < argc ? argv[arg] : NULL,
                             LNDB_PON_BLINK_MIN_INTERVAL_MS,
                             LNDB_PON_BLINK_MAX_INTERVAL_MS,
                             &blink_interval) < 0 ||
             parse_blink_arg(arg + 1 < argc ? argv[arg + 1] : NULL,
                             LNDB_PON_BLINK_MIN_TIMES,
                             LNDB_PON_BLINK_MAX_TIMES,
                             &blink_times) < 0 ||
             (uint64_t)2 * blink_interval * blink_times > LNDB_PON_BLINK_MAX_DURATION_MS)) {
            fprintf(stderr, "usage: lndb TARGET blink INTERVAL_MS TIMES (interval 1..60000, times 1..1000, total duration <=120000 ms)\n");
            rc = 2;
            goto operation_done;
        }
        int id_result = resolve_device_id(endpoint, config_path, &host, &port);
        if (id_result < 0) {
            fprintf(stderr, "lndb: unknown or invalid device ID '%s'; run 'lndb devices' to refresh IDs\n", endpoint);
            rc = 2;
        } else if (id_result == 0 && parse_endpoint(endpoint, cfg.port, &host, &port) < 0) {
            fprintf(stderr, "lndb: invalid endpoint: %s\n", endpoint); rc = 2;
        } else if (cfg.api_key_count == 0 &&
                   (!strcmp(op, "shell") || !strcmp(op, "push") || !strcmp(op, "pull") || !strcmp(op, "blink"))) {
            fprintf(stderr,
                    "lndb: no API keys configured in %s; register the daemon key with "
                    "'lndb add device %s <API_KEY>'\n",
                    config_path, endpoint);
            rc = 1;
        } else {
            cfg.port = port;
            int fd = connect_auth(host, &cfg, 0, &tcp_connected, &identified);
            if (fd < 0) {
                const char *reason = identified ? "API key rejected (unauthorized)" :
                    tcp_connected ? "TCP connected, but the peer did not send a valid LNDB1 greeting" :
                    "could not establish a TCP connection to the device";
                fprintf(stderr, "lndb: %s: %s\n", endpoint, reason);
                rc = 1;
            }
            else {
                if (!strcmp(op, "shell")) rc = shell_run(fd, argc - arg, argv + arg);
                else if (!strcmp(op, "blink")) {
                    if (pon_blink(fd, blink_interval, blink_times) < 0) { perror("lndb: PON LED blink"); rc = 1; }
                    else { printf("PON LED blink request queued (%u cycles, %u ms per phase)\n", blink_times, blink_interval); rc = 0; }
                }
                else if (!strcmp(op, "push") || !strcmp(op, "pull")) {
                    if (argc - arg != 2) { fprintf(stderr, "usage: lndb HOST[:PORT] %s SRC DST\n", op); rc = 2; }
                    else if (lndb_client_transfer(fd, !strcmp(op, "push"), argv[arg], argv[arg + 1]) < 0) {
                        perror("lndb: transfer"); rc = 1;
                    } else rc = 0;
                } else {
                    fprintf(stderr, "lndb: unknown operation: %s\n", op); rc = 2;
                }
                close(fd);
            }
        }
operation_done:
        free(host);
    } else { fprintf(stderr, "lndb: missing host and operation\n"); rc = 2; }
out:
    lndb_client_config_free(&cfg);
    free(auto_path);
    return rc;
}
