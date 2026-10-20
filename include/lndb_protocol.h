#ifndef LNDB_PROTOCOL_H
#define LNDB_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LNDB_PROTOCOL_VERSION 1u
#define LNDB_FRAME_HEADER_SIZE 10u
#define LNDB_FRAME_MAX_PAYLOAD (1024u * 1024u)
#define LNDB_PON_BLINK_MIN_INTERVAL_MS 1u
#define LNDB_PON_BLINK_MAX_INTERVAL_MS 60000u
#define LNDB_PON_BLINK_MIN_TIMES 1u
#define LNDB_PON_BLINK_MAX_TIMES 1000u
#define LNDB_PON_BLINK_MAX_DURATION_MS 120000u

enum lndb_message_type {
    LNDB_MSG_HELLO = 1,
    LNDB_MSG_AUTH = 2,
    LNDB_MSG_AUTH_OK = 3,
    LNDB_MSG_ERROR = 4,
    LNDB_MSG_SHELL_REQUEST = 10,
    LNDB_MSG_SHELL_STDIN = 11,
    LNDB_MSG_SHELL_STDOUT = 12,
    LNDB_MSG_SHELL_STDERR = 13,
    LNDB_MSG_SHELL_RESIZE = 14,
    LNDB_MSG_SHELL_EXIT = 15,
    LNDB_MSG_FILE_REQUEST = 20,
    LNDB_MSG_FILE_META = 21,
    LNDB_MSG_FILE_DATA = 22,
    LNDB_MSG_FILE_END = 23,
    LNDB_MSG_CLOSE = 24,
    LNDB_MSG_FILE_DONE = 25,
    LNDB_MSG_DISCOVER = 30,
    LNDB_MSG_DISCOVER_REPLY = 31,
    LNDB_MSG_PON_BLINK = 40,
    LNDB_MSG_PON_BLINK_DONE = 41,
    LNDB_MSG_PON_BLINK_QUEUED = 42
};

/* Returns 0 on success, -1 on error. read_full returns 1 on clean EOF before
 * any byte, and -1 on a truncated stream or other error. */
int lndb_read_full(int fd, void *buf, size_t len);
int lndb_write_full(int fd, const void *buf, size_t len);
int lndb_send_frame(int fd, uint8_t type, const void *payload, uint32_t len);
/* Payload is allocated with malloc (or NULL for length zero); caller frees it. */
int lndb_recv_frame(int fd, uint8_t *type, uint8_t **payload, uint32_t *len);
void lndb_put_u32(uint8_t out[4], uint32_t value);
uint32_t lndb_get_u32(const uint8_t in[4]);

#endif
