#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "config_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

char *lndb_client_default_config_path(void)
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
        if (mkdir_parents(path) < 0) {
            int saved = errno;
            free(path);
            errno = saved;
            return NULL;
        }
        *slash = '/';
    }
    return path;
}

int lndb_client_config_ensure(const char *path)
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

int lndb_client_config_add_key(const char *path,
                               const struct lndb_client_config *cfg,
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
        free(tmp);
        free(line);
        errno = saved;
    }
    return rc;
}
