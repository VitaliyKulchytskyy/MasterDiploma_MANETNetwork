#include "alert.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_CHIPS 16

static int g_fd = -1;       // the line handle

bool alert_open(const char* name) {
    for (int chip = 0; chip < MAX_CHIPS; ++chip) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpiochip%d", chip);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct gpiochip_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
            close(fd);
            continue;
        }
        for (unsigned line = 0; line < info.lines; ++line) {
            struct gpioline_info li;
            memset(&li, 0, sizeof(li));
            li.line_offset = line;
            if (ioctl(fd, GPIO_GET_LINEINFO_IOCTL, &li) < 0 || strcmp(li.name, name) != 0)
                continue;
            struct gpiohandle_request req;
            memset(&req, 0, sizeof(req));
            req.lineoffsets[0] = line;
            req.lines = 1;
            req.flags = GPIOHANDLE_REQUEST_OUTPUT;
            req.default_values[0] = 0;
            snprintf(req.consumer_label, sizeof(req.consumer_label), "merld");
            if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
                fprintf(stderr, "alert: %s is %s line %u, cannot request it: %s\n", name, path, line, strerror(errno));
                close(fd);
                return false;
            }
            close(fd);
            g_fd = req.fd;
            fprintf(stderr, "alert: %s = %s line %u, output low\n", name, path, line);
            return true;
        }
        close(fd);
    }
    fprintf(stderr, "alert: no GPIO line named %s (device tree without it?): SOS alerts to the unit off\n", name);
    return false;
}

void alert_set(bool on) {
    if (g_fd < 0)
        return;
    struct gpiohandle_data data;
    memset(&data, 0, sizeof(data));
    data.values[0] = on ? 1 : 0;
    if (ioctl(g_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0)
        fprintf(stderr, "alert: set %d: %s\n", on, strerror(errno));
}

bool alert_available(void) {
    return g_fd >= 0;
}

void alert_close(void) {
    if (g_fd < 0)
        return;
    alert_set(false);
    close(g_fd);
    g_fd = -1;
}
