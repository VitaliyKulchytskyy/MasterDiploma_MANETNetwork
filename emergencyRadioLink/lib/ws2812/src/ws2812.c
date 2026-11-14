#include "ws2812.h"

#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <pico/time.h>

#include "ws2812.pio.h"

// WS2812 only latches a new color after the line has been held low for a reset/idle gap;
// datasheets and clones vary from 50us to 280us. Two writes issued back-to-back with no gap
// (e.g. ws2812_set() immediately followed by ws2812_set_brightness()) would otherwise look
// like one continuous transmission to the LED, which per the protocol latches only the FIRST
// 24 bits it receives before a reset and forwards the rest to a (nonexistent) next LED in the
// chain - so the second write would be silently dropped. This margin comfortably covers a
// ~30us single-pixel transmit plus the worst documented reset time.
#define WS2812_MIN_WRITE_GAP_US 400

static PIO g_pio;
static uint g_sm;
static uint g_offset;
static ws2812_order_t g_order;
static bool g_initialized;
static uint8_t g_brightness;   // 0-255, applied to g_color_r/g/b before every write
static uint8_t g_color_r, g_color_g, g_color_b;   // last color set, unscaled
static uint64_t g_last_write_us;   // to_us_since_boot() at the start of the last push, 0 if none yet

static uint8_t scale8(const uint8_t value, const uint8_t brightness) {
    return (uint8_t)(((uint16_t)value * brightness) / 255);
}

// Reorders (r, g, b) into the wire order this LED expects, top-aligned in a 32-bit word: the
// state machine shifts left and autopulls every 24 bits, so only bits 31:8 are ever driven
// onto the wire (see sm_config_set_out_shift(..., 24) in ws2812_init()).
static uint32_t pack_pixel(const uint8_t r, const uint8_t g, const uint8_t b) {
    uint8_t first, second, third;   // wire order: first byte shifted out, then second, then third

    switch (g_order) {
        case WS2812_ORDER_RGB: first = r; second = g; third = b; break;
        case WS2812_ORDER_RBG: first = r; second = b; third = g; break;
        case WS2812_ORDER_GBR: first = g; second = b; third = r; break;
        case WS2812_ORDER_BRG: first = b; second = r; third = g; break;
        case WS2812_ORDER_BGR: first = b; second = g; third = r; break;
        case WS2812_ORDER_GRB:
        default:               first = g; second = r; third = b; break;
    }

    return ((uint32_t)first << 24) | ((uint32_t)second << 16) | ((uint32_t)third << 8);
}

bool ws2812_init(const ws2812_config_t* config) {
    if (config == NULL)
        return false;

    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&ws2812_program, &g_pio, &g_sm, &g_offset,
                                                           config->pin, 1, true)) {
        return false;
    }

    g_order = config->order;
    g_brightness = config->brightness;
    g_color_r = g_color_g = g_color_b = 0;
    g_last_write_us = 0;

    const float freq = config->frequency_hz > 0.0f ? config->frequency_hz : 800000.0f;
    const float cycles_per_bit = (float)(ws2812_T1 + ws2812_T2 + ws2812_T3);

    pio_sm_config sm_config = ws2812_program_get_default_config(g_offset);
    sm_config_set_sideset_pins(&sm_config, config->pin);
    sm_config_set_out_shift(&sm_config, false, true, 24);
    sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&sm_config, (float)clock_get_hz(clk_sys) / (freq * cycles_per_bit));

    pio_gpio_init(g_pio, config->pin);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm, config->pin, 1, true);
    pio_sm_init(g_pio, g_sm, g_offset, &sm_config);
    pio_sm_set_enabled(g_pio, g_sm, true);

    g_initialized = true;
    return true;
}

void ws2812_deinit(void) {
    if (!g_initialized)
        return;

    pio_remove_program_and_unclaim_sm(&ws2812_program, g_pio, g_sm, g_offset);
    g_initialized = false;
}

static void push_current_color(void) {
    if (!g_initialized)
        return;

    // enforce the reset/latch gap since the previous write, so this one is seen as its own
    // fresh frame rather than a continuation of the last one (see WS2812_MIN_WRITE_GAP_US)
    const uint64_t now_us = to_us_since_boot(get_absolute_time());
    const uint64_t elapsed_us = now_us - g_last_write_us;
    if (g_last_write_us != 0 && elapsed_us < WS2812_MIN_WRITE_GAP_US) {
        sleep_us(WS2812_MIN_WRITE_GAP_US - elapsed_us);
    }

    pio_sm_put_blocking(g_pio, g_sm, pack_pixel(scale8(g_color_r, g_brightness),
                                                scale8(g_color_g, g_brightness),
                                                scale8(g_color_b, g_brightness)));
    g_last_write_us = to_us_since_boot(get_absolute_time());
}

void ws2812_set(const uint8_t r, const uint8_t g, const uint8_t b) {
    g_color_r = r;
    g_color_g = g;
    g_color_b = b;
    push_current_color();
}

void ws2812_off(void) {
    ws2812_set(0, 0, 0);
}

void ws2812_set_brightness(const uint8_t brightness) {
    g_brightness = brightness;
    push_current_color();
}

uint8_t ws2812_get_brightness(void) {
    return g_brightness;
}
