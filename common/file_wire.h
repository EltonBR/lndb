#ifndef LNDB_FILE_WIRE_H
#define LNDB_FILE_WIRE_H
#include <stddef.h>
#include <stdint.h>

/* FILE_META payload: kind, mode, size, path length, path bytes. */
int lndb_file_meta_encode(uint8_t kind, uint32_t mode, uint64_t size,
                          const char *path, uint8_t **payload, uint32_t *length);
int lndb_file_meta_decode(const uint8_t *payload, uint32_t length,
                          uint8_t *kind, uint32_t *mode, uint64_t *size,
                          const uint8_t **path, size_t *path_length);
#endif
