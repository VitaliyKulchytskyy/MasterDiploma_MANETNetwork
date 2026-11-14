//  FILE: INA226_linux.c
//  Platform implementation for Linux i2c-dev (/dev/i2c-N), the counterpart of
//  INA226_pico.c in rp/ina226_demo. Overrides the weak hooks of INA226.c.

#include "INA226_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "INA226.h"

static int g_fd = -1;
static int g_address = -1;     // slave address g_fd is currently set to

int INA226_linuxOpen(const char *path)
{
    INA226_linuxClose();
    g_fd = open(path, O_RDWR | O_CLOEXEC);
    return g_fd < 0 ? -1 : 0;
}

void INA226_linuxClose(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    g_address = -1;
}

static int select_device(const INA226_t *dev)
{
    if (g_fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (g_address != dev->address) {
        if (ioctl(g_fd, I2C_SLAVE, dev->address) < 0)
            return -1;
        g_address = dev->address;
    }
    return 0;
}

int INA226_writeRegister(INA226_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {
        reg,
        (uint8_t)(value >> 8),      /* MSB first (big-endian) */
        (uint8_t)(value & 0xFF),
    };
    if (select_device(dev) < 0)
        return -1;
    return write(g_fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : -1;
}

int INA226_readRegister(INA226_t *dev, uint8_t reg, uint16_t *out)
{
    /* register pointer, then a separate read transaction, like the Pico port */
    uint8_t buf[2] = {0, 0};
    if (select_device(dev) < 0)
        return -1;
    if (write(g_fd, &reg, 1) != 1)
        return -1;
    if (read(g_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
        return -1;

    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return 0;
}

uint32_t INA226_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void INA226_delayMs(const uint32_t ms)
{
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}
