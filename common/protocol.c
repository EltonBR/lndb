#include "lndb_protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int lndb_read_full(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n > 0) { done += (size_t)n; continue; }
        if (n == 0) return done == 0 ? 1 : -1;
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

int lndb_write_full(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n;
#ifdef MSG_NOSIGNAL
        n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK) n = write(fd, (const char *)buf + done, len - done);
#else
        n = write(fd, (const char *)buf + done, len - done);
#endif
        if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

void lndb_put_u32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

uint32_t lndb_get_u32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

int lndb_send_frame(int fd, uint8_t type, const void *payload, uint32_t len)
{
    uint8_t header[LNDB_FRAME_HEADER_SIZE];
    if (len > LNDB_FRAME_MAX_PAYLOAD || (len && !payload)) { errno = EINVAL; return -1; }
    memcpy(header, "LNDB", 4);
    header[4] = LNDB_PROTOCOL_VERSION;
    header[5] = type;
    lndb_put_u32(header + 6, len);
    if (lndb_write_full(fd, header, sizeof(header)) < 0) return -1;
    return len ? lndb_write_full(fd, payload, len) : 0;
}

int lndb_recv_frame(int fd, uint8_t *type, uint8_t **payload, uint32_t *len)
{
    uint8_t header[LNDB_FRAME_HEADER_SIZE];
    uint8_t *data = NULL;
    uint32_t size;
    int r;
    if (!type || !payload || !len) { errno = EINVAL; return -1; }
    *payload = NULL;
    *len = 0;
    r = lndb_read_full(fd, header, sizeof(header));
    if (r != 0) {
        if (r > 0) errno = ECONNRESET;
        return -1;
    }
    if (memcmp(header, "LNDB", 4) != 0 || header[4] != LNDB_PROTOCOL_VERSION) {
        errno = EPROTO; return -1;
    }
    size = lndb_get_u32(header + 6);
    if (size > LNDB_FRAME_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    switch (header[5]) {
    case LNDB_MSG_HELLO: case LNDB_MSG_AUTH: case LNDB_MSG_AUTH_OK:
    case LNDB_MSG_ERROR: case LNDB_MSG_SHELL_REQUEST:
    case LNDB_MSG_SHELL_STDIN: case LNDB_MSG_SHELL_STDOUT:
    case LNDB_MSG_SHELL_STDERR: case LNDB_MSG_SHELL_RESIZE:
    case LNDB_MSG_SHELL_EXIT: case LNDB_MSG_FILE_REQUEST:
    case LNDB_MSG_FILE_META: case LNDB_MSG_FILE_DATA:
    case LNDB_MSG_FILE_END: case LNDB_MSG_CLOSE: case LNDB_MSG_FILE_DONE:
    case LNDB_MSG_DISCOVER: case LNDB_MSG_DISCOVER_REPLY:
    case LNDB_MSG_PON_BLINK: case LNDB_MSG_PON_BLINK_DONE:
    case LNDB_MSG_PON_BLINK_QUEUED:
        break;
    default:
        errno = EPROTO;
        return -1;
    }
    if (size) {
        data = malloc(size);
        if (!data) return -1;
        if (lndb_read_full(fd, data, size) != 0) { free(data); errno = EPROTO; return -1; }
    }
    *type = header[5];
    *payload = data;
    *len = size;
    return 0;
}
