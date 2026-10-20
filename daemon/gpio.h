#ifndef LNDBD_GPIO_H
#define LNDBD_GPIO_H

#include <stddef.h>

/* Blink the AN5506-01-A PON LED (active-low GPIO15).
 * Each ON and OFF phase lasts interval_ms. Valid ranges are 1..60000 ms
 * and 1..1000 cycles; total requested time may not exceed 120000 ms.
 * Returns 0 on success, -1 on failure and fills error when provided.
 */
int lndbd_gpio_pon_blink(unsigned int interval_ms, unsigned int times,
                         char *error, size_t error_size);

#endif
