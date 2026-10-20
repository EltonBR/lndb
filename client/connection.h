#ifndef LNDB_CLIENT_CONNECTION_H
#define LNDB_CLIENT_CONNECTION_H

#include "lndb_config.h"

#include <stdint.h>

/* Parse HOST[:PORT] or [IPv6]:PORT. The caller owns *host_out. */
int lndb_client_parse_endpoint(const char *endpoint, uint16_t default_port,
                               char **host_out, uint16_t *port_out);

/* Connect with a bounded TCP connect timeout. Returns a connected fd or -1. */
int lndb_client_connect_host(const char *host, uint16_t port,
                             unsigned timeout_ms);

/*
 * Try configured API keys in order. Success returns an authenticated socket
 * which remains open. On failure returns -1; tcp_connected and identified
 * describe whether TCP connected and a valid LNDB greeting was received.
 */
int lndb_client_connect_auth(const char *host,
                             const struct lndb_client_config *cfg,
                             int io_timeout, int *tcp_connected,
                             int *identified);

int lndb_client_valid_api_key(const char *key);

#endif
