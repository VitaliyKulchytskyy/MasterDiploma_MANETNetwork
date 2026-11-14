#ifndef EMERGENCYRADIOLINK_MERL_HANDLER_H
#define EMERGENCYRADIOLINK_MERL_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#include "merl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transmit a user message over ERL (provided by the application, it owns the radio queue).
 *
 * @param receiver ERL address of the receiver (a subnet broadcast address is allowed).
 * @param message  Message text, already validated (ends with \r\n, fits into an ERL packet).
 * @param length   Length of @p message in bytes.
 * @return true if the message was queued for transmission, false if it could not be (queue full).
 */
typedef bool (*merl_send_user_message_fn)(uint16_t receiver, const uint8_t* message, uint8_t length);

/**
 * @brief Reset the MERL receive state and set up the ERL user message transmit hook.
 *
 * Call once, after uart_init_all() and before merl_handler_service().
 *
 * @param send_user_message Hook used for MERL_CMD_USER_MESSAGE_SEND. If NULL, that command is
 *                          answered as unimplemented.
 */
void merl_handler_init(merl_send_user_message_fn send_user_message);

/**
 * @brief Process the MERL link on UART_MAIN.
 *
 * Reads the bytes received so far, parses request frames, executes the commands and
 * sends the answer back. A half-received frame is dropped after MERL_PROTOCOL_RX_TIMEOUT_MS.
 * Never blocks waiting for data, call it periodically. It must be called from a single
 * core only, because it is the only reader and writer of UART_MAIN.
 */
void merl_handler_service(void);

/**
 * @brief Forward a user message received over ERL to the main unit (chat).
 *
 * Sends a MERL_CMD_USER_MESSAGE_SEND request with address = receiver << 16 | sender, so the
 * main unit knows who wrote it and whether it was a broadcast. The main unit answers 0x0000,
 * which is consumed by merl_handler_service(). Must be called from the same core as
 * merl_handler_service(), because both write to UART_MAIN.
 *
 * @param sender   ERL address of the node that sent the message.
 * @param receiver ERL receiver of the packet (this node or the subnet broadcast address).
 * @param message  Message text as received, with or without the trailing \r\n.
 * @param length   Length of @p message in bytes.
 * @return true if the request was sent, false if the message does not fit.
 */
bool merl_handler_forward_user_message(uint16_t sender, uint16_t receiver, const uint8_t* message, uint8_t length);

/**
 * @brief Pass raw data received over ERL on to the main unit.
 *
 * Sends a MERL_CMD_USER_MESSAGE_PASSTHROUGH request. The main unit answers 0x0000, which
 * is consumed by merl_handler_service(). It must be called from the same core as
 * merl_handler_service(), because both write to UART_MAIN.
 *
 * @param message Message text as received.
 * @param length  Length of @p message in bytes (at most MERL_PROTOCOL_PAYLOAD_MAX_SIZE).
 * @return true if the request was sent, false if the arguments are invalid.
 */
bool merl_handler_passthrough_message(const uint8_t* message, uint8_t length);

/**
 * @brief Format a MERL frame as a human readable line, for debug output.
 *
 * The output looks like `(MERL RX)[>]: command=0x1001, len=4, payload={CF 01 FF E0 }`.
 * The payload is cut with "..." if it does not fit into @p out.
 *
 * @param frame    Complete frame as it is on the wire (big-endian fields), with a valid header.
 * @param tx       true if the frame is sent by this device, false if it was received.
 * @param out      Destination buffer, always NUL-terminated.
 * @param out_size Size of @p out in bytes.
 * @return Number of characters written, without the terminating NUL.
 */
uint16_t merl_handler_print(const uint8_t* frame, bool tx, char* out, uint16_t out_size);

#ifdef __cplusplus
}
#endif

#endif //EMERGENCYRADIOLINK_MERL_HANDLER_H
