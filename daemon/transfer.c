#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "transfer.h"
#include "lndb_protocol.h"
#include "../common/file_wire.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad_path(const char *p)
{
    if (!p || !*p || *p == '/') return 1;
    const char *s = p;
    while (*s) {
        const char *e = strchr(s, '/'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 1;
        s = e ? e + 1 : s + n;
    }
    return 0;
}

/* Reject a symlink in every existing component. Missing suffixes are allowed
   for destinations that will be created. This is defense in depth; the path
   is not a chroot and concurrent rename attacks remain possible. */
static int has_symlink_component(const char *path)
{
    if (!path || !*path) return 1;
    char *copy = strdup(path);
    if (!copy) return 1;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    for (char *s = copy + (*copy == '/'); ; ) {
        char *slash = strchr(s, '/');
        if (slash) *slash = '\0';
        if (*copy) {
            struct stat st;
            if (lstat(copy, &st) == 0) {
                if (S_ISLNK(st.st_mode)) { free(copy); return 1; }
            } else if (errno != ENOENT) { free(copy); return 1; }
        }
        if (!slash) break;
        *slash = '/';
        s = slash + 1;
    }
    free(copy);
    return 0;
}

static int has_parent_component(const char *path)
{
    const char *s = path;
    while (*s) {
        while (*s == '/') s++;
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 2 && s[0] == '.' && s[1] == '.') return 1;
        if (!e) break;
        s = e + 1;
    }
    return 0;
}

static int mkdir_parents(const char *path)
{
    char *p = strdup(path); if (!p) return -1;
    for (char *s = p + (*p == '/'); *s; s++) if (*s == '/') {
        *s = 0; if (*p && mkdir(p, 0755) < 0 && errno != EEXIST) { free(p); return -1; } *s = '/';
    }
    free(p); return 0;
}

static int send_meta(int fd, uint8_t kind, uint32_t mode, uint64_t size, const char *path)
{
    uint8_t *p = NULL; uint32_t n = 0;
    if (lndb_file_meta_encode(kind, mode, size, path, &p, &n) < 0) return -1;
    int r = lndb_send_frame(fd, LNDB_MSG_FILE_META, p, n);
    free(p);
    return r;
}

static int send_tree(int fd, const char *path, const char *rel)
{
    struct stat st;
    if (has_symlink_component(path) || lstat(path, &st) < 0 || S_ISLNK(st.st_mode)) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (send_meta(fd, 2, st.st_mode & 0777, 0, rel) < 0) return -1;
        int dfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        DIR *d = dfd >= 0 ? fdopendir(dfd) : NULL;
        if (!d) { if (dfd >= 0) close(dfd); return -1; }
        struct dirent *e; int rc = 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *child = NULL, *childrel = NULL;
            if (asprintf(&child, "%s/%s", path, e->d_name) < 0 ||
                asprintf(&childrel, "%s%s%s", rel, *rel ? "/" : "", e->d_name) < 0) {
                free(child); free(childrel); rc = -1; break;
            }
            rc = send_tree(fd, child, childrel); free(child); free(childrel); if (rc < 0) break;
        }
        closedir(d); return rc;
    }
    if (!S_ISREG(st.st_mode) || send_meta(fd, 1, st.st_mode & 0777, (uint64_t)st.st_size, rel) < 0) return -1;
    int f = open(path, O_RDONLY | O_NOFOLLOW); if (f < 0) return -1;
    uint8_t b[32768]; ssize_t n; int rc = 0;
    while ((n = read(f, b, sizeof(b))) > 0) if (lndb_send_frame(fd, LNDB_MSG_FILE_DATA, b, (uint32_t)n) < 0) { rc = -1; break; }
    if (n < 0) rc = -1;
    close(f);
    if (!rc) rc = lndb_send_frame(fd, LNDB_MSG_FILE_END, NULL, 0);
    return rc;
}

static int receive_tree(int fd, const char *dest)
{
    uint8_t type, *p = NULL; uint32_t n; int rc = 0;
    struct stat ds; int dest_is_dir = stat(dest, &ds) == 0 && S_ISDIR(ds.st_mode);
    char *root = NULL; int root_kind = 0;
    for (;;) {
        if (lndb_recv_frame(fd, &type, &p, &n) < 0) { rc = -1; break; }
        if (type != LNDB_MSG_FILE_META) { free(p); rc = -1; break; }
        uint8_t kind; uint32_t mode; uint64_t size; const uint8_t *wire_path; size_t pn;
        if (lndb_file_meta_decode(p, n, &kind, &mode, &size, &wire_path, &pn) < 0) { free(p); rc = -1; break; }
        char *rel = malloc(pn + 1); if (!rel) { free(p); rc = -1; break; }
        memcpy(rel, wire_path, pn); rel[pn] = 0; free(p); p = NULL;
        if (kind == 0) { free(rel); break; }
        if (bad_path(rel) || size > (uint64_t)INT64_MAX) { free(rel); rc = -1; break; }
        if (!root) {
            const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
            if (kind == 2 && dest_is_dir) { if (asprintf(&root, "%s/%s", dest, base) < 0) root = NULL; }
            else if (dest_is_dir) { if (asprintf(&root, "%s/%s", dest, base) < 0) root = NULL; }
            else root = strdup(dest);
            root_kind = kind;
            if (!root) { free(rel); rc = -1; break; }
            if (has_symlink_component(root)) { free(rel); rc = -1; break; }
            if (root_kind == 2 && mkdir_parents(root) < 0) { free(rel); rc = -1; break; }
        }
        const char *tail = rel;
        if (root_kind == 2) { const char *slash = strchr(rel, '/'); tail = slash ? slash + 1 : NULL; }
        else tail = NULL;
        char *target = NULL;
        if (!tail || !*tail) target = strdup(root);
        else if (asprintf(&target, "%s/%s", root, tail) < 0) target = NULL;
        if (!target) { free(rel); rc = -1; break; }
        if (has_symlink_component(target)) { free(target); free(rel); rc = -1; break; }
        if (kind == 2) {
            if (mkdir_parents(target) < 0 || (mkdir(target, mode & 0777) < 0 && errno != EEXIST)) rc = -1;
            else if (chmod(target, mode & 0777) < 0) rc = -1;
        } else if (kind == 1) {
            if (mkdir_parents(target) < 0) rc = -1;
            char *tmp = NULL; int out = -1;
            if (!rc && asprintf(&tmp, "%s.lndb-tmp-%ld", target, (long)getpid()) >= 0)
                out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode & 0777);
            if (out < 0) rc = -1;
            uint64_t got = 0;
            while (!rc) {
                if (lndb_recv_frame(fd, &type, &p, &n) < 0) { rc = -1; break; }
                if (type == LNDB_MSG_FILE_END) { free(p); p = NULL; break; }
                if (type != LNDB_MSG_FILE_DATA || got + n > size || lndb_write_full(out, p, n) < 0) rc = -1;
                else got += n;
                free(p); p = NULL;
            }
            if (out >= 0) {
                if (!rc && fchmod(out, mode & 0777) < 0) rc = -1;
                if (close(out) < 0) rc = -1;
                if (!rc && got == size) { if (rename(tmp, target) < 0) rc = -1; } else rc = -1;
            }
            if (tmp) { if (rc) unlink(tmp); free(tmp); }
        } else rc = -1;
        free(target); free(rel);
        if (rc) break;
    }
    free(root);
    uint8_t status[4]; lndb_put_u32(status, rc ? 1 : 0);
    lndb_send_frame(fd, LNDB_MSG_FILE_DONE, status, sizeof(status));
    return rc;
}

int lndbd_transfer(int fd, const uint8_t *req, uint32_t len)
{
    if (len < 2 || !req[1] || memchr(req + 1, 0, len - 1))
        return lndb_send_frame(fd, LNDB_MSG_ERROR, "bad file request", 16);
    char *path = malloc(len); if (!path) return -1;
    memcpy(path, req + 1, len - 1); path[len - 1] = 0;
    int rc;
    if (has_parent_component(path)) rc = lndb_send_frame(fd, LNDB_MSG_ERROR, "parent path component rejected", (uint32_t)(sizeof("parent path component rejected") - 1));
    else if (has_symlink_component(path)) rc = lndb_send_frame(fd, LNDB_MSG_ERROR, "symlink path rejected", (uint32_t)(sizeof("symlink path rejected") - 1));
    else if (req[0] == 1) rc = receive_tree(fd, path);
    else if (req[0] == 2) {
        size_t path_len = strlen(path);
        while (path_len > 1 && path[path_len - 1] == '/') path[--path_len] = '\0';
        char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        rc = (*base && strcmp(base, ".") && strcmp(base, "..")) ? send_tree(fd, path, base) : -1;
        if (!rc) rc = send_meta(fd, 0, 0, 0, "");
        uint8_t status[4]; lndb_put_u32(status, rc ? 1 : 0);
        if (lndb_send_frame(fd, LNDB_MSG_FILE_DONE, status, sizeof(status)) < 0) rc = -1;
    } else rc = -1;
    free(path); return rc;
}
