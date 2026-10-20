#define _POSIX_C_SOURCE 200809L
#include "gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GPIO_SYSFS "/sys/class/gpio"
#define PON_GPIO 15

static void set_error(char *error, size_t error_size, const char *what)
{
    if (error && error_size)
        (void)snprintf(error, error_size, "%s: %s", what, strerror(errno));
}

static int write_attr(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    size_t left = strlen(value);
    const char *p = value;
    if (fd < 0) return -1;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int saved = n == 0 ? EIO : errno;
            close(fd);
            errno = saved;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) < 0) return -1;
    return 0;
}

static int read_attr(const char *path, char *value, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t used = 0;
    if (fd < 0) return -1;
    while (used + 1 < capacity) {
        ssize_t n = read(fd, value + used, capacity - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    if (close(fd) < 0) return -1;
    if (used + 1 == capacity) {
        errno = EOVERFLOW;
        return -1;
    }
    while (used && (value[used - 1] == '\n' || value[used - 1] == '\r'))
        used--;
    value[used] = '\0';
    return 0;
}

static int check_gpiochip(void)
{
    char path[128], value[32], *end;
    long base, ngpio;
    snprintf(path, sizeof(path), "%s/gpiochip0/base", GPIO_SYSFS);
    if (read_attr(path, value, sizeof(value)) < 0) return -1;
    errno = 0;
    base = strtol(value, &end, 10);
    if (errno || end == value || *end || base != 0) {
        errno = ENODEV;
        return -1;
    }
    snprintf(path, sizeof(path), "%s/gpiochip0/ngpio", GPIO_SYSFS);
    if (read_attr(path, value, sizeof(value)) < 0) return -1;
    errno = 0;
    ngpio = strtol(value, &end, 10);
    if (errno || end == value || *end || ngpio != 74) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static int gpio_exported(const char *gpio_path)
{
    struct stat st;
    if (stat(gpio_path, &st) == 0) return S_ISDIR(st.st_mode);
    if (errno == ENOENT) return 0;
    return -1;
}

static int wait_for_gpio(const char *gpio_path)
{
    struct timespec pause = {0, 1000000L};
    int i;
    for (i = 0; i < 100; i++) {
        int present = gpio_exported(gpio_path);
        if (present == 1) return 0;
        if (present < 0) return -1;
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
        pause.tv_sec = 0;
        pause.tv_nsec = 1000000L;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int sleep_ms(unsigned int milliseconds)
{
    struct timespec remaining;
    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

int lndbd_gpio_pon_blink(unsigned int interval_ms, unsigned int times,
                         char *error, size_t error_size)
{
    char gpio_path[128], direction_path[160], value_path[160];
    char export_path[128], unexport_path[128];
    char previous_direction[16], previous_value[8];
    struct stat lock_stat;
    int exported_here = 0, have_previous = 0, changed_direction = 0, result = -1;
    int lock_fd = -1;
    int present;
    unsigned int i;

    if (error && error_size) error[0] = '\0';
    if (interval_ms < 1U || interval_ms > 60000U || times < 1U || times > 1000U ||
        2ULL * (unsigned long long)interval_ms * (unsigned long long)times > 120000ULL) {
        errno = EINVAL;
        set_error(error, error_size, "invalid PON LED blink interval or count");
        return -1;
    }
    snprintf(gpio_path, sizeof(gpio_path), "%s/gpio%d", GPIO_SYSFS, PON_GPIO);
    snprintf(direction_path, sizeof(direction_path), "%s/direction", gpio_path);
    snprintf(value_path, sizeof(value_path), "%s/value", gpio_path);
    snprintf(export_path, sizeof(export_path), "%s/export", GPIO_SYSFS);
    snprintf(unexport_path, sizeof(unexport_path), "%s/unexport", GPIO_SYSFS);

    lock_fd = open("/tmp/lndbd-gpio15.lock",
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        set_error(error, error_size, "opening GPIO15 lock");
        return -1;
    }
    if (fstat(lock_fd, &lock_stat) < 0) {
        set_error(error, error_size, "checking GPIO15 lock file");
        close(lock_fd);
        return -1;
    }
    if (!S_ISREG(lock_stat.st_mode) || lock_stat.st_uid != geteuid() ||
        (lock_stat.st_mode & 077) != 0) {
        errno = EPERM;
        set_error(error, error_size, "unsafe GPIO15 lock file");
        close(lock_fd);
        return -1;
    }
    while (flock(lock_fd, LOCK_EX) < 0) {
        if (errno == EINTR) continue;
        set_error(error, error_size, "locking GPIO15");
        close(lock_fd);
        return -1;
    }

    if (check_gpiochip() < 0) {
        set_error(error, error_size, "GPIO chip mismatch or unavailable (expected gpiochip0 base=0 ngpio=74)");
        goto cleanup;
    }
    present = gpio_exported(gpio_path);
    if (present < 0) {
        set_error(error, error_size, "checking GPIO15 export state");
        goto cleanup;
    }
    if (!present) {
        char pin[] = "15";
        if (write_attr(export_path, pin) < 0) {
            /* Another process may have exported it concurrently. */
            present = gpio_exported(gpio_path);
            if (present != 1) {
                set_error(error, error_size, "exporting GPIO15");
                goto cleanup;
            }
        } else {
            exported_here = 1;
        }
        if (wait_for_gpio(gpio_path) < 0) {
            set_error(error, error_size, "waiting for GPIO15 sysfs node");
            goto cleanup;
        }
    }

    if (read_attr(direction_path, previous_direction, sizeof(previous_direction)) < 0) {
        set_error(error, error_size, "reading GPIO15 direction");
        goto cleanup;
    }
    if (strcmp(previous_direction, "in") && strcmp(previous_direction, "out")) {
        errno = EINVAL;
        set_error(error, error_size, "unexpected GPIO15 direction");
        goto cleanup;
    }
    if (read_attr(value_path, previous_value, sizeof(previous_value)) < 0) {
        set_error(error, error_size, "reading GPIO15 value");
        goto cleanup;
    }
    if (strcmp(previous_value, "0") && strcmp(previous_value, "1")) {
        errno = EINVAL;
        set_error(error, error_size, "unexpected GPIO15 value");
        goto cleanup;
    }
    have_previous = 1;

    for (i = 0; i < times; i++) {
        /* Sysfs accepts low/high to set output direction and level atomically. */
        changed_direction = 1;
        if (write_attr(direction_path, "low") < 0) {
            set_error(error, error_size, "lighting PON LED via GPIO15");
            goto cleanup;
        }
        if (sleep_ms(interval_ms) < 0) {
            set_error(error, error_size, "waiting during PON LED on phase");
            goto cleanup;
        }
        if (write_attr(direction_path, "high") < 0) {
            set_error(error, error_size, "turning PON LED off via GPIO15");
            goto cleanup;
        }
        if (sleep_ms(interval_ms) < 0) {
            set_error(error, error_size, "waiting during PON LED off phase");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (have_previous && changed_direction) {
        const char *restore = !strcmp(previous_direction, "out")
            ? (!strcmp(previous_value, "1") ? "high" : "low")
            : "in";
        if (write_attr(direction_path, restore) < 0 && result == 0) {
            set_error(error, error_size, "restoring GPIO15 state");
            result = -1;
        }
    }
    if (exported_here && write_attr(unexport_path, "15") < 0 && result == 0) {
        set_error(error, error_size, "unexporting GPIO15");
        result = -1;
    }
    if (lock_fd >= 0) {
        (void)flock(lock_fd, LOCK_UN);
        (void)close(lock_fd);
    }
    return result;
}
