#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "lndb_config.h"
#include "lndb_protocol.h"
#include "lndb_version.h"
#include "transfer.h"
#include "shell.h"
#include "connection.h"
#include "config_store.h"
#include "discovery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (!config_path) { auto_path = lndb_client_default_config_path(); config_path = auto_path; }
    struct lndb_client_config cfg; char err[256];
    if (config_path && lndb_client_config_ensure(config_path) < 0) {
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
        if (!lndb_client_valid_api_key(key)) {
            fprintf(stderr, "lndb: API key must contain exactly 64 hexadecimal characters\n"); rc = 2; goto out;
        }
        if (lndb_client_parse_endpoint(endpoint, cfg.port, &host, &port) < 0) {
            fprintf(stderr, "lndb: invalid endpoint: %s\n", endpoint); rc = 2; goto out;
        }
        char *single_key[] = { (char *)key };
        struct lndb_client_config one = cfg;
        one.port = port; one.api_keys = single_key; one.api_key_count = 1;
        int tcp_connected = 0, identified = 0;
        int fd = lndb_client_connect_auth(host, &one, 1, &tcp_connected, &identified);
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
        if (lndb_client_config_add_key(config_path, &cfg, key) < 0) {
            perror("lndb: updating client config"); rc = 1; goto out;
        }
        printf("LNDB device authenticated; API key added to %s\n", config_path);
        rc = 0;
    }
    else if (!strcmp(argv[arg], "devices")) rc = lndb_client_devices(&cfg, config_path);
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
        int id_result = lndb_client_resolve_device_id(endpoint, config_path, &host, &port);
        if (id_result < 0) {
            fprintf(stderr, "lndb: unknown or invalid device ID '%s'; run 'lndb devices' to refresh IDs\n", endpoint);
            rc = 2;
        } else if (id_result == 0 && lndb_client_parse_endpoint(endpoint, cfg.port, &host, &port) < 0) {
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
            int fd = lndb_client_connect_auth(host, &cfg, 0, &tcp_connected, &identified);
            if (fd < 0) {
                const char *reason = identified ? "API key rejected (unauthorized)" :
                    tcp_connected ? "TCP connected, but the peer did not send a valid LNDB1 greeting" :
                    "could not establish a TCP connection to the device";
                fprintf(stderr, "lndb: %s: %s\n", endpoint, reason);
                rc = 1;
            }
            else {
                if (!strcmp(op, "shell")) rc = lndb_client_shell_run(fd, argc - arg, argv + arg);
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
