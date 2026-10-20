#ifndef LNDB_CLIENT_DISCOVERY_H
#define LNDB_CLIENT_DISCOVERY_H

#include "lndb_config.h"

#include <stdint.h>

/* Convert CIDR, IPv4 range, or final-octet wildcard into host-order bounds. */
int lndb_client_ip_range(const char *spec, uint32_t *first_ip,
                         uint32_t *last_ip);

/* Persist the latest scan and print the existing devices listing. */
int lndb_client_devices(const struct lndb_client_config *cfg,
                        const char *config_path);

/* Returns 1 for a resolved numeric ID, 0 for a host endpoint, or -1 on error. */
int lndb_client_resolve_device_id(const char *endpoint,
                                  const char *config_path,
                                  char **host_out, uint16_t *port_out);

#endif
