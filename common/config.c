#define _POSIX_C_SOURCE 200809L
#include "lndb_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void seterr(char *err, size_t n, const char *s)
{
    if (err && n) snprintf(err, n, "%s", s);
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) ++s;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = '\0';
    return s;
}

static int parse_uint(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long n;
    if (!*s || *s == '-' || *s == '+') return -1;
    errno = 0;
    n = strtoul(s, &end, 10);
    if (errno || *end || n == 0 || n > max) return -1;
    *out = n;
    return 0;
}

static int valid_key(const char *s)
{
    size_t i;
    if (strlen(s) != 64) return 0;
    for (i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int random_key(char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char b[32];
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (got < sizeof(b)) {
        ssize_t n = read(fd, b + got, sizeof(b) - got);
        if (n > 0) got += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { int saved = errno ? errno : EIO; close(fd); errno = saved; return -1; }
    }
    close(fd);
    for (got = 0; got < sizeof(b); ++got) {
        out[got * 2] = hex[b[got] >> 4];
        out[got * 2 + 1] = hex[b[got] & 15];
    }
    out[64] = '\0';
    return 0;
}

static int mkdir_parents(const char *path)
{
    char *copy = strdup(path), *p;
    if (!copy) return -1;
    for (p = copy + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) < 0 && errno != EEXIST) { free(copy); return -1; }
            *p = '/';
        }
    }
    free(copy);
    return 0;
}

static int create_config(const char *path, struct lndb_server_config *cfg)
{
    char text[256];
    int fd, n, saved;
    size_t done = 0;
    if (random_key(cfg->api_key) < 0 || mkdir_parents(path) < 0) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    n = snprintf(text, sizeof(text), "[server]\nbind = 0.0.0.0\nport = 5555\n\n[auth]\napi_key = %s\n", cfg->api_key);
    if (fchmod(fd, 0600) < 0) {
        saved = errno; close(fd); unlink(path); errno = saved; return -1;
    }
    while (done < (size_t)n) {
        ssize_t written = write(fd, text + done, (size_t)n - done);
        if (written > 0) done += (size_t)written;
        else if (written < 0 && errno == EINTR) continue;
        else {
            saved = errno ? errno : EIO;
            close(fd); unlink(path); errno = saved; return -1;
        }
    }
    if (fsync(fd) < 0) {
        saved = errno ? errno : EIO; close(fd); unlink(path); errno = saved; return -1;
    }
    if (close(fd) < 0) return -1;
    strcpy(cfg->bind, "0.0.0.0");
    cfg->port = 5555;
    return 0;
}

static int parse_file(const char *path, struct lndb_server_config *srv,
                      struct lndb_client_config *cli, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    char line[1024], section[32] = "";
    unsigned seen = 0;
    unsigned long val;
    unsigned lineno = 0;
    if (!f) { seterr(err, errlen, "cannot open config file"); return -1; }
    while (fgets(line, sizeof(line), f)) {
        char *s, *eq, *key, *value;
        ++lineno;
        if (!strchr(line, '\n') && !feof(f)) goto invalid;
        s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            size_t l = strlen(s);
            if (l < 3 || s[l - 1] != ']' || l - 2 >= sizeof(section)) goto invalid;
            s[l - 1] = '\0';
            strcpy(section, s + 1);
            if (strcmp(section, "server") && strcmp(section, "auth") &&
                strcmp(section, "client") && strcmp(section, "discovery")) goto invalid;
            continue;
        }
        eq = strchr(s, '=');
        if (!eq || !*section) goto invalid;
        *eq = '\0'; key = trim(s); value = trim(eq + 1);
#define ONCE(bit) do { if (seen & (bit)) goto invalid; seen |= (bit); } while (0)
        if (!strcmp(section, "server") && !strcmp(key, "bind") && srv) {
            ONCE(1u << 0);
            if (!*value || strlen(value) >= sizeof(srv->bind)) goto invalid;
            strcpy(srv->bind, value);
        } else if (!strcmp(section, "server") && !strcmp(key, "port") && srv) {
            ONCE(1u << 1); if (parse_uint(value, 65535, &val) < 0) goto invalid; srv->port = (uint16_t)val;
        } else if (!strcmp(section, "auth") && !strcmp(key, "api_key") && srv) {
            ONCE(1u << 2); if (!valid_key(value)) goto invalid; strcpy(srv->api_key, value);
        } else if (!strcmp(section, "client") && !strcmp(key, "port") && cli) {
            ONCE(1u << 3); if (parse_uint(value, 65535, &val) < 0) goto invalid; cli->port = (uint16_t)val;
        } else if (!strcmp(section, "client") && !strcmp(key, "connect_timeout_ms") && cli) {
            ONCE(1u << 4); if (parse_uint(value, 600000, &val) < 0) goto invalid; cli->connect_timeout_ms = (unsigned)val;
        } else if (!strcmp(section, "discovery") && !strcmp(key, "network") && cli) {
            ONCE(1u << 5); if (!*value || strlen(value) >= sizeof(cli->network)) goto invalid; strcpy(cli->network, value);
        } else if (!strcmp(section, "discovery") && !strcmp(key, "concurrency") && cli) {
            ONCE(1u << 6); if (parse_uint(value, 4096, &val) < 0) goto invalid; cli->concurrency = (unsigned)val;
        } else if (!strcmp(section, "auth") && !strcmp(key, "api_key_list") && cli) {
            char *tok, *save = NULL;
            ONCE(1u << 7);
            for (tok = strtok_r(value, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                char *item = trim(tok), **items;
                if (!valid_key(item)) goto invalid;
                items = realloc(cli->api_keys, (cli->api_key_count + 1) * sizeof(*items));
                if (!items) goto failed;
                cli->api_keys = items; cli->api_keys[cli->api_key_count] = strdup(item);
                if (!cli->api_keys[cli->api_key_count]) goto failed;
                ++cli->api_key_count;
            }
        } else goto invalid;
#undef ONCE
    }
    if (ferror(f)) { fclose(f); seterr(err, errlen, "error reading config file"); return -1; }
    fclose(f);
    if (srv && !(seen & (1u << 2))) { seterr(err, errlen, "missing [auth] api_key"); return -1; }
    if (cli && !(seen & (1u << 7))) { seterr(err, errlen, "missing [auth] api_key_list"); return -1; }
    return 0;
invalid:
    fclose(f);
    if (err && errlen) snprintf(err, errlen, "invalid config at line %u", lineno);
    return -1;
failed:
    fclose(f); seterr(err, errlen, "out of memory"); return -1;
}

int lndb_server_config_load(const char *path, struct lndb_server_config *out,
                            char *err, size_t errlen)
{
    struct stat st;
    if (!path || !out) { seterr(err, errlen, "invalid argument"); return -1; }
    memset(out, 0, sizeof(*out));
    strcpy(out->bind, "0.0.0.0"); out->port = 5555;
    if (stat(path, &st) < 0) {
        if (errno != ENOENT) { seterr(err, errlen, "cannot stat config file"); return -1; }
        if (create_config(path, out) == 0) return 0;
        if (errno != EEXIST) { seterr(err, errlen, "cannot create config file"); return -1; }
    }
    if (parse_file(path, out, NULL, err, errlen) < 0) return -1;
    return 0;
}

int lndb_client_config_load(const char *path, struct lndb_client_config *out,
                            char *err, size_t errlen)
{
    if (!path || !out) { seterr(err, errlen, "invalid argument"); return -1; }
    memset(out, 0, sizeof(*out));
    out->port = 5555; out->connect_timeout_ms = 500; out->concurrency = 32;
    strcpy(out->network, "192.168.10.*");
    if (parse_file(path, NULL, out, err, errlen) < 0) { lndb_client_config_free(out); return -1; }
    return 0;
}

void lndb_client_config_free(struct lndb_client_config *cfg)
{
    size_t i;
    if (!cfg) return;
    for (i = 0; i < cfg->api_key_count; ++i) free(cfg->api_keys[i]);
    free(cfg->api_keys);
    memset(cfg, 0, sizeof(*cfg));
}
