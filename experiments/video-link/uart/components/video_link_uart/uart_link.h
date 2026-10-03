#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "link_tx.h"

/**
 * uart_link.c - the UART side of the video link:
 * the ESP-IDF UART driver as the byte port of link_tx.c, and the receiver of
 * commands from the display board. Protocol: link_proto.h.
 */

typedef void (*uart_link_cmd_cb_t)(uint8_t cmd, uint8_t arg);

/** Install the UART driver, start the command receiver task (commands go to
 * on_command, from that task) and hand the port to link_tx. */
esp_err_t uart_link_init(uart_link_cmd_cb_t on_command);

/** Link statistics: packets received from the display and their errors. */
void uart_link_rx_stats(uint32_t *packets, uint32_t *crc_errors);
