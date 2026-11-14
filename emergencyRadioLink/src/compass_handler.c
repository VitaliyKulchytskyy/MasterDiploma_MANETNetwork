#include "compass_handler.h"

#include <string.h>


static compass_reading_t g_reading;
static bool g_have_reading;

bool compass_handler_get(compass_reading_t* out) {
    if (!g_have_reading || out == NULL)
        return false;

    *out = g_reading;
    return true;
}

bool compass_handler_set(const int16_t x, const int16_t y, const int16_t z) {
    g_reading.x = x;
    g_reading.y = y;
    g_reading.z = z;
    g_have_reading = true;
    return true;
}

#ifdef FEATURE_COMPASS

#include "qmc5883.h"

#if !defined(CONFIG_I2C_COMPAS_HW) || !defined(CONFIG_I2C_COMPAS_SDA) || !defined(CONFIG_I2C_COMPAS_SCL)
#error "FEATURE_COMPASS needs CONFIG_I2C_COMPAS_HW/SDA/SCL (see CMakePresets.json)"
#endif

void compass_handler_init(void) {
    g_have_reading = false;
    memset(&g_reading, 0, sizeof(g_reading));

    const qmc5883_config_t config = {
        .i2c = CONFIG_I2C_COMPAS_HW,
        .sda_pin = CONFIG_I2C_COMPAS_SDA,
        .scl_pin = CONFIG_I2C_COMPAS_SCL,
        .baudrate = 0,   // 0 -> default 100 kHz
    };
    qmc5883_init(&config);
}

void compass_handler_service(void) {
    int16_t x, y, z;
    if (!qmc5883_read(&x, &y, &z))
        return;

    g_reading.x = x;
    g_reading.y = y;
    g_reading.z = z;
    g_have_reading = true;
}

#else // !FEATURE_COMPASS

// No compass hardware support compiled in (FEATURE_COMPASS missing from CONFIG_DEVICE_FEATURES
// in CMakePresets.json): nothing to poll, but compass_handler_get/set() above still work, so a
// reading set manually over MERL is still cached and broadcast normally.
void compass_handler_init(void) {
    g_have_reading = false;
    memset(&g_reading, 0, sizeof(g_reading));
}

void compass_handler_service(void) {}

#endif // FEATURE_COMPASS
