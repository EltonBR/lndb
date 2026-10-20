#define _POSIX_C_SOURCE 200809L
#include "blink_queue.h"
#include "lndb_protocol.h"
#include "gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLINK_QUEUE_CAPACITY 16U
#define BLINK_REQUEST_SIZE 8U

static volatile sig_atomic_t worker_stopping;

static void on_worker_signal(int sig)
{
    (void)sig;
    worker_stopping = 1;
}

static int set_fd_flag(int fd, int command, int flag)
{
    int value = fcntl(fd, command == F_GETFL ? F_GETFL : F_GETFD);
    int set_command = command == F_GETFL ? F_SETFL : F_SETFD;
    if (value < 0) return -1;
    return fcntl(fd, set_command, value | flag);
}

static int read_pipe_record(int fd, uint8_t record[BLINK_REQUEST_SIZE])
{
    size_t used = 0;
    while (used < BLINK_REQUEST_SIZE) {
        ssize_t n = read(fd, record + used, BLINK_REQUEST_SIZE - used);
        if (n > 0) { used += (size_t)n; continue; }
        if (n == 0) return used == 0 ? 0 : -1;
        if (errno == EINTR) {
            if (worker_stopping) return 0;
            continue;
        }
        return -1;
    }
    return 1;
}

static void blink_worker(int request_fd, int slot_fd)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_worker_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    for (;;) {
        uint8_t request[BLINK_REQUEST_SIZE];
        int status = read_pipe_record(request_fd, request);
        if (status <= 0 || worker_stopping) break;
        uint32_t interval_ms = lndb_get_u32(request);
        uint32_t times = lndb_get_u32(request + 4);
        char error[160];
        if (lndbd_gpio_pon_blink(interval_ms, times, error, sizeof(error)) < 0)
            fprintf(stderr, "lndbd: queued PON LED blink failed: %s\n",
                    error[0] ? error : "GPIO15 operation failed");
        /* Return one admission token only after the request has completed. */
        uint8_t slot = 1;
        while (write(slot_fd, &slot, sizeof(slot)) < 0 && errno == EINTR) {}
        if (worker_stopping) break;
    }
    close(request_fd);
    close(slot_fd);
    _exit(0);
}

static void close_pipes(int request_pipe[2], int slot_pipe[2])
{
    int i;
    for (i = 0; i < 2; i++) {
        if (request_pipe[i] >= 0) close(request_pipe[i]);
        if (slot_pipe[i] >= 0) close(slot_pipe[i]);
    }
}

int lndbd_blink_queue_start(struct lndbd_blink_queue *queue, int server_fd)
{
    int request_pipe[2] = {-1, -1}, slot_pipe[2] = {-1, -1};
    pid_t pid;
    uint8_t slots[BLINK_QUEUE_CAPACITY];

    if (!queue) { errno = EINVAL; return -1; }
    queue->worker_pid = 0;
    queue->request_fd = -1;
    queue->slots_fd = -1;
    queue->slots_return_fd = -1;
    if (pipe(request_pipe) < 0 || pipe(slot_pipe) < 0) goto fail;
    if (set_fd_flag(request_pipe[0], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(request_pipe[1], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(slot_pipe[0], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(slot_pipe[1], F_GETFD, FD_CLOEXEC) < 0 ||
        set_fd_flag(request_pipe[1], F_GETFL, O_NONBLOCK) < 0 ||
        set_fd_flag(slot_pipe[0], F_GETFL, O_NONBLOCK) < 0)
        goto fail;
    memset(slots, 1, sizeof(slots));
    if (lndb_write_full(slot_pipe[1], slots, sizeof(slots)) < 0) goto fail;

    pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        close(server_fd);
        close(request_pipe[1]);
        close(slot_pipe[0]);
        signal(SIGCHLD, SIG_DFL);
        blink_worker(request_pipe[0], slot_pipe[1]);
    }
    close(request_pipe[0]);
    queue->worker_pid = pid;
    queue->request_fd = request_pipe[1];
    queue->slots_fd = slot_pipe[0];
    queue->slots_return_fd = slot_pipe[1];
    return 0;

fail:
    {
        int saved_errno = errno;
        close_pipes(request_pipe, slot_pipe);
        errno = saved_errno;
        return -1;
    }
}

int lndbd_blink_queue_enqueue(const struct lndbd_blink_queue *queue,
                              uint32_t interval_ms, uint32_t times)
{
    uint8_t slot, request[BLINK_REQUEST_SIZE];
    ssize_t n;
    do { n = read(queue->slots_fd, &slot, sizeof(slot)); }
    while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(slot)) return -1;

    lndb_put_u32(request, interval_ms);
    lndb_put_u32(request + 4, times);
    do { n = write(queue->request_fd, request, sizeof(request)); }
    while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(request)) {
        int saved_errno = errno;
        while (write(queue->slots_return_fd, &slot, sizeof(slot)) < 0 && errno == EINTR) {}
        errno = saved_errno;
        return -1;
    }
    return 0;
}

void lndbd_blink_queue_reap(struct lndbd_blink_queue *queue)
{
    pid_t waited;
    if (!queue || queue->worker_pid <= 0) return;
    waited = waitpid(queue->worker_pid, NULL, WNOHANG);
    if (waited == queue->worker_pid || (waited < 0 && errno == ECHILD))
        queue->worker_pid = 0;
}

void lndbd_blink_queue_stop(struct lndbd_blink_queue *queue)
{
    if (!queue) return;
    if (queue->request_fd >= 0) close(queue->request_fd);
    queue->request_fd = -1;
    /* The worker finishes an active pulse, restoring GPIO state, then exits. */
    if (queue->worker_pid > 0) {
        kill(queue->worker_pid, SIGTERM);
        while (waitpid(queue->worker_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    queue->worker_pid = 0;
    if (queue->slots_fd >= 0) close(queue->slots_fd);
    queue->slots_fd = -1;
    if (queue->slots_return_fd >= 0) close(queue->slots_return_fd);
    queue->slots_return_fd = -1;
}
