#ifndef LNDBD_BLINK_QUEUE_H
#define LNDBD_BLINK_QUEUE_H

#include <stdint.h>
#include <sys/types.h>

/* The descriptors are inherited by client session children after fork. */
struct lndbd_blink_queue {
    pid_t worker_pid;
    int request_fd;
    int slots_fd;
};

int lndbd_blink_queue_start(struct lndbd_blink_queue *queue, int server_fd);
int lndbd_blink_queue_enqueue(const struct lndbd_blink_queue *queue,
                              uint32_t interval_ms, uint32_t times);
void lndbd_blink_queue_reap(struct lndbd_blink_queue *queue);
void lndbd_blink_queue_stop(struct lndbd_blink_queue *queue);

#endif
