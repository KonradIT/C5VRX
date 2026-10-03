/**
 * uart_link.c - the UART side of the video link; see uart_link.h.
 */

#include "uart_link.h"

#include <stdio.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "link_proto.h"

#define LINK_UART  UART_NUM_1
#define TX_BUFFER  16384        /* link_tx stages a whole frame; this only needs to keep the UART busy */
#define RX_BUFFER  1024

static uart_link_cmd_cb_t s_on_command;
static link_parser_t s_parser;

static size_t port_free(void *ctx)
{
    (void)ctx;
    size_t n = 0u;
    uart_get_tx_buffer_free_size(LINK_UART, &n);
    return n;
}

static void port_write(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    uart_write_bytes(LINK_UART, data, n);
}

static void on_packet(void *ctx, uint8_t type, uint8_t a, uint8_t b, const uint8_t *payload, int len)
{
    (void)ctx;
    (void)payload;
    (void)len;
    if (type == LINK_T_CMD && s_on_command) s_on_command(a, b);
}

/* Commands from the display. */
static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) link_parser_feed(&s_parser, buf, n, on_packet, NULL);
    }
}

esp_err_t uart_link_init(uart_link_cmd_cb_t on_command)
{
    s_on_command = on_command;
    link_parser_init(&s_parser);
    const uart_config_t cfg = {
        .baud_rate = CONFIG_C5VRX_LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LINK_UART, RX_BUFFER, TX_BUFFER, 0, NULL, 0);
    if (err != ESP_OK) return err;
    if ((err = uart_param_config(LINK_UART, &cfg)) != ESP_OK) return err;
    err = uart_set_pin(LINK_UART, CONFIG_C5VRX_LINK_UART_TX_GPIO, CONFIG_C5VRX_LINK_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    uint32_t baud = 0u;
    uart_get_baudrate(LINK_UART, &baud);
    if (xTaskCreate(rx_task, "link_rx", 3072, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    const link_tx_port_t port = { port_free, port_write, NULL };
    link_tx_init(&port, baud ? baud : (uint32_t)CONFIG_C5VRX_LINK_UART_BAUD);
    printf("[LINK] UART%d TX GPIO%d RX GPIO%d at %lu baud (protocol %u)\n", (int)LINK_UART,
           CONFIG_C5VRX_LINK_UART_TX_GPIO, CONFIG_C5VRX_LINK_UART_RX_GPIO, (unsigned long)baud,
           (unsigned)LINK_VERSION);
    return ESP_OK;
}

void uart_link_rx_stats(uint32_t *packets, uint32_t *crc_errors)
{
    *packets = s_parser.packets;
    *crc_errors = s_parser.crc_errors;
}
