#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "lndb_config.h"
#include "lndb_protocol.h"
#include "../client/transfer.h"
#include "../common/file_wire.h"
#include "../daemon/blink_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern int lndbd_transfer(int, const uint8_t *, uint32_t);

#define CHECK(x) do { if (!(x)) { perror(#x); return 1; } } while (0)

static int test_invalid_frames(void)
{
    int sv[2]; uint8_t type, *payload = NULL; uint32_t len;
    uint8_t header[LNDB_FRAME_HEADER_SIZE] = { 'L', 'N', 'D', 'B', LNDB_PROTOCOL_VERSION, LNDB_MSG_AUTH };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[0], header, 4) == 4);
    CHECK(shutdown(sv[0], SHUT_WR) == 0);
    CHECK(lndb_recv_frame(sv[1], &type, &payload, &len) < 0 && payload == NULL);
    close(sv[0]); close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    lndb_put_u32(header + 6, 3);
    CHECK(write(sv[0], header, sizeof(header)) == (ssize_t)sizeof(header));
    CHECK(write(sv[0], "x", 1) == 1);
    CHECK(shutdown(sv[0], SHUT_WR) == 0);
    CHECK(lndb_recv_frame(sv[1], &type, &payload, &len) < 0 && payload == NULL);
    close(sv[0]); close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    lndb_put_u32(header + 6, LNDB_FRAME_MAX_PAYLOAD + 1u);
    CHECK(write(sv[0], header, sizeof(header)) == (ssize_t)sizeof(header));
    CHECK(lndb_recv_frame(sv[1], &type, &payload, &len) < 0 && payload == NULL);
    close(sv[0]); close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    header[5] = 255; lndb_put_u32(header + 6, 0);
    CHECK(write(sv[0], header, sizeof(header)) == (ssize_t)sizeof(header));
    CHECK(lndb_recv_frame(sv[1], &type, &payload, &len) < 0 && payload == NULL);
    close(sv[0]); close(sv[1]);
    return 0;
}

static int test_blink_enqueue(void)
{
    int requests[2], slots[2]; uint8_t token = 1, record[8];
    struct lndbd_blink_queue queue = { 0, -1, -1, -1 };
    int flags;
    CHECK(pipe(requests) == 0 && pipe(slots) == 0);
    CHECK(write(slots[1], &token, 1) == 1);
    flags = fcntl(slots[0], F_GETFL, 0); CHECK(flags >= 0);
    CHECK(fcntl(slots[0], F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(requests[1], F_GETFL, 0); CHECK(flags >= 0);
    CHECK(fcntl(requests[1], F_SETFL, flags | O_NONBLOCK) == 0);
    queue.request_fd = requests[1]; queue.slots_fd = slots[0]; queue.slots_return_fd = slots[1];
    CHECK(lndbd_blink_queue_enqueue(&queue, 100, 3) == 0);
    CHECK(read(requests[0], record, sizeof(record)) == (ssize_t)sizeof(record));
    CHECK(lndb_get_u32(record) == 100 && lndb_get_u32(record + 4) == 3);
    errno = 0;
    CHECK(lndbd_blink_queue_enqueue(&queue, 100, 3) < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK));
    close(requests[0]); close(requests[1]); close(slots[0]); close(slots[1]);

    CHECK(pipe(requests) == 0 && pipe(slots) == 0);
    CHECK(write(slots[1], &token, 1) == 1);
    close(requests[0]);
    queue.request_fd = requests[1]; queue.slots_fd = slots[0]; queue.slots_return_fd = slots[1];
    void (*old_handler)(int) = signal(SIGPIPE, SIG_IGN);
    errno = 0;
    CHECK(lndbd_blink_queue_enqueue(&queue, 100, 3) < 0 && errno == EPIPE);
    CHECK(read(slots[0], &token, 1) == 1 && token == 1);
    (void)signal(SIGPIPE, old_handler);
    close(requests[1]); close(slots[0]); close(slots[1]);
    return 0;
}

static int server_transfer(int fd)
{
    uint8_t type, *payload = NULL;
    uint32_t len = 0;
    int rc = lndb_recv_frame(fd, &type, &payload, &len);
    if (rc == 0 && type == LNDB_MSG_FILE_REQUEST)
        rc = lndbd_transfer(fd, payload, len);
    else rc = -1;
    free(payload);
    close(fd);
    _exit(rc == 0 ? 0 : 1);
}

static int run_transfer(int push, const char *src, const char *dst)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { close(sv[0]); server_transfer(sv[1]); }
    close(sv[1]);
    int rc = lndb_client_transfer(sv[0], push, src, dst);
    close(sv[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static void server_reject_upload(int fd)
{
    uint8_t type, *p = NULL; uint32_t len = 0;
    if (lndb_recv_frame(fd, &type, &p, &len) < 0 || type != LNDB_MSG_FILE_REQUEST) _exit(2);
    free(p); p = NULL;
    for (;;) {
        if (lndb_recv_frame(fd, &type, &p, &len) < 0) _exit(3);
        if (type == LNDB_MSG_FILE_META && len >= 15 && p[0] == 0) {
            free(p); uint8_t status[4]; lndb_put_u32(status, 7);
            _exit(lndb_send_frame(fd, LNDB_MSG_FILE_DONE, status, sizeof(status)) == 0 ? 0 : 4);
        }
        free(p); p = NULL;
    }
}

static int run_remote_failure(const char *src, const char *dst)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { close(sv[0]); server_reject_upload(sv[1]); }
    close(sv[1]);
    int rc = lndb_client_transfer(sv[0], 1, src, dst);
    close(sv[0]); int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(rc < 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static void server_interrupt_download(int fd)
{
    uint8_t type, *p = NULL; uint32_t len = 0;
    if (lndb_recv_frame(fd, &type, &p, &len) < 0 || type != LNDB_MSG_FILE_REQUEST) _exit(2);
    free(p);
    uint8_t *meta = NULL; uint32_t ml = 0;
    if (lndb_file_meta_encode(1, 0600, 10, "source/file", &meta, &ml) < 0) _exit(3);
    if (lndb_send_frame(fd, LNDB_MSG_FILE_META, meta, ml) < 0) _exit(4);
    free(meta);
    if (lndb_send_frame(fd, LNDB_MSG_FILE_DATA, "abc", 3) < 0) _exit(5);
    close(fd); _exit(0);
}

static int run_interrupted_download(const char *dest, const char *expected)
{
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { close(sv[0]); server_interrupt_download(sv[1]); }
    close(sv[1]);
    int rc = lndb_client_transfer(sv[0], 0, "source/file", dest);
    close(sv[0]); int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    struct stat st;
    CHECK(rc < 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && lstat(expected, &st) < 0 && errno == ENOENT);
    return 0;
}

static void server_interrupt_upload(int fd)
{
    uint8_t type, *payload = NULL; uint32_t len = 0;
    if (lndb_recv_frame(fd, &type, &payload, &len) < 0 || type != LNDB_MSG_FILE_REQUEST) _exit(2);
    free(payload); payload = NULL;
    if (lndb_recv_frame(fd, &type, &payload, &len) < 0 || type != LNDB_MSG_FILE_META) _exit(3);
    free(payload);
    close(fd);
    _exit(0);
}

static int run_interrupted_upload(const char *src, const char *dest)
{
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { close(sv[0]); server_interrupt_upload(sv[1]); }
    close(sv[1]);
    int rc = lndb_client_transfer(sv[0], 1, src, dest);
    close(sv[0]); int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(rc < 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int run_symlink_reject(const char *linkpath, const char *dest)
{
    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { close(sv[0]); server_transfer(sv[1]); }
    close(sv[1]);
    int rc = lndb_client_transfer(sv[0], 0, linkpath, dest);
    close(sv[0]); int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(rc < 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        done += (size_t)n;
    }
    return close(fd);
}

static int same_file(const char *a, const char *b)
{
    int x = open(a, O_RDONLY), y = open(b, O_RDONLY);
    if (x < 0 || y < 0) { if (x >= 0) close(x); if (y >= 0) close(y); return 0; }
    unsigned char xb[1024], yb[1024]; int equal = 1;
    for (;;) {
        ssize_t nx = read(x, xb, sizeof(xb)), ny = read(y, yb, sizeof(yb));
        if (nx < 0 || ny < 0 || nx != ny || (nx > 0 && memcmp(xb, yb, (size_t)nx))) { equal = 0; break; }
        if (nx == 0) break;
    }
    close(x); close(y); return equal;
}

static int append_path(char *out, size_t cap, const char *base, const char *suffix)
{
    size_t a = strlen(base), b = strlen(suffix);
    if (a + b + 1 > cap) return -1;
    memcpy(out, base, a);
    memcpy(out + a, suffix, b + 1);
    return 0;
}

int main(void)
{
    char tmp[] = "/tmp/lndb-local-test-XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    CHECK(test_invalid_frames() == 0);
    CHECK(test_blink_enqueue() == 0);
    char conf[512], badconf[512], clientconf[512], srcdir[512], subdir[512], remote[512], remote2[512], remote3[512];
    char local[512], local2[512], local3[512], local_existing[512], interrupted[512], interrupted_file[512], symlink_path[512], p[512], q[512], remote_src[512];
    CHECK(append_path(conf, sizeof(conf), tmp, "/nested/lndbd.conf") == 0);
    struct lndb_server_config sc; char err[128];
    uint8_t *wire = NULL, wire_kind;
    uint32_t wire_len = 0, wire_mode;
    uint64_t wire_size;
    const uint8_t *wire_path;
    size_t wire_path_len;
    CHECK(lndb_file_meta_encode(1, 0644, 3, "file", &wire, &wire_len) == 0);
    CHECK(lndb_file_meta_decode(wire, wire_len, &wire_kind, &wire_mode,
                                &wire_size, &wire_path, &wire_path_len) == 0);
    CHECK(wire_kind == 1 && wire_mode == 0644 && wire_size == 3 &&
          wire_path_len == 4 && memcmp(wire_path, "file", 4) == 0);
    wire[0] = 3;
    CHECK(lndb_file_meta_decode(wire, wire_len, &wire_kind, &wire_mode,
                                &wire_size, &wire_path, &wire_path_len) < 0);
    free(wire); wire = NULL;
    CHECK(lndb_file_meta_encode(3, 0644, 3, "file", &wire, &wire_len) < 0);
    CHECK(lndb_file_meta_encode(0, 0, 0, "not-empty", &wire, &wire_len) < 0);
    CHECK(lndb_server_config_load(conf, &sc, err, sizeof(err)) == 0);
    struct stat st; CHECK(stat(conf, &st) == 0 && (st.st_mode & 0777) == 0600);
    char first[65]; memcpy(first, sc.api_key, sizeof(first));
    CHECK(lndb_server_config_load(conf, &sc, err, sizeof(err)) == 0);
    CHECK(strcmp(first, sc.api_key) == 0 && strlen(sc.api_key) == 64);
    CHECK(append_path(badconf, sizeof(badconf), tmp, "/invalid.conf") == 0);
    static const unsigned char invalid_config[] = "[auth]\napi_key = invalid\n";
    CHECK(write_file(badconf, invalid_config, sizeof(invalid_config) - 1, 0600) == 0);
    CHECK(lndb_server_config_load(badconf, &sc, err, sizeof(err)) < 0);
    CHECK(append_path(clientconf, sizeof(clientconf), tmp, "/lndb.conf") == 0);
    char client_text[256];
    int client_len = snprintf(client_text, sizeof(client_text),
        "[client]\nport = 5555\nconnect_timeout_ms = 500\n\n"
        "[discovery]\nnetwork = 192.168.10.*\nconcurrency = 8\n\n"
        "[auth]\napi_key_list = %s,%s\n", first, first);
    CHECK(client_len > 0 && (size_t)client_len < sizeof(client_text));
    CHECK(write_file(clientconf, (const unsigned char *)client_text, (size_t)client_len, 0600) == 0);
    struct lndb_client_config cc;
    CHECK(lndb_client_config_load(clientconf, &cc, err, sizeof(err)) == 0);
    CHECK(cc.api_key_count == 2 && cc.port == 5555 && cc.concurrency == 8 &&
          strcmp(cc.network, "192.168.10.*") == 0);
    lndb_client_config_free(&cc);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        if (errno == EPERM || errno == EACCES) {
            puts("config checks passed; stream tests skipped by sandbox policy");
            return 0;
        }
        perror("socketpair"); return 1;
    }
    uint8_t type, *payload; uint32_t len;
    int frame_rc = lndb_send_frame(sv[0], LNDB_MSG_AUTH, "key", 3);
    if (frame_rc < 0 && (errno == EPERM || errno == EACCES)) {
        close(sv[0]); close(sv[1]);
        puts("config checks passed; stream tests skipped by sandbox policy");
        return 0;
    }
    CHECK(frame_rc == 0);
    CHECK(lndb_recv_frame(sv[1], &type, &payload, &len) == 0);
    CHECK(type == LNDB_MSG_AUTH && len == 3 && memcmp(payload, "key", 3) == 0);
    free(payload); close(sv[0]); close(sv[1]);

    CHECK(append_path(srcdir, sizeof(srcdir), tmp, "/source tree") == 0);
    CHECK(append_path(subdir, sizeof(subdir), srcdir, "/sub dir") == 0);
    CHECK(append_path(remote, sizeof(remote), tmp, "/remote") == 0);
    CHECK(append_path(remote2, sizeof(remote2), tmp, "/remote two") == 0);
    CHECK(append_path(remote3, sizeof(remote3), tmp, "/remote three") == 0);
    CHECK(append_path(local, sizeof(local), tmp, "/downloaded tree") == 0);
    CHECK(append_path(local2, sizeof(local2), tmp, "/downloaded tree two") == 0);
    CHECK(append_path(local3, sizeof(local3), tmp, "/downloaded tree three") == 0);
    CHECK(append_path(local_existing, sizeof(local_existing), tmp, "/existing destination") == 0);
    CHECK(append_path(interrupted, sizeof(interrupted), tmp, "/interrupted destination") == 0);
    CHECK(append_path(symlink_path, sizeof(symlink_path), tmp, "/remote-link") == 0);
    CHECK(mkdir(srcdir, 0755) == 0 && mkdir(subdir, 0750) == 0 && mkdir(remote, 0755) == 0);
    CHECK(symlink(remote, symlink_path) == 0);
    CHECK(run_symlink_reject(symlink_path, local) == 0);
    CHECK(run_symlink_reject("../remote", local) == 0);
    CHECK(append_path(p, sizeof(p), subdir, "/binary file.bin") == 0);
    CHECK(append_path(q, sizeof(q), srcdir, "/empty") == 0);
    static const unsigned char bytes[] = { 0, 1, 2, 0xff, 'L', 'N', 'D', 'B', '\n' };
    CHECK(write_file(p, bytes, sizeof(bytes), 0640) == 0);
    CHECK(write_file(q, bytes, 0, 0600) == 0);
    CHECK(run_remote_failure(q, remote) == 0); /* positive FILE_DONE is failure */
    CHECK(run_interrupted_upload(p, remote) == 0);
    CHECK(run_transfer(1, srcdir, remote) == 0);
    CHECK(append_path(p, sizeof(p), remote, "/source tree/sub dir/binary file.bin") == 0);
    CHECK(append_path(q, sizeof(q), remote, "/source tree/empty") == 0);
    CHECK(same_file(p, q) == 0); /* distinct payloads and sizes */
    CHECK(append_path(q, sizeof(q), srcdir, "/sub dir/binary file.bin") == 0);
    CHECK(same_file(p, q));
    CHECK(append_path(p, sizeof(p), remote, "/source tree") == 0);
    CHECK(run_transfer(0, p, local) == 0);
    CHECK(append_path(p, sizeof(p), local, "/sub dir/binary file.bin") == 0);
    CHECK(append_path(q, sizeof(q), srcdir, "/sub dir/binary file.bin") == 0);
    CHECK(same_file(p, q));
    CHECK(append_path(q, sizeof(q), local, "/empty") == 0);
    CHECK(stat(q, &st) == 0 && st.st_size == 0);
    CHECK((st.st_mode & 0777) == 0600);

    /* Each temp remote root stands in for the copy destination (such as /).
       All source spellings must create root/source tree and its full subtree. */
    CHECK(mkdir(remote2, 0755) == 0 && mkdir(remote3, 0755) == 0);
    CHECK(chdir(tmp) == 0);
    const char *source_forms[] = { "source tree/", "./source tree", "./source tree/" };
    const char *remote_roots[] = { remote, remote2, remote3 };
    const char *pull_suffixes[] = { "/source tree/", "/source tree", "/source tree///" };
    const char *local_dests[] = { local, local2, local3 };
    for (size_t i = 0; i < 3; ++i) {
        CHECK(run_transfer(1, source_forms[i], remote_roots[i]) == 0);
        CHECK(append_path(p, sizeof(p), remote_roots[i], "/source tree/sub dir/binary file.bin") == 0);
        CHECK(append_path(q, sizeof(q), srcdir, "/sub dir/binary file.bin") == 0);
        CHECK(same_file(p, q));
        CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0640);
        CHECK(append_path(p, sizeof(p), remote_roots[i], "/source tree/empty") == 0);
        CHECK(stat(p, &st) == 0 && st.st_size == 0 && (st.st_mode & 0777) == 0600);

        CHECK(append_path(remote_src, sizeof(remote_src), remote_roots[i], pull_suffixes[i]) == 0);
        CHECK(run_transfer(0, remote_src, local_dests[i]) == 0);
        CHECK(append_path(p, sizeof(p), local_dests[i], "/sub dir/binary file.bin") == 0);
        CHECK(append_path(q, sizeof(q), srcdir, "/sub dir/binary file.bin") == 0);
        CHECK(same_file(p, q));
        CHECK(append_path(p, sizeof(p), local_dests[i], "/empty") == 0);
        CHECK(stat(p, &st) == 0 && st.st_size == 0 && (st.st_mode & 0777) == 0600);
    }

    /* Pull into an existing destination directory keeps the source basename. */
    CHECK(mkdir(local_existing, 0755) == 0);
    CHECK(mkdir(interrupted, 0755) == 0);
    CHECK(append_path(interrupted_file, sizeof(interrupted_file), interrupted, "/source/file") == 0);
    CHECK(run_interrupted_download(interrupted, interrupted_file) == 0);
    CHECK(append_path(remote_src, sizeof(remote_src), remote, "/source tree/") == 0);
    CHECK(run_transfer(0, remote_src, local_existing) == 0);
    CHECK(append_path(p, sizeof(p), local_existing, "/source tree/sub dir/binary file.bin") == 0);
    CHECK(append_path(q, sizeof(q), srcdir, "/sub dir/binary file.bin") == 0);
    CHECK(same_file(p, q));
    puts("local LNDB config, framing, recursive directory push/pull checks passed");
    return 0;
}
