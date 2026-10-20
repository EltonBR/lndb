#define _POSIX_C_SOURCE 200809L
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
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t got_winch;
static void on_winch(int sig) { (void)sig; got_winch = 1; }


static int set_raw(int fd, struct termios *old)
{
    if (!isatty(fd) || tcgetattr(fd, old) < 0) return -1;
    struct termios t = *old;
    t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSAFLUSH, &t);
}

static int send_resize(int fd, int tty_fd)
{
    struct winsize ws;
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) < 0) memset(&ws, 0, sizeof(ws));
    uint8_t b[8];
    b[0] = (uint8_t)(ws.ws_row >> 8); b[1] = (uint8_t)ws.ws_row;
    b[2] = (uint8_t)(ws.ws_col >> 8); b[3] = (uint8_t)ws.ws_col;
    b[4] = b[5] = b[6] = b[7] = 0;
    return lndb_send_frame(fd, LNDB_MSG_SHELL_RESIZE, b, sizeof(b));
}

int lndb_client_shell_run(int fd, int argc, char **argv)
{
    int interactive = argc == 0 && isatty(STDIN_FILENO);
    int script_interactive = 0, tty_fd = -1;
    if (argc == 0 && !interactive) {
        tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty_fd >= 0 && isatty(tty_fd)) script_interactive = 1;
        else if (tty_fd >= 0) { close(tty_fd); tty_fd = -1; }
    }
    const char *default_command = "/bin/sh";
    size_t clen = (argc == 0 && !interactive && !script_interactive) ? strlen(default_command) : 0;
    for (int i = 0; i < argc; ++i) clen += strlen(argv[i]) + (i != 0);
    char *cmd = malloc(clen + 1);
    if (!cmd) { if (tty_fd >= 0) close(tty_fd); return -1; }
    size_t pos = 0;
    if (argc == 0 && !interactive && !script_interactive) { memcpy(cmd, default_command, clen); pos = clen; }
    for (int i = 0; i < argc; ++i) { if (i) cmd[pos++] = ' '; size_t n = strlen(argv[i]); memcpy(cmd + pos, argv[i], n); pos += n; }
    cmd[pos] = '\0';
    uint8_t *req = malloc(clen + 1);
    if (!req) { free(cmd); if (tty_fd >= 0) close(tty_fd); return -1; }
    req[0] = script_interactive ? 2 : (interactive ? 0 : 1); memcpy(req + 1, cmd, clen); free(cmd);
    if (lndb_send_frame(fd, LNDB_MSG_SHELL_REQUEST, req, (uint32_t)(clen + 1)) < 0) {
        free(req); if (tty_fd >= 0) close(tty_fd); return -1;
    }
    free(req);
    if (script_interactive) {
        int rc = -1;
        uint8_t b[4096];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, b, sizeof(b));
            if (n > 0) {
                if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) goto script_interactive_done;
            } else if (n == 0) break;
            else if (errno == EINTR) continue;
            else goto script_interactive_done;
        }
        if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) goto script_interactive_done;
        /* Fall through to the PTY loop, using the controlling terminal even
         * though stdin itself was redirected from the script. */
        {
            struct termios old;
            int raw = set_raw(tty_fd, &old) == 0;
            struct sigaction sa, oldsa;
            memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_winch; sigemptyset(&sa.sa_mask);
            int have_oldsa = sigaction(SIGWINCH, &sa, &oldsa) == 0;
            (void)send_resize(fd, tty_fd);
            int done = 0, status = 0;
            while (!done) {
                if (got_winch) { got_winch = 0; if (send_resize(fd, tty_fd) < 0) { status = 1; break; } }
                struct pollfd pfd[2] = {{ fd, POLLIN, 0 }, { tty_fd, POLLIN, 0 }};
                int r = poll(pfd, 2, -1);
                if (r < 0) { if (errno == EINTR) continue; status = 1; break; }
                if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                    uint8_t type, *p = NULL; uint32_t n = 0;
                    if (lndb_recv_frame(fd, &type, &p, &n) < 0) { status = 1; break; }
                    if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                        if (n && lndb_write_full(tty_fd, p, n) < 0) status = 1;
                    } else if (type == LNDB_MSG_SHELL_EXIT) {
                        if (n == 4) status = (int)lndb_get_u32(p);
                        done = 1;
                    } else if (type == LNDB_MSG_ERROR) {
                        if (n) (void)lndb_write_full(tty_fd, p, n);
                        status = 1; done = 1;
                    }
                    free(p);
                    if (status == 1 && !done) break;
                }
                if (!done && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                    ssize_t n = read(tty_fd, b, sizeof(b));
                    if (n <= 0) {
                        (void)lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0);
                        done = 1;
                    } else if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) {
                        status = 1; done = 1;
                    }
                }
            }
            if (have_oldsa) (void)sigaction(SIGWINCH, &oldsa, NULL);
            if (raw) (void)tcsetattr(tty_fd, TCSAFLUSH, &old);
            rc = status;
        }
script_interactive_done:
        close(tty_fd);
        return rc;
    }
    if (!interactive) {
        int input_open = 1, status = 255;
        for (;;) {
            struct pollfd pfd[2] = {
                { fd, POLLIN, 0 },
                { input_open ? STDIN_FILENO : -1, POLLIN, 0 }
            };
            int ready = poll(pfd, 2, -1);
            if (ready < 0) { if (errno == EINTR) continue; return -1; }

            if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                uint8_t type, *p = NULL; uint32_t n = 0;
                if (lndb_recv_frame(fd, &type, &p, &n) < 0) return status;
                if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                    int out = type == LNDB_MSG_SHELL_STDERR ? STDERR_FILENO : STDOUT_FILENO;
                    if (n && lndb_write_full(out, p, n) < 0) { free(p); return -1; }
                } else if (type == LNDB_MSG_SHELL_EXIT && n == 4) {
                    uint32_t v = lndb_get_u32(p);
                    status = v > 255 ? 255 : (int)v;
                    free(p);
                    return status;
                } else if (type == LNDB_MSG_ERROR) {
                    if (n) (void)lndb_write_full(STDERR_FILENO, p, n);
                    free(p);
                    return 1;
                }
                free(p);
            }

            if (input_open && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                uint8_t b[4096];
                ssize_t n = read(STDIN_FILENO, b, sizeof(b));
                if (n > 0) {
                    if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) return -1;
                } else if (n == 0) {
                    if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) return -1;
                    input_open = 0;
                } else if (errno != EINTR) {
                    if (lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0) < 0) return -1;
                    input_open = 0;
                }
            }
        }
    }
    tty_fd = STDIN_FILENO;
    struct termios old; int raw = set_raw(tty_fd, &old) == 0;
    struct sigaction sa, oldsa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_winch; sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, &oldsa); (void)send_resize(fd, tty_fd);
    int done = 0, status = 0;
    while (!done) {
        if (got_winch) { got_winch = 0; if (send_resize(fd, tty_fd) < 0) break; }
        struct pollfd pfd[2] = {{ fd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 }};
        int r = poll(pfd, 2, -1); if (r < 0) { if (errno == EINTR) continue; break; }
        if (pfd[0].revents & (POLLIN | POLLHUP)) {
            uint8_t type, *p; uint32_t n;
            if (lndb_recv_frame(fd, &type, &p, &n) < 0) break;
            if (type == LNDB_MSG_SHELL_STDOUT || type == LNDB_MSG_SHELL_STDERR) {
                int out = type == LNDB_MSG_SHELL_STDERR ? STDERR_FILENO : STDOUT_FILENO;
                if (n && lndb_write_full(out, p, n) < 0) { free(p); break; }
            } else if (type == LNDB_MSG_SHELL_EXIT) { if (n == 4) status = (int)lndb_get_u32(p); done = 1; }
            else if (type == LNDB_MSG_ERROR) { if (n) (void)lndb_write_full(STDERR_FILENO, p, n); done = 1; status = 1; }
            free(p);
        }
        if (!done && (pfd[1].revents & (POLLIN | POLLHUP))) {
            uint8_t b[4096]; ssize_t n = read(STDIN_FILENO, b, sizeof(b));
            if (n <= 0) { (void)lndb_send_frame(fd, LNDB_MSG_CLOSE, NULL, 0); done = 1; }
            else if (lndb_send_frame(fd, LNDB_MSG_SHELL_STDIN, b, (uint32_t)n) < 0) break;
        }
    }
    sigaction(SIGWINCH, &oldsa, NULL); if (raw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    return status;
}

