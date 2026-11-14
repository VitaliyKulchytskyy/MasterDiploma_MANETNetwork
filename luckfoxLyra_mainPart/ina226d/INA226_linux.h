//  FILE: INA226_linux.h
//  Platform implementation of the INA226 library hooks for Linux (/dev/i2c-N).

#ifndef INA226_LINUX_H
#define INA226_LINUX_H

/**
 * Open the I2C bus the INA226 is on, e.g. "/dev/i2c-1".
 * Call once before INA226_init(); the hooks use this bus for every device.
 *
 * @return 0 on success, -1 on error (errno is set).
 */
int INA226_linuxOpen(const char *path);

void INA226_linuxClose(void);

#endif // INA226_LINUX_H
