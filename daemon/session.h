#ifndef LNDBD_SESSION_H
#define LNDBD_SESSION_H

#include "blink_queue.h"

/* Authenticate a connected client and dispatch its first operation. */
void lndbd_session_serve(int fd, const char *api_key,
                         const struct lndbd_blink_queue *blink_queue);

#endif
