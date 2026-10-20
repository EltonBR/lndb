#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include "shell.h"
#include "lndb_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int send_exit(int fd, int status)
{
    uint8_t b[4];
    if (WIFEXITED(status)) status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) status = 128 + WTERMSIG(status);
    else status = 255;
    lndb_put_u32(b, (uint32_t)status);
    return lndb_send_frame(fd, LNDB_MSG_SHELL_EXIT, b, sizeof(b));
}

static int wait_child(pid_t pid, int *status)
{
    pid_t r;
    do { r = waitpid(pid, status, 0); } while (r < 0 && errno == EINTR);
    return r == pid ? 0 : -1;
}

static void stop_process_group(pid_t pid)
{
    if (pid <= 0) return;
    (void)kill(-pid, SIGHUP);
    /* Give the shell a chance to clean up, then ensure descendants are gone. */
    usleep(100000);
    (void)kill(-pid, SIGTERM);
    usleep(100000);
    (void)kill(-pid, SIGKILL);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#define SCRIPT_LIMIT (16U * 1024U * 1024U)

static int shell_pty(int fd, const char *script_path)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    pid_t pid;
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        if (master >= 0) close(master);
        return -1;
    }
    char *name = ptsname(master);
    if (!name) { close(master); return -1; }
    pid = fork();
    if (pid == 0) {
        int slave, saved_errno;
        close(master);
        if (setsid() < 0) _exit(127);
        slave = open(name, O_RDWR);
        if (slave < 0) _exit(127);
        if (ioctl(slave, TIOCSCTTY, 0) < 0 ||
            dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) {
            saved_errno = errno;
            close(slave);
            errno = saved_errno;
            _exit(127);
        }
        if (slave > 2) close(slave);
        if (script_path) execl("/bin/sh", "sh", script_path, (char *)NULL);
        else execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { close(master); return -1; }
    int abort_child = 0;
    for (;;) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {master, POLLIN, 0}};
        int n = poll(p, 2, -1);
        if (n < 0) { if (errno == EINTR) continue; abort_child = 1; break; }
        if (p[0].revents & POLLNVAL) { abort_child = 1; break; }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            uint8_t t, *data = NULL; uint32_t len;
            if (lndb_recv_frame(fd, &t, &data, &len) < 0) { free(data); abort_child = 1; break; }
            if (t == LNDB_MSG_SHELL_STDIN) {
                if (len && lndb_write_full(master, data, len) < 0) { free(data); abort_child = 1; break; }
            } else if (t == LNDB_MSG_SHELL_RESIZE && len == 8) {
                struct winsize ws;
                ws.ws_row = (unsigned short)((data[0] << 8) | data[1]);
                ws.ws_col = (unsigned short)((data[2] << 8) | data[3]);
                ws.ws_xpixel = (unsigned short)((data[4] << 8) | data[5]);
                ws.ws_ypixel = (unsigned short)((data[6] << 8) | data[7]);
                (void)ioctl(master, TIOCSWINSZ, &ws);
            } else if (t == LNDB_MSG_CLOSE) { free(data); abort_child = 1; break; }
            else { free(data); abort_child = 1; break; }
            free(data);
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            uint8_t b[8192]; ssize_t got = read(master, b, sizeof(b));
            if (got <= 0) {
                if (got < 0 && errno != EIO && errno != EINTR) abort_child = 1;
                if (got < 0 && errno == EINTR) continue;
                break;
            }
            if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDOUT, b, (uint32_t)got) < 0) { abort_child = 1; break; }
        }
    }
    close(master);
    if (abort_child) stop_process_group(pid);
    int status = -1;
    if (wait_child(pid, &status) < 0) status = -1;
    return send_exit(fd, status);
}

static int shell_interactive_script(int fd)
{
    char path[] = "/tmp/lndbd-script-XXXXXX";
    int script_fd = mkstemp(path);
    uint32_t total = 0;
    int result = -1;
    if (script_fd < 0) {
        return lndb_send_frame(fd, LNDB_MSG_ERROR, "cannot create script file", 25);
    }

    for (;;) {
        uint8_t type, *data = NULL;
        uint32_t len;
        if (lndb_recv_frame(fd, &type, &data, &len) < 0) {
            free(data);
            goto done;
        }
        if (type == LNDB_MSG_CLOSE && len == 0) {
            free(data);
            break;
        }
        if (type != LNDB_MSG_SHELL_STDIN || len == 0 || len > SCRIPT_LIMIT - total) {
            free(data);
            (void)lndb_send_frame(fd, LNDB_MSG_ERROR,
                                  type == LNDB_MSG_SHELL_STDIN ? "script exceeds 16 MiB" : "invalid script upload",
                                  type == LNDB_MSG_SHELL_STDIN ? 20 : 21);
            goto done;
        }
        if (lndb_write_full(script_fd, data, len) < 0) {
            free(data);
            (void)lndb_send_frame(fd, LNDB_MSG_ERROR, "cannot write script file", 24);
            goto done;
        }
        total += len;
        free(data);
    }

    if (fsync(script_fd) < 0) {
        close(script_fd);
        script_fd = -1;
        (void)lndb_send_frame(fd, LNDB_MSG_ERROR, "cannot save script file", 23);
        goto done;
    }
    if (close(script_fd) < 0) {
        script_fd = -1;
        (void)lndb_send_frame(fd, LNDB_MSG_ERROR, "cannot save script file", 23);
        goto done;
    }
    script_fd = -1;
    result = shell_pty(fd, path);

done:
    if (script_fd >= 0) close(script_fd);
    unlink(path);
    return result;
}

static int shell_exec(int fd, const uint8_t *command, uint32_t len)
{
    int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
    int status = -1, abort_child = 0;
    int input_open = 1, output_open = 1, error_open = 1;
    pid_t pid;
    uint8_t *pending = NULL;
    uint32_t pending_len = 0, pending_offset = 0;
    if (pipe(out) < 0) goto setup_error;
    if (pipe(err) < 0) goto setup_error;
    if (pipe(in) < 0) goto setup_error;
    pid = fork();
    if (pid == 0) {
        close(out[0]); close(err[0]); close(in[1]);
        if (setpgid(0, 0) < 0 ||
            dup2(in[0], STDIN_FILENO) < 0 ||
            dup2(out[1], STDOUT_FILENO) < 0 ||
            dup2(err[1], STDERR_FILENO) < 0)
            _exit(127);
        close(in[0]); close(out[1]); close(err[1]);
        char *cmd = malloc((size_t)len + 1);
        if (!cmd) _exit(127);
        memcpy(cmd, command, len); cmd[len] = 0;
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) goto setup_error;
    /* The child also calls setpgid, so either side can win the fork race. */
    if (setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH) {
        abort_child = 1;
        goto parent_cleanup;
    }
    if (out[1] >= 0) close(out[1]);
    out[1] = -1;
    if (err[1] >= 0) close(err[1]);
    err[1] = -1;
    if (in[0] >= 0) close(in[0]);
    in[0] = -1;
    if (set_nonblock(out[0]) < 0 || set_nonblock(err[0]) < 0 || set_nonblock(in[1]) < 0) {
        abort_child = 1;
        goto parent_cleanup;
    }
    while (output_open || error_open || input_open) {
        struct pollfd p[4] = {
            {fd, (input_open && !pending) ? POLLIN : 0, 0},
            {out[0], output_open ? POLLIN : 0, 0},
            {err[0], error_open ? POLLIN : 0, 0},
            {in[1], (input_open && pending) ? POLLOUT : 0, 0}
        };
        if (poll(p, 4, -1) < 0) { if (errno == EINTR) continue; abort_child = 1; break; }
        if (p[0].revents & POLLNVAL) { abort_child = 1; break; }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            uint8_t t, *data = NULL; uint32_t n;
            if (lndb_recv_frame(fd, &t, &data, &n) < 0) { free(data); abort_child = 1; break; }
            if (t == LNDB_MSG_SHELL_STDIN && n && input_open) {
                pending = data;
                pending_len = n;
                pending_offset = 0;
                data = NULL;
            } else if (t == LNDB_MSG_CLOSE && input_open) {
                input_open = 0;
                close(in[1]); in[1] = -1;
            } else {
                free(data);
                abort_child = 1;
                break;
            }
            free(data);
        }
        if (pending && p[3].revents & (POLLOUT | POLLERR | POLLHUP)) {
            ssize_t n = write(in[1], pending + pending_offset, pending_len - pending_offset);
            if (n > 0) {
                pending_offset += (uint32_t)n;
                if (pending_offset == pending_len) {
                    free(pending); pending = NULL; pending_len = pending_offset = 0;
                }
            } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                free(pending); pending = NULL; pending_len = pending_offset = 0;
                input_open = 0;
                close(in[1]); in[1] = -1;
            }
        }
        for (int j = 1; j <= 2; j++) if (p[j].revents & (POLLIN | POLLHUP | POLLERR)) {
            int *active = j == 1 ? &output_open : &error_open;
            int stream = j == 1 ? LNDB_MSG_SHELL_STDOUT : LNDB_MSG_SHELL_STDERR;
            uint8_t b[8192]; ssize_t n = read(p[j].fd, b, sizeof(b));
            if (n > 0) {
                if (lndb_send_frame(fd, (uint8_t)stream, b, (uint32_t)n) < 0) {
                    *active = 0;
                    abort_child = 1;
                }
            } else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            } else { *active = 0; close(p[j].fd); if (j == 1) out[0] = -1; else err[0] = -1; }
        }
        if (abort_child) break;
    }
parent_cleanup:
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    if (err[0] >= 0) close(err[0]);
    if (err[1] >= 0) close(err[1]);
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    free(pending);
    if (abort_child) stop_process_group(pid);
    if (wait_child(pid, &status) < 0) status = -1;
    return send_exit(fd, status);

setup_error:
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    if (err[0] >= 0) close(err[0]);
    if (err[1] >= 0) close(err[1]);
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    return -1;
}

int lndbd_shell(int fd, const uint8_t *request, uint32_t len)
{
    if (len < 1) return lndb_send_frame(fd, LNDB_MSG_ERROR, "bad shell request", 18);
    if (request[0] == 0 && len == 1) return shell_pty(fd, NULL);
    if (request[0] == 2 && len == 1) return shell_interactive_script(fd);
    if (request[0] == 1 && memchr(request + 1, 0, len - 1))
        return lndb_send_frame(fd, LNDB_MSG_ERROR, "invalid command", 15);
    if (request[0] == 1) return shell_exec(fd, request + 1, len - 1);
    return lndb_send_frame(fd, LNDB_MSG_ERROR, "bad shell mode", 14);
}
