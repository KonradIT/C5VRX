/**
 * main.cpp - LilyGo T-Embed CC1101: display and remote control for the C5VRX
 * UART video link.
 *
 * The XIAO ESP32-C5 running C5VRX link mode grabs 224x168 luma frames from
 * the analog FPV signal and streams them over UART (protocol version 2,
 * ../protocol/link_proto.h, shared with the C5 firmware: packets with CRC-16,
 * rows coded near-losslessly). This board parses and decodes them, shows
 * the frame on the 320x170 ST7789 with the link status in the left margin,
 * and sends commands back.
 *
 * Wiring: XIAO D6 (GPIO11, TX) -> T-Embed RXD (GPIO44)
 *         XIAO D7 (GPIO12, RX) <- T-Embed TXD (GPIO43)
 *         GND - GND (leave 3V3 unconnected; both boards on USB)
 *
 * Controls:
 *   encoder turn        previous / next channel (bands R A B E F L)
 *   encoder press       rotate the picture by 180 degrees
 *   encoder hold 0.7 s  start the channel scan (locks onto the first channel
 *                       with live video, "SCAN MODE" on screen, a beep on
 *                       lock); hold again to stop and stay on the channel
 *   BACK press          picture mode: AUTO (fastest) / FINE / RAW
 *   BACK hold 0.7 s     horizontal smoothing on / off
 *
 * The UART follows the C5's baud rate (4 or 5 Mbaud): with bytes arriving
 * but no valid packet for 1.5 s it switches to the other one.
 */

#include <Arduino.h>

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/semphr.h"
#include "soc/gpio_reg.h"

#include "font3x5.h"
#include "link_proto.h"

/* ---------------------------------------------------------------- board */
#define PIN_PWR_EN    15    /* board power latch: must be driven high early */
#define PIN_LCD_BL    21
#define PIN_LCD_RST   40    /* display reset; also the speaker amplifier's LRCLK */
#define PIN_LCD_CS    41
#define PIN_LCD_DC    16
#define PIN_SPI_SCLK  11
#define PIN_SPI_MOSI  9
#define PIN_SPI_MISO  10
#define PIN_CC1101_CS 12    /* shares the SPI bus: keep deselected */
#define PIN_SD_CS     13
#define PIN_ENC_A     4
#define PIN_ENC_B     5
#define PIN_ENC_KEY   0
#define PIN_BACK_KEY  6
#define PIN_SPK_BCLK  46    /* MAX98357A */
#define PIN_SPK_LRCLK 40
#define PIN_SPK_DIN   7
#define PIN_LINK_RX   44    /* UART connector RXD */
#define PIN_LINK_TX   43    /* UART connector TXD */

#define LCD_W 320
#define LCD_H 170
#define LCD_SPI_HZ (40 * 1000 * 1000)

/* Encoder: quadrature counts per detent (LilyGo drives it as TWO03). Flip
 * ENC_DIR if turning clockwise steps backwards. */
#define ENC_COUNTS_PER_DETENT 2
#define ENC_DIR       1
#define LONG_PRESS_MS 700
#define DEBOUNCE_MS   20

/* ---------------------------------------------------------------- link */
#define LINK_UART   UART_NUM_1
#define IMG_W       LINK_W
#define IMG_H       LINK_H
#define IMG_X       68      /* the status column is 0 .. 67 */
#define IMG_Y       1
#define BEEP        1       /* 0: no sound on scan lock */
#define AUTO_LEVELS 1       /* 0: show the receiver's levels as they come */
#define SERIAL_STATUS 0     /* 1: link statistics on the USB serial port once a second */

static const uint32_t k_bauds[2] = { 4000000u, 5000000u };

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_done;
static uint16_t *s_fb;              /* IMG_W x IMG_H, RGB565 byte-swapped for SPI */
static uint16_t s_gray[256];       /* grey level -> display colour (auto levels applied) */

/* Auto levels. The receiver scales pixels from the sync and blanking levels
 * it measures, assuming standard white; real cameras and transmitters
 * differ (on the bench the OSD stopped reaching white). Every frame's
 * histogram sets a black point (darkest 1 %) and a white point (brightest
 * 0.5 %, usually the OSD), followed slowly, the stretch capped at 1.6x. */
static uint16_t s_hist[256];
static uint32_t s_hist_n;
static float    s_black = 0.0f, s_white = 255.0f;
static int      s_lut_black = -1, s_lut_white = -1;

static void gray_lut(int black, int white)
{
    for (int v = 0; v < 256; ++v) {
        int o = (v - black) * 255 / (white - black);
        o = o < 0 ? 0 : o > 255 ? 255 : o;
        uint8_t g = (uint8_t)o;
        uint16_t c = (uint16_t)(((g & 0xf8) << 8) | ((g & 0xfc) << 3) | (g >> 3));
        s_gray[v] = (uint16_t)((c >> 8) | (c << 8));
    }
    s_lut_black = black;
    s_lut_white = white;
}

/* At the end of a frame: move the levels towards this frame's, rebuild the
 * table when they moved by a grey level. */
static void levels_update(void)
{
#if AUTO_LEVELS
    if (s_hist_n < 2000) return;
    uint32_t lo = s_hist_n / 100, hi = s_hist_n - s_hist_n / 200, acc = 0;
    int black = 0, white = 255;
    for (int v = 0; v < 256; ++v) {
        acc += s_hist[v];
        if (acc <= lo) black = v;
        if (acc <= hi) white = v;
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_n = 0;
    if (black > 48) black = 48;                    /* never crush a dark scene */
    if (white < 160) white = 160;                  /* never blow up a flat, noisy one */
    s_black += 0.15f * ((float)black - s_black);
    s_white += 0.15f * ((float)white - s_white);
    int b = (int)(s_black + 0.5f), w = (int)(s_white + 0.5f);
    if (w - b < 160) b = w - 160;                  /* stretch at most 255/160 = 1.6x */
    if (b < 0) b = 0;
    if (b != s_lut_black || w != s_lut_white) gray_lut(b, w);
#endif
}
static bool s_flipped;
static link_parser_t s_parser;
static int s_baud_idx;

/* What the receiver last reported. */
static uint8_t s_info[LINK_INFO_LEN];
static bool s_have_info;
static uint32_t s_info_ms;          /* time of the last INFO */
static int s_frame = -1;            /* frame id whose rows are in s_fb */
static int s_rows_this_frame;
static int s_last_locks = -1;

/* Local state. */
static bool s_scan_wanted;          /* long press: scanning requested, until the receiver says otherwise */
static uint32_t s_scan_wanted_ms;
static char s_toast[20];            /* short message over the picture */
static uint32_t s_toast_until;
static uint16_t s_toast_color;
static volatile bool s_beeping;
static volatile bool s_beep_done;

/* Statistics. */
static uint32_t s_frames_drawn, s_rows, s_bad_rows, s_bytes, s_last_byte_ms, s_last_good_ms;

/* Milliseconds since t. Signed: t can be a little later than a 'now' taken
 * before a packet was parsed (drawing a frame takes 15 ms), and an unsigned
 * difference would then read as 49 days: that switched the baud rate away
 * from a working link on the bench. */
static inline int32_t age_ms(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t);
}

/* ---------------------------------------------------------------- colours */

static inline uint16_t rgb565_swapped(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

#define C_BLACK  rgb565_swapped(0, 0, 0)
#define C_WHITE  rgb565_swapped(255, 255, 255)
#define C_DIM    rgb565_swapped(140, 140, 140)
#define C_GREEN  rgb565_swapped(40, 220, 60)
#define C_RED    rgb565_swapped(230, 50, 40)
#define C_YELLOW rgb565_swapped(255, 210, 0)
#define C_CYAN   rgb565_swapped(60, 200, 255)

/* ---------------------------------------------------------------- display */

static bool IRAM_ATTR lcd_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_done, &woken);
    return woken == pdTRUE;
}

static void lcd_draw(int x0, int y0, int w, int h, const uint16_t *pixels)
{
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + h, pixels);
    xSemaphoreTake(s_lcd_done, pdMS_TO_TICKS(100));   /* buffer must stay valid until sent */
}

static void lcd_orientation(void)
{
    /* Landscape as TFT_eSPI rotation 3 (MADCTL MV | MY), the orientation the
     * 1101view project uses on this board; flipped = rotation 1. */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, s_flipped, !s_flipped);
}

static void lcd_panel_start(void)
{
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, 0, 35);     /* 170-line panel in the 240-line RAM */
    lcd_orientation();
    esp_lcd_panel_disp_on_off(s_panel, true);
}

static void lcd_init(void)
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_SPI_MOSI;
    bus.miso_io_num = PIN_SPI_MISO;
    bus.sclk_io_num = PIN_SPI_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = IMG_W * IMG_H * 2 + 64;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.dc_gpio_num = PIN_LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = LCD_SPI_HZ;
    io_cfg.trans_queue_depth = 4;
    io_cfg.on_color_trans_done = lcd_done_cb;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = PIN_LCD_RST;
    dev.color_space = ESP_LCD_COLOR_SPACE_RGB;
    dev.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev, &s_panel));
    lcd_panel_start();
}

static void lcd_fill(int x0, int y0, int w, int h, uint16_t color)
{
    static uint16_t line[LCD_W];
    for (int i = 0; i < w; ++i) line[i] = color;
    for (int y = y0; y < y0 + h; ++y) lcd_draw(x0, y, w, 1, line);
}

/* 3x5 glyphs at scale s into an RGB565 buffer (stride in pixels). */
static void text_to(uint16_t *buf, int stride, int bw, int bh, int x0, int y0, const char *text, int s, uint16_t fg)
{
    for (int c = 0; text[c]; ++c) {
        uint16_t g = font3x5_glyph(text[c]);
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if (g & (1u << (14 - row * 3 - col)))
                    for (int dy = 0; dy < s; ++dy)
                        for (int dx = 0; dx < s; ++dx) {
                            int x = x0 + c * 4 * s + col * s + dx, y = y0 + row * s + dy;
                            if (x >= 0 && x < bw && y >= 0 && y < bh) buf[y * stride + x] = fg;
                        }
    }
}

/* One line of text straight to the display: maxc cells of background. */
static void lcd_text(int x0, int y0, const char *text, int s, uint16_t fg, uint16_t bg, int maxc)
{
    static uint16_t buf[LCD_W * 5 * 4];
    int w = maxc * 4 * s, h = 5 * s;
    if (w > LCD_W) w = LCD_W;
    for (int i = 0; i < w * h; ++i) buf[i] = bg;
    text_to(buf, w, w, h, 0, 0, text, s, fg);
    lcd_draw(x0, y0, w, h, buf);
}

/* ---------------------------------------------------------------- sound */

#if BEEP
/* The MAX98357A's LRCLK is the display's reset line, so a beep resets the
 * display: the beep runs in its own task while the main loop keeps reading
 * the link but draws nothing, and the display is started again afterwards. */
static void beep_task(void *arg)
{
    (void)arg;
    const int rate = 16000;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = rate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.tx_desc_auto_clear = true;
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) == ESP_OK) {
        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = PIN_SPK_BCLK;
        pins.ws_io_num = PIN_SPK_LRCLK;
        pins.data_out_num = PIN_SPK_DIN;
        pins.data_in_num = I2S_PIN_NO_CHANGE;
        i2s_set_pin(I2S_NUM_0, &pins);
        /* Two rising tones, 70 ms each, soft edges. */
        static int16_t buf[2 * 256];
        const int tones[2] = { 1400, 2100 };
        for (int t = 0; t < 2; ++t) {
            const int n = rate * 70 / 1000;
            for (int i = 0; i < n; i += 256) {
                int m = n - i < 256 ? n - i : 256;
                for (int k = 0; k < m; ++k) {
                    int j = i + k;
                    float env = j < 160 ? j / 160.0f : (n - j) < 160 ? (n - j) / 160.0f : 1.0f;
                    int16_t v = (int16_t)(9000.0f * env * sinf(6.2831853f * tones[t] * j / rate));
                    buf[2 * k] = v;
                    buf[2 * k + 1] = v;
                }
                size_t written = 0;
                i2s_write(I2S_NUM_0, buf, (size_t)m * 4u, &written, pdMS_TO_TICKS(200));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));                 /* let the DMA play out */
        i2s_driver_uninstall(I2S_NUM_0);
    }
    /* Give GPIO40 back to the display as a plain output. */
    gpio_reset_pin((gpio_num_t)PIN_LCD_RST);
    gpio_set_direction((gpio_num_t)PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_LCD_RST, 1);
    s_beep_done = true;
    vTaskDelete(NULL);
}
#endif

static void beep(void)
{
#if BEEP
    if (s_beeping) return;
    s_beeping = true;
    s_beep_done = false;
    xTaskCreatePinnedToCore(beep_task, "beep", 4096, NULL, 3, NULL, 0);
#endif
}

/* ---------------------------------------------------------------- input */

static volatile int32_t s_enc_count;
static volatile uint8_t s_enc_state;

static void IRAM_ATTR enc_isr(void)
{
    static const int8_t k_step[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint8_t ab = (uint8_t)((((in >> PIN_ENC_A) & 1u) << 1) | ((in >> PIN_ENC_B) & 1u));
    s_enc_state = (uint8_t)(((s_enc_state << 2) | ab) & 0x0fu);
    s_enc_count += k_step[s_enc_state];
}

typedef struct {
    int pin;
    bool raw, down, long_fired;
    uint32_t t_change, t_down;
} button_t;

typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG } button_event_t;

static button_event_t button_poll(button_t *b, uint32_t now)
{
    bool raw = digitalRead(b->pin) == LOW;
    if (raw != b->raw) {
        b->raw = raw;
        b->t_change = now;
    }
    if (now - b->t_change >= DEBOUNCE_MS && raw != b->down) {
        b->down = raw;
        if (raw) {
            b->t_down = now;
            b->long_fired = false;
        } else if (!b->long_fired && now - b->t_down < LONG_PRESS_MS) {
            return BTN_SHORT;
        }
    }
    if (b->down && !b->long_fired && now - b->t_down >= LONG_PRESS_MS) {
        b->long_fired = true;
        return BTN_LONG;
    }
    return BTN_NONE;
}

static void send_command(uint8_t cmd, uint8_t arg)
{
    uint8_t pkt[LINK_OVERHEAD];
    int n = link_packet(pkt, LINK_T_CMD, cmd, arg, NULL, 0);
    uart_write_bytes(LINK_UART, (const char *)pkt, (size_t)n);
}

static void toast(const char *text, uint16_t color, uint32_t ms)
{
    snprintf(s_toast, sizeof(s_toast), "%s", text);
    s_toast_color = color;
    s_toast_until = millis() + ms;
}

/* ---------------------------------------------------------------- link */

static void link_init(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate = (int)k_bauds[s_baud_idx];
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 65536, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, PIN_LINK_TX, PIN_LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_timeout(LINK_UART, 2);
    link_parser_init(&s_parser);
}

static bool scanning(void)
{
    if (s_scan_wanted) return true;
    return s_have_info && (s_info[LINK_INFO_FLAGS] & LINK_F_SCANNING);
}

static uint8_t picture_mode(void)
{
    return s_have_info ? (uint8_t)((s_info[LINK_INFO_FLAGS] >> LINK_F_MODE_SHIFT) & 3u) : LINK_MODE_AUTO;
}

static void channel_text(char *out, size_t n)
{
    char name[3];
    link_channel_name(s_have_info ? s_info[LINK_INFO_CHANNEL] : 0xffu, name);
    unsigned mhz = s_have_info ? (unsigned)(s_info[LINK_INFO_MHZ] | (s_info[LINK_INFO_MHZ + 1] << 8)) : 0u;
    snprintf(out, n, "%s %u", name, mhz);
}

/* The toast, drawn into the frame so that it does not flicker. */
static void overlay_toast(void)
{
    if (!s_toast[0] || (int32_t)(millis() - s_toast_until) > 0) {
        s_toast[0] = '\0';
        return;
    }
    int w = (int)strlen(s_toast) * 8 + 8;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < w && x < IMG_W; ++x) s_fb[(IMG_H - 18 + y) * IMG_W + x] = C_BLACK;
    text_to(s_fb + (IMG_H - 18) * IMG_W, IMG_W, IMG_W, 16, 4, 3, s_toast, 2, s_toast_color);
}

static void draw_frame(void)
{
    if (s_beeping) return;
    overlay_toast();
    lcd_draw(IMG_X, IMG_Y, IMG_W, IMG_H, s_fb);
    s_frames_drawn++;
}

/* SCAN MODE banner on the picture while scanning (no frames arrive then). */
static void draw_scan_banner(void)
{
    if (s_beeping) return;
    static uint16_t buf[IMG_W * 22];
    for (int i = 0; i < IMG_W * 22; ++i) buf[i] = C_BLACK;
    char ch[16], line[32];
    channel_text(ch, sizeof(ch));
    text_to(buf, IMG_W, IMG_W, 22, 6, 1, "SCAN MODE", 2, C_YELLOW);
    snprintf(line, sizeof(line), "%s", ch);
    text_to(buf, IMG_W, IMG_W, 22, 6 + 10 * 8 + 8, 1, line, 2, C_WHITE);
    for (int x = 0; x < IMG_W; ++x) buf[21 * IMG_W + x] = C_YELLOW;
    lcd_draw(IMG_X, IMG_Y, IMG_W, 22, buf);
}

static void on_packet(void *ctx, uint8_t type, uint8_t a, uint8_t b, const uint8_t *p, int len)
{
    (void)ctx;
    s_last_good_ms = millis();
    if (type == LINK_T_ROW) {
        if (b >= IMG_H) return;
        if (s_frame >= 0 && a != (uint8_t)s_frame && s_rows_this_frame > 0) {
            draw_frame();                           /* the INFO of the last frame was lost */
            s_rows_this_frame = 0;
        }
        s_frame = a;
        static uint8_t px[LINK_W];
        if (!link_row_decode(p, len, px)) {
            s_bad_rows++;
            return;
        }
        uint16_t *dst = s_fb + (size_t)b * IMG_W;
        for (int x = 0; x < IMG_W; ++x) dst[x] = s_gray[px[x]];
        for (int x = 1; x < IMG_W; x += 4) s_hist[px[x]]++;
        s_hist_n += IMG_W / 4;
        s_rows++;
        s_rows_this_frame++;
        return;
    }
    if (type != LINK_T_INFO || len < LINK_INFO_LEN) return;
    memcpy(s_info, p, LINK_INFO_LEN);
    s_have_info = true;
    s_info_ms = millis();
    bool scan = s_info[LINK_INFO_FLAGS] & LINK_F_SCANNING;
    if (scan || age_ms(millis(), s_scan_wanted_ms) > 1500) s_scan_wanted = false;   /* the receiver has answered */

    int locks = s_info[LINK_INFO_LOCKS];
    if (s_last_locks >= 0 && locks == ((s_last_locks + 1) & 0xff)) {    /* not a receiver reboot (back to 0) */
        char ch[16], msg[20];
        channel_text(ch, sizeof(ch));
        snprintf(msg, sizeof(msg), "LOCK %s", ch);
        toast(msg, C_GREEN, 2500);
        beep();
    }
    s_last_locks = locks;

    if (s_info[LINK_INFO_ROWS] > 0 && s_rows_this_frame > 0) {
        draw_frame();
        levels_update();                            /* for the next frame's rows */
    }
    else if (scan) draw_scan_banner();
    s_rows_this_frame = 0;
    s_frame = -1;
}

/* ---------------------------------------------------------------- status */

static const char *error_text(uint8_t e)
{
    switch (e) {
    case LINK_E_NONE: return "OK";
    case LINK_E_NO_HSYNC: return "NO SYNC";
    case LINK_E_NO_VSYNC: return "NO VSYNC";
    case LINK_E_LOST: return "LOST";
    case LINK_E_TIMEOUT: return "TIMEOUT";
    case LINK_E_LOST_VSYNC: return "V LOST";
    default: return "ERROR";
    }
}

static void draw_status(float fps, uint32_t link_errors)
{
    if (s_beeping) return;
    char t[16];
    uint32_t now = millis();
    bool linked = age_ms(now, s_last_good_ms) < 700;
    bool video = s_have_info && s_info[LINK_INFO_ROWS] > 0 && age_ms(now, s_info_ms) < 700;
    if (s_have_info && s_info[LINK_INFO_VERSION] != LINK_VERSION)
        lcd_text(2, 4, "C5 FW?", 2, C_RED, C_BLACK, 8);
    else if (scanning())
        lcd_text(2, 4, "SCAN", 2, C_YELLOW, C_BLACK, 8);
    else
        lcd_text(2, 4, linked ? (video ? "VIDEO" : "NO SIG") : "NO LINK", 2, video ? C_GREEN : C_RED, C_BLACK, 8);

    char name[3];
    link_channel_name(s_have_info ? s_info[LINK_INFO_CHANNEL] : 0xffu, name);
    lcd_text(2, 20, name, 3, C_WHITE, C_BLACK, 5);
    snprintf(t, sizeof(t), "%u", s_have_info ? (unsigned)(s_info[LINK_INFO_MHZ] | (s_info[LINK_INFO_MHZ + 1] << 8)) : 0u);
    lcd_text(2, 38, t, 2, C_WHITE, C_BLACK, 8);
    lcd_text(2, 50, "MHZ", 2, C_DIM, C_BLACK, 8);
    lcd_text(2, 64, s_have_info ? ((s_info[LINK_INFO_FLAGS] & LINK_F_PAL) ? "PAL" : "NTSC") : "", 2, C_WHITE, C_BLACK, 8);

    snprintf(t, sizeof(t), "%.1f", fps);
    lcd_text(2, 80, t, 2, C_WHITE, C_BLACK, 8);
    lcd_text(2, 92, "FPS", 2, C_DIM, C_BLACK, 8);

    uint8_t mode = picture_mode();
    uint8_t d = s_have_info ? s_info[LINK_INFO_DELTA] : 0xffu;
    if (mode == LINK_MODE_RAW) snprintf(t, sizeof(t), "RAW");
    else snprintf(t, sizeof(t), "%s %u", mode == LINK_MODE_AUTO ? "AUTO" : "FINE", d == 0xffu ? 0u : (unsigned)d);
    lcd_text(2, 108, t, 2, C_CYAN, C_BLACK, 8);

    snprintf(t, sizeof(t), "G%u%s", s_have_info ? (unsigned)s_info[LINK_INFO_GAIN] : 0u,
             s_have_info && !(s_info[LINK_INFO_FLAGS] & LINK_F_SMOOTHING) ? " S0" : "");
    lcd_text(2, 124, t, 2, C_WHITE, C_BLACK, 8);
    uint8_t e = s_have_info ? s_info[LINK_INFO_ERROR] : 0u;
    lcd_text(2, 138, s_have_info ? error_text(e) : "", 2, e ? C_RED : C_DIM, C_BLACK, 8);
    snprintf(t, sizeof(t), "E%lu", (unsigned long)(link_errors > 999 ? 999 : link_errors));
    lcd_text(2, 154, t, 2, link_errors ? C_RED : C_DIM, C_BLACK, 8);
}

static void redraw_all(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK);
    lcd_draw(IMG_X, IMG_Y, IMG_W, IMG_H, s_fb);
}

/* ---------------------------------------------------------------- main */

void setup(void)
{
    Serial.begin(115200);
    pinMode(PIN_PWR_EN, OUTPUT);
    digitalWrite(PIN_PWR_EN, HIGH);
    pinMode(PIN_CC1101_CS, OUTPUT);
    digitalWrite(PIN_CC1101_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    pinMode(PIN_ENC_KEY, INPUT_PULLUP);
    pinMode(PIN_BACK_KEY, INPUT_PULLUP);
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    delay(50);

    s_lcd_done = xSemaphoreCreateBinary();
    s_fb = (uint16_t *)heap_caps_malloc(IMG_W * IMG_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    gray_lut(0, 255);
    for (int i = 0; i < IMG_W * IMG_H; ++i) s_fb[i] = s_gray[(i / IMG_W) * 255 / IMG_H];   /* test ramp */
    text_to(s_fb, IMG_W, IMG_W, IMG_H, 36, 76, "C5VRX LINK", 4, C_WHITE);

    lcd_init();
    redraw_all();
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    s_enc_state = (uint8_t)(((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B)) & 3);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), enc_isr, CHANGE);

    link_init();
    Serial.printf("C5VRX T-Embed link (protocol %u): UART%d RX GPIO%d TX GPIO%d at %lu baud, image %dx%d at %d,%d\n",
                  (unsigned)LINK_VERSION, (int)LINK_UART, PIN_LINK_RX, PIN_LINK_TX, (unsigned long)k_bauds[s_baud_idx],
                  IMG_W, IMG_H, IMG_X, IMG_Y);
}

void loop(void)
{
    static uint8_t buf[4096];
    static uint32_t t_status, frames_at_status, bytes_at_status, crc_at_status, bad_at_status;
    static button_t enc_key = { PIN_ENC_KEY, false, false, false, 0, 0 };
    static button_t back_key = { PIN_BACK_KEY, false, false, false, 0, 0 };
    static int32_t enc_used;
    static uint32_t t_step;
    static int pending_steps;

    int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(5));
    if (n > 0) {
        s_bytes += (uint32_t)n;
        s_last_byte_ms = millis();
        link_parser_feed(&s_parser, buf, n, on_packet, NULL);
    }
    uint32_t now = millis();                        /* after parsing: packets may have drawn a frame */

    /* Display back after a beep. */
    if (s_beep_done) {
        s_beep_done = false;
        lcd_panel_start();
        s_beeping = false;
        redraw_all();
        t_status = 0;                               /* status column now */
    }

    /* Encoder: whole detents become channel steps, sent at most every 60 ms. */
    int32_t count = s_enc_count;
    int32_t detents = (count - enc_used) / ENC_COUNTS_PER_DETENT;
    if (detents != 0) {
        enc_used += detents * ENC_COUNTS_PER_DETENT;
        pending_steps += ENC_DIR * (int)detents;
        s_scan_wanted = false;
    }
    if (pending_steps != 0 && now - t_step >= 60) {
        int step = pending_steps > 8 ? 8 : pending_steps < -8 ? -8 : pending_steps;
        send_command(LINK_CMD_CHANNEL_STEP, (uint8_t)(int8_t)step);
        pending_steps -= step;
        t_step = now;
    }

    switch (button_poll(&enc_key, now)) {
    case BTN_SHORT:
        s_flipped = !s_flipped;
        if (!s_beeping) {
            lcd_orientation();
            redraw_all();
            t_status = 0;
        }
        break;
    case BTN_LONG:
        if (scanning()) {
            send_command(LINK_CMD_SCAN_STOP, 0);
            s_scan_wanted = false;
            toast("SCAN OFF", C_YELLOW, 1500);
        } else {
            send_command(LINK_CMD_SCAN_START, 0);
            s_scan_wanted = true;
            s_scan_wanted_ms = now;
            draw_scan_banner();
        }
        t_status = 0;
        break;
    default:
        break;
    }
    switch (button_poll(&back_key, now)) {
    case BTN_SHORT: {
        uint8_t mode = (uint8_t)((picture_mode() + 1u) % LINK_MODE_COUNT);
        send_command(LINK_CMD_MODE, mode);
        toast(mode == LINK_MODE_AUTO ? "AUTO: FASTEST" : mode == LINK_MODE_FINE ? "FINE: BEST" : "RAW: 8 BIT", C_CYAN, 1500);
        break;
    }
    case BTN_LONG: {
        bool on = !(s_have_info && (s_info[LINK_INFO_FLAGS] & LINK_F_SMOOTHING));
        send_command(LINK_CMD_SMOOTHING, on ? 1 : 0);
        toast(on ? "SMOOTHING ON" : "SMOOTHING OFF", C_CYAN, 1500);
        break;
    }
    default:
        break;
    }

    /* Follow the receiver's baud rate: bytes keep coming, packets do not. */
    static uint32_t t_baud;
    if (age_ms(now, s_last_byte_ms) < 300 && age_ms(now, s_last_good_ms) > 1500 && age_ms(now, t_baud) > 1500) {
        s_baud_idx ^= 1;
        uart_set_baudrate(LINK_UART, k_bauds[s_baud_idx]);
        link_parser_init(&s_parser);
        s_last_good_ms = now;
        t_baud = now;
#if SERIAL_STATUS
        Serial.printf("no valid packets: switching to %lu baud\n", (unsigned long)k_bauds[s_baud_idx]);
#endif
    }

    if (now - t_status >= 1000) {
        float dt = t_status ? (now - t_status) / 1000.0f : 1.0f;
        float fps = (s_frames_drawn - frames_at_status) / dt;
        float kbps = (s_bytes - bytes_at_status) / dt / 1024.0f;
        uint32_t errs = (s_parser.crc_errors - crc_at_status) + (s_bad_rows - bad_at_status);
        draw_status(fps, errs);
#if SERIAL_STATUS
        Serial.printf("fps %.1f  link %.0f KB/s  rows %lu  crc errors %lu  bad rows %lu  info rows %u err %u "
                      "mode %u delta %u freq %u ch %u locks %u scan %d grab %u ms late %u nosync %u\n",
                      fps, kbps, (unsigned long)s_rows, (unsigned long)s_parser.crc_errors, (unsigned long)s_bad_rows,
                      s_have_info ? s_info[LINK_INFO_ROWS] : 0, s_have_info ? s_info[LINK_INFO_ERROR] : 0,
                      picture_mode(), s_have_info ? s_info[LINK_INFO_DELTA] : 0,
                      s_have_info ? (unsigned)(s_info[LINK_INFO_MHZ] | (s_info[LINK_INFO_MHZ + 1] << 8)) : 0u,
                      s_have_info ? s_info[LINK_INFO_CHANNEL] : 0, s_have_info ? s_info[LINK_INFO_LOCKS] : 0,
                      scanning() ? 1 : 0,
                      s_have_info ? (unsigned)(s_info[LINK_INFO_GRAB_MS] | (s_info[LINK_INFO_GRAB_MS + 1] << 8)) : 0u,
                      s_have_info ? s_info[LINK_INFO_LATE] : 0, s_have_info ? s_info[LINK_INFO_NOSYNC] : 0);
#else
        (void)kbps;
#endif
        t_status = now;
        frames_at_status = s_frames_drawn;
        bytes_at_status = s_bytes;
        crc_at_status = s_parser.crc_errors;
        bad_at_status = s_bad_rows;
    }
}
