#ifndef LNDBD_SESSION_H
#define LNDBD_SESSION_H

#include "lndb_features.h"
#if LNDB_ENABLE_PON_BLINK
#include "blink_queue.h"
#endif

/* Authenticate a connected client and dispatch its first operation. */
#if LNDB_ENABLE_PON_BLINK
void lndbd_session_serve(int fd, const char *api_key,
                         const struct lndbd_blink_queue *blink_queue);
#else
void lndbd_session_serve(int fd, const char *api_key);
#endif

#endif
