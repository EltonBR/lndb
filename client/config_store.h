#ifndef LNDB_CLIENT_CONFIG_STORE_H
#define LNDB_CLIENT_CONFIG_STORE_H

#include "lndb_config.h"

/* Returns an allocated path for the per-user client configuration. */
char *lndb_client_default_config_path(void);

/* Create the default client configuration when path does not exist. */
int lndb_client_config_ensure(const char *path);

/* Atomically append key to the configured API key list. */
int lndb_client_config_add_key(const char *path,
                               const struct lndb_client_config *cfg,
                               const char *key);

#endif
