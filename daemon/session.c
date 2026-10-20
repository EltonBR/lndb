#define _POSIX_C_SOURCE 200809L
#include "session.h"

#include "lndb_protocol.h"
#include "shell.h"
#include "transfer.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define AUTH_TIMEOUT_MS 4000

static int send_error(int fd, const char *message)
{
    return lndb_send_frame(fd, LNDB_MSG_ERROR, message, (uint32_t)strlen(message));
}

/* Read only the initial AUTH frame against one absolute deadline. Once the
 * key is accepted, normal blocking protocol I/O is used so large transfers
 * and interactive sessions are not constrained by this timeout. */
static int auth_read_full(int fd, void *buffer, size_t length,
                          const struct timespec *deadline)
{
    size_t done = 0;
    while (done < length) {
        struct timespec now;
        struct pollfd pfd;
        int timeout, ready;
        ssize_t n;

        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        int64_t remaining = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
                            (deadline->tv_nsec - now.tv_nsec) / 1000000;
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) { errno = ETIMEDOUT; return -1; }
        if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
        n = recv(fd, (char *)buffer + done, length - done, MSG_DONTWAIT);
        if (n > 0) { done += (size_t)n; continue; }
        if (n == 0) { errno = ECONNRESET; return -1; }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -1;
    }
    return 0;
}

static int authenticate(int fd, const char *key)
{
    uint8_t header[LNDB_FRAME_HEADER_SIZE], payload[65];
    uint32_t len;
    size_t key_len = strlen(key);
    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return -1;
    deadline.tv_sec += AUTH_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (AUTH_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    if (lndb_send_frame(fd, LNDB_MSG_HELLO, NULL, 0) < 0 ||
        auth_read_full(fd, header, sizeof(header), &deadline) < 0)
        return -1;

    len = lndb_get_u32(header + 6);
    if (memcmp(header, "LNDB", 4) != 0 ||
        header[4] != LNDB_PROTOCOL_VERSION || header[5] != LNDB_MSG_AUTH ||
        len != key_len || len > sizeof(payload)) {
        send_error(fd, "unauthorized");
        return -1;
    }
    if (len && auth_read_full(fd, payload, len, &deadline) < 0) return -1;
    int ok = len == key_len && memcmp(payload, key, len) == 0;
    if (!ok) {
        send_error(fd, "unauthorized");
        return -1;
    }
    return lndb_send_frame(fd, LNDB_MSG_AUTH_OK, NULL, 0);
}

void lndbd_session_serve(int fd, const char *api_key,
                         const struct lndbd_blink_queue *blink_queue)
{
    uint8_t type, *payload = NULL;
    uint32_t len;

    if (authenticate(fd, api_key) < 0) return;
    if (lndb_recv_frame(fd, &type, &payload, &len) < 0) return;
    if (type == LNDB_MSG_CLOSE) {
        free(payload);
        return;
    }
    if (type == LNDB_MSG_SHELL_REQUEST) {
        lndbd_shell(fd, payload, len);
    } else if (type == LNDB_MSG_FILE_REQUEST) {
        lndbd_transfer(fd, payload, len);
    } else if (type == LNDB_MSG_PON_BLINK) {
        if (len != 8) {
            send_error(fd, "invalid PON LED blink request");
        } else {
            uint32_t interval_ms = lndb_get_u32(payload);
            uint32_t times = lndb_get_u32(payload + 4);
            if (interval_ms < LNDB_PON_BLINK_MIN_INTERVAL_MS ||
                interval_ms > LNDB_PON_BLINK_MAX_INTERVAL_MS ||
                times < LNDB_PON_BLINK_MIN_TIMES || times > LNDB_PON_BLINK_MAX_TIMES ||
                (uint64_t)2 * interval_ms * times > LNDB_PON_BLINK_MAX_DURATION_MS) {
                send_error(fd, "invalid PON LED blink parameters");
            } else if (lndbd_blink_queue_enqueue(blink_queue, interval_ms, times) < 0) {
                send_error(fd, "PON LED blink queue is full");
            } else {
                lndb_send_frame(fd, LNDB_MSG_PON_BLINK_QUEUED, NULL, 0);
            }
        }
    } else {
        send_error(fd, "unsupported operation");
    }
    free(payload);
}
