#ifndef EMERGENCYRADIOLINK_WS2812_H
#define EMERGENCYRADIOLINK_WS2812_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Byte order the LED expects on its data line.
 *
 * Genuine WS2812/WS2812B parts always use GRB, but a board's "onboard RGB LED" is often a
 * clone or a different part wired for a different order, so this is picked at init time
 * instead of hardcoded. If colors come out wrong (e.g. red shows as green), try another value.
 */
typedef enum {
    WS2812_ORDER_RGB,
    WS2812_ORDER_RBG,
    WS2812_ORDER_GRB,   /**< The order almost every genuine WS2812/WS2812B uses. */
    WS2812_ORDER_GBR,
    WS2812_ORDER_BRG,
    WS2812_ORDER_BGR,
} ws2812_order_t;

/** @brief Configuration for the onboard WS2812. */
typedef struct {
    uint32_t pin;          /**< GPIO number of the LED's data line. */
    float frequency_hz;    /**< Bit rate. 0 defaults to 800000.0f (WS2812/WS2812B/SK6812). */
    ws2812_order_t order;  /**< Byte order this particular LED expects on the wire. */
    uint8_t brightness;    /**< 0-255 (255 = full). A zero-initialized config starts at 0 (off). */
} ws2812_config_t;

/**
 * @brief Claim a free PIO state machine and start driving the LED.
 *
 * Only one LED (state machine) is supported per ws2812_init() call; a second call before
 * ws2812_deinit() replaces the first without releasing its state machine.
 *
 * @param config Configuration, must not be NULL.
 * @return true on success, false if no PIO block reachable from @p config->pin has a free
 *         state machine and program space.
 */
bool ws2812_init(const ws2812_config_t* config);

/**
 * @brief Release the PIO state machine and program claimed by ws2812_init().
 */
void ws2812_deinit(void);

/**
 * @brief Set the LED's color, scaled by the current brightness (see ws2812_set_brightness()).
 *
 * Bytes are reordered internally to match the ws2812_order_t chosen at init time, so callers
 * always think in plain (r, g, b) regardless of what the LED itself expects on the wire. The
 * unscaled (r, g, b) is remembered, so a later ws2812_set_brightness() call re-applies it.
 *
 * Safe to call back-to-back with ws2812_set()/ws2812_set_brightness() with no delay: this
 * blocks as needed to keep the required reset gap between writes, so each one actually
 * latches instead of being absorbed into the previous write as if it were a second LED
 * further down a chain.
 *
 * @param r Red channel, 0-255.
 * @param g Green channel, 0-255.
 * @param b Blue channel, 0-255.
 */
void ws2812_set(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn the LED off. Shorthand for ws2812_set(0, 0, 0).
 */
void ws2812_off(void);

/**
 * @brief Change the brightness and immediately re-send the last color at the new level.
 *
 * @param brightness 0 (off) to 255 (full, i.e. the raw color passed to ws2812_set()).
 */
void ws2812_set_brightness(uint8_t brightness);

/**
 * @brief Get the brightness currently in effect (see ws2812_set_brightness()).
 */
uint8_t ws2812_get_brightness(void);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_WS2812_H
