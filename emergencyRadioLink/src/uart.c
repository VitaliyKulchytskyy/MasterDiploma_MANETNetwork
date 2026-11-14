#include "uart.h"

#include <string.h>

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>


static const uart_config_t k_uart_config[UART_COUNT] = {
    [UART_MAIN] = {
        .hw = CONFIG_UART_MAIN_HW,
        .baudrate = CONFIG_UART_MAIN_BAUDRATE,
        .tx_pin = CONFIG_UART_MAIN_TX,
        .rx_pin = CONFIG_UART_MAIN_RX,
        .data_bits = UART_DATA_BITS,
        .stop_bits = UART_STOP_BITS,
        .parity = UART_PARITY_NONE,
        .rx_use_dma = true,
        .tx_use_dma = true,
    },
    [UART_GPS] = {
        .hw = CONFIG_UART_GPS_HW,
        .baudrate = CONFIG_UART_GPS_BAUDRATE,
        .tx_pin = CONFIG_UART_GPS_TX,
        .rx_pin = CONFIG_UART_GPS_RX,
        .data_bits = UART_DATA_BITS,
        .stop_bits = UART_STOP_BITS,
        .parity = UART_PARITY_NONE,
        .rx_use_dma = true,
        .tx_use_dma = false,
    },
};

typedef struct {
    uint8_t rx_buffer[UART_RX_BUFFER_SIZE] __attribute__((aligned(UART_RX_BUFFER_SIZE)));
    uint8_t tx_buffer[UART_TX_BUFFER_SIZE];     // DMA source, used by tx_use_dma ports only
    uart_inst_t* hw;
    bool rx_use_dma;
    bool tx_use_dma;
    int tx_dma;             // DMA channel: tx_buffer -> UART (tx_use_dma only, else -1)
    int rx_dma;             // DMA channel: UART -> rx_buffer (ring) (rx_use_dma only, else -1)
    int rx_reload_dma;      // DMA channel: re-arms rx_dma when it completes (RP2040 has no self-trigger)
    uint32_t rx_reload;     // transfer count written by rx_reload_dma
    uint16_t head;          // written by the ISR (interrupt ports only)
    uint16_t tail;          // written by the reader
    uint32_t overruns;
} uart_port_t;

static uart_port_t g_ports[UART_COUNT];

static inline uint16_t ring_head(const uart_port_t* port) {
    if (port->rx_use_dma) {
        const uintptr_t write_addr = dma_channel_hw_addr(port->rx_dma)->write_addr;
        return (uint16_t)(write_addr - (uintptr_t)port->rx_buffer) & (UART_RX_BUFFER_SIZE - 1);
    }
    return __atomic_load_n(&port->head, __ATOMIC_ACQUIRE);
}

static inline size_t ring_count(const uart_port_t* port) {
    const uint16_t head = ring_head(port);
    const uint16_t tail = __atomic_load_n(&port->tail, __ATOMIC_ACQUIRE);
    return (uint16_t)(head - tail) & (UART_RX_BUFFER_SIZE - 1);
}

static void uart_rx_irq_handler(void) {
    for (int id = 0; id < UART_COUNT; ++id) {
        uart_port_t* port = &g_ports[id];
        if (port->hw == NULL || port->rx_use_dma)
            continue;

        while (uart_is_readable(port->hw)) {
            const uint8_t byte = uart_getc(port->hw);
            const uint16_t head = port->head;
            const uint16_t next = (head + 1) & (UART_RX_BUFFER_SIZE - 1);

            if (next == __atomic_load_n(&port->tail, __ATOMIC_ACQUIRE)) {
                port->overruns++;
                continue;
            }

            port->rx_buffer[head] = byte;
            __atomic_store_n(&port->head, next, __ATOMIC_RELEASE);
        }
    }
}

static void port_dma_release(uart_port_t* port) {
    int* channels[] = {&port->rx_reload_dma, &port->rx_dma, &port->tx_dma};

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
        if (*channels[i] >= 0) {
            dma_channel_abort(*channels[i]);
            dma_channel_unclaim(*channels[i]);
            *channels[i] = -1;
        }
    }
}

static bool port_dma_init(uart_port_t* port, uart_inst_t* hw, const bool rx_use_dma, const bool tx_use_dma) {
    port->tx_dma = tx_use_dma ? dma_claim_unused_channel(false) : -1;
    port->rx_dma = rx_use_dma ? dma_claim_unused_channel(false) : -1;
    port->rx_reload_dma = rx_use_dma ? dma_claim_unused_channel(false) : -1;

    if ((tx_use_dma && port->tx_dma < 0) || (rx_use_dma && (port->rx_dma < 0 || port->rx_reload_dma < 0))) {
        port_dma_release(port);
        return false;
    }

    uint32_t dmacr = 0;

    if (tx_use_dma) {
        dma_channel_config tx = dma_channel_get_default_config(port->tx_dma);
        channel_config_set_transfer_data_size(&tx, DMA_SIZE_8);
        channel_config_set_read_increment(&tx, true);
        channel_config_set_write_increment(&tx, false);
        channel_config_set_dreq(&tx, uart_get_dreq(hw, true));
        dma_channel_configure(port->tx_dma, &tx, &uart_get_hw(hw)->dr, port->tx_buffer, 0, false);
        dmacr |= UART_UARTDMACR_TXDMAE_BITS;
    }

    if (rx_use_dma) {
        dma_channel_config rx = dma_channel_get_default_config(port->rx_dma);
        channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
        channel_config_set_read_increment(&rx, false);
        channel_config_set_write_increment(&rx, true);
        channel_config_set_dreq(&rx, uart_get_dreq(hw, false));
        channel_config_set_ring(&rx, true, UART_RX_BUFFER_SIZE_LOG2);
        channel_config_set_chain_to(&rx, port->rx_reload_dma);
        dma_channel_configure(port->rx_dma, &rx, port->rx_buffer, &uart_get_hw(hw)->dr, UART_RX_BUFFER_SIZE, false);

        port->rx_reload = UART_RX_BUFFER_SIZE;
        dma_channel_config reload = dma_channel_get_default_config(port->rx_reload_dma);
        channel_config_set_transfer_data_size(&reload, DMA_SIZE_32);
        channel_config_set_read_increment(&reload, false);
        channel_config_set_write_increment(&reload, false);
        dma_channel_configure(port->rx_reload_dma, &reload, &dma_hw->ch[port->rx_dma].al1_transfer_count_trig,
                              &port->rx_reload, 1, false);

        dmacr |= UART_UARTDMACR_RXDMAE_BITS;
        dma_channel_start(port->rx_dma);
    }

    uart_get_hw(hw)->dmacr = dmacr;
    return true;
}

bool uart_port_init(const uart_id_t id, const uart_config_t* config) {
    if (id >= UART_COUNT || config == NULL || config->hw == NULL)
        return false;

    uart_port_t* port = &g_ports[id];
    port->hw = NULL;
    port->rx_use_dma = false;
    port->tx_use_dma = false;
    port_dma_release(port);
    port->head = port->tail = 0;
    port->overruns = 0;

    uart_init(config->hw, config->baudrate);
    gpio_set_function(config->tx_pin, GPIO_FUNC_UART);
    gpio_set_function(config->rx_pin, GPIO_FUNC_UART);
    uart_set_hw_flow(config->hw, false, false);
    uart_set_format(config->hw, config->data_bits, config->stop_bits, config->parity);
    uart_set_fifo_enabled(config->hw, true);

    if (config->rx_use_dma || config->tx_use_dma) {
        if (!port_dma_init(port, config->hw, config->rx_use_dma, config->tx_use_dma))
            return false;

        port->rx_use_dma = config->rx_use_dma;
        port->tx_use_dma = config->tx_use_dma;
    }

    if (!config->rx_use_dma) {
        const uint irq = UART_IRQ_NUM(config->hw);
        irq_set_exclusive_handler(irq, uart_rx_irq_handler);
        irq_set_enabled(irq, true);
        uart_set_irq_enables(config->hw, true, false);   // RX only
    }

    port->hw = config->hw;
    return true;
}

bool uart_init_all(void) {
    for (int id = 0; id < UART_COUNT; ++id) {
        g_ports[id].tx_dma = g_ports[id].rx_dma = g_ports[id].rx_reload_dma = -1;
    }

    bool ok = true;
    for (int id = 0; id < UART_COUNT; ++id) {
        ok &= uart_port_init((uart_id_t)id, &k_uart_config[id]);
    }
    return ok;
}

size_t uart_write(const uart_id_t id, const uint8_t* data, const size_t length) {
    if (id >= UART_COUNT || g_ports[id].hw == NULL || data == NULL)
        return 0;

    uart_port_t* port = &g_ports[id];
    if (!port->tx_use_dma) {
        uart_write_blocking(port->hw, data, length);
        return length;
    }

    size_t sent = 0;
    while (sent < length) {
        const size_t chunk = length - sent < UART_TX_BUFFER_SIZE ? length - sent : UART_TX_BUFFER_SIZE;

        dma_channel_wait_for_finish_blocking(port->tx_dma);     // previous chunk still reads tx_buffer
        memcpy(port->tx_buffer, data + sent, chunk);
        dma_channel_set_read_addr(port->tx_dma, port->tx_buffer, false);
        dma_channel_set_trans_count(port->tx_dma, chunk, true);
        sent += chunk;
    }

    return length;
}

bool uart_tx_busy(const uart_id_t id) {
    if (id >= UART_COUNT || g_ports[id].hw == NULL)
        return false;

    const uart_port_t* port = &g_ports[id];
    return (port->tx_use_dma && dma_channel_is_busy(port->tx_dma)) ||
           (uart_get_hw(port->hw)->fr & UART_UARTFR_BUSY_BITS);
}

size_t uart_available(const uart_id_t id) {
    if (id >= UART_COUNT || g_ports[id].hw == NULL)
        return 0;
    return ring_count(&g_ports[id]);
}

size_t uart_read(const uart_id_t id, uint8_t* out, const size_t max_length) {
    if (id >= UART_COUNT || g_ports[id].hw == NULL || out == NULL)
        return 0;

    uart_port_t* port = &g_ports[id];
    const size_t count = ring_count(port) < max_length ? ring_count(port) : max_length;
    uint16_t tail = port->tail;

    for (size_t i = 0; i < count; ++i) {
        out[i] = port->rx_buffer[tail];
        tail = (tail + 1) & (UART_RX_BUFFER_SIZE - 1);
    }

    __atomic_store_n(&port->tail, tail, __ATOMIC_RELEASE);
    return count;
}

void uart_flush_rx(const uart_id_t id) {
    if (id >= UART_COUNT || g_ports[id].hw == NULL)
        return;
    __atomic_store_n(&g_ports[id].tail, ring_head(&g_ports[id]), __ATOMIC_RELEASE);
}

uint32_t uart_overruns(const uart_id_t id) {
    return id < UART_COUNT ? g_ports[id].overruns : 0;
}
