#ifndef EMERGENCYRADIOLINK_UART_H
#define EMERGENCYRADIOLINK_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hardware/uart.h>

#define UART_RX_BUFFER_SIZE_LOG2    8
#define UART_RX_BUFFER_SIZE         (1u << UART_RX_BUFFER_SIZE_LOG2)
#define UART_TX_BUFFER_SIZE         256
#define UART_DATA_BITS              8
#define UART_STOP_BITS              1

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Logical UART ports of the board.
 *
 * Hardware, pins and baudrate of each port come from the CONFIG_UART_<NAME>_*
 * cache variables (see CMakePresets.json).
 */
typedef enum {
    UART_MAIN,      /**< Link to the main unit (MERL). */
    UART_GPS,       /**< GPS receiver. */
    UART_COUNT,     /**< Number of ports, not a valid port id. */
} uart_id_t;

/** @brief Configuration of a single UART port. */
typedef struct {
    uart_inst_t* hw;        /**< Hardware instance (uart0 / uart1). */
    uint32_t baudrate;      /**< Baudrate in bit/s. */
    uint8_t tx_pin;         /**< GPIO number of the TX pin. */
    uint8_t rx_pin;         /**< GPIO number of the RX pin. */
    uint8_t data_bits;      /**< Data bits per frame (5-8). */
    uint8_t stop_bits;      /**< Stop bits per frame (1 or 2). */
    uart_parity_t parity;   /**< Parity mode. */
    bool rx_use_dma;        /**< Use a DMA channel for RX instead of the RX interrupt. */
    bool tx_use_dma;        /**< Use a DMA channel for TX instead of blocking uart_write_blocking(). */
} uart_config_t;

/**
 * @brief Initialize every port from the board config table.
 *
 * Equivalent of calling MX_USARTx_Init() for each port in CubeMX.
 * Must be called once, from core 0, before any other uart_* function.
 *
 * @return true if all ports were initialized, false if any of them failed.
 */
bool uart_init_all(void);

/**
 * @brief Initialize a single port with a custom config.
 *
 * Sets up the pins, the frame format, the FIFO and the RX interrupt.
 *
 * @param id     Port to initialize.
 * @param config Port configuration, must not be NULL.
 * @return true on success, false if @p id is invalid or @p config is NULL / has no hardware.
 */
bool uart_port_init(uart_id_t id, const uart_config_t* config);

/**
 * @brief Transmit data.
 *
 * A port without tx_use_dma blocks until every byte is in the UART FIFO. A port with it
 * copies the data into an internal buffer and returns as soon as the last chunk has been
 * handed to the DMA, so @p data may be reused immediately; use uart_tx_busy() to know when
 * the line is idle. Only one writer at a time is supported.
 *
 * @param id     Port to write to.
 * @param data   Bytes to send.
 * @param length Number of bytes to send.
 * @return Number of bytes written (0 if the port is not initialized or @p data is NULL).
 */
size_t uart_write(uart_id_t id, const uint8_t* data, size_t length);

/**
 * @brief Check whether the port is still transmitting.
 *
 * @param id Port to query.
 * @return true while a TX DMA transfer (if tx_use_dma) or the UART shift register / FIFO is still busy.
 */
bool uart_tx_busy(uart_id_t id);

/**
 * @brief Read received data from the RX ring buffer without blocking.
 *
 * The buffer is filled from the RX interrupt, or by a DMA channel if rx_use_dma is set
 * (256 bytes, the oldest data is overwritten if it is not read in time). It is lock-free for a single
 * reader, so it may be read from either core (but not from both at once).
 *
 * @param id         Port to read from.
 * @param out        Destination buffer.
 * @param max_length Size of @p out in bytes.
 * @return Number of bytes copied into @p out (may be 0).
 */
size_t uart_read(uart_id_t id, uint8_t* out, size_t max_length);

/**
 * @brief Get the number of bytes waiting in the RX buffer.
 *
 * @param id Port to query.
 * @return Number of bytes available for uart_read().
 */
size_t uart_available(uart_id_t id);

/**
 * @brief Drop everything in the RX buffer.
 *
 * @param id Port to flush.
 */
void uart_flush_rx(uart_id_t id);

/**
 * @brief Get the number of received bytes lost because the RX buffer was full.
 *
 * Only counted when rx_use_dma is not set; a DMA-driven RX always reports 0 because the
 * DMA overwrites the oldest data without notice.
 *
 * @param id Port to query.
 * @return Number of dropped bytes since the port was initialized.
 */
uint32_t uart_overruns(uart_id_t id);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_UART_H
