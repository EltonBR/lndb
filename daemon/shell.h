#ifndef LNDBD_SHELL_H
#define LNDBD_SHELL_H

#include <stdint.h>

int lndbd_shell(int fd, const uint8_t *request, uint32_t len);

#endif
