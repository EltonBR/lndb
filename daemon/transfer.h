#ifndef LNDBD_TRANSFER_H
#define LNDBD_TRANSFER_H

#include <stdint.h>

int lndbd_transfer(int fd, const uint8_t *request, uint32_t len);

#endif
