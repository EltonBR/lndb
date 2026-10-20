#include "file_wire.h"
#include "lndb_protocol.h"
#include <stdlib.h>
#include <string.h>

int lndb_file_meta_encode(uint8_t kind, uint32_t mode, uint64_t size,
                          const char *path, uint8_t **payload, uint32_t *length)
{
    if (!path || !payload || !length || kind > 2 ||
        (kind == 0 && *path != '\0') || (kind != 0 && *path == '\0'))
        return -1;
    size_t n = strlen(path);
    if (n > UINT16_MAX || n + 15 > LNDB_FRAME_MAX_PAYLOAD) return -1;
    uint8_t *p = malloc(n + 15);
    if (!p) return -1;
    p[0] = kind;
    lndb_put_u32(p + 1, mode);
    for (unsigned i = 0; i < 8; i++) p[5 + i] = (uint8_t)(size >> (56 - 8 * i));
    p[13] = (uint8_t)(n >> 8);
    p[14] = (uint8_t)n;
    memcpy(p + 15, path, n);
    *payload = p;
    *length = (uint32_t)(n + 15);
    return 0;
}

int lndb_file_meta_decode(const uint8_t *p, uint32_t n,
                          uint8_t *kind, uint32_t *mode, uint64_t *size,
                          const uint8_t **path, size_t *path_length)
{
    if (!p || n < 15 || !kind || !mode || !size || !path || !path_length) return -1;
    size_t pn = ((size_t)p[13] << 8) | p[14];
    if (pn != (size_t)n - 15 || memchr(p + 15, 0, pn) || p[0] > 2 ||
        (p[0] == 0 && pn != 0) || (p[0] != 0 && pn == 0))
        return -1;
    *kind = p[0];
    *mode = lndb_get_u32(p + 1);
    *size = 0;
    for (unsigned i = 0; i < 8; i++) *size = (*size << 8) | p[5 + i];
    *path = p + 15;
    *path_length = pn;
    return 0;
}
