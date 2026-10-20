#ifndef LNDB_CONFIG_H
#define LNDB_CONFIG_H

#include <stddef.h>
#include <stdint.h>

struct lndb_server_config {
    char bind[64];
    uint16_t port;
    char api_key[65];
};

struct lndb_client_config {
    uint16_t port;
    unsigned connect_timeout_ms;
    unsigned concurrency;
    char network[64];
    char **api_keys;
    size_t api_key_count;
};

/* Server load creates parent directories and a mode-0600 config with a fresh
 * key when path does not exist. Existing invalid files are never replaced. */
int lndb_server_config_load(const char *path, struct lndb_server_config *out,
                            char *err, size_t errlen);
int lndb_client_config_load(const char *path, struct lndb_client_config *out,
                            char *err, size_t errlen);
void lndb_client_config_free(struct lndb_client_config *cfg);

#endif
