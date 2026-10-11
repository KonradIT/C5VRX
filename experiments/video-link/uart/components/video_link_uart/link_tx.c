/**
 * link_tx.c - the sending half of the UART video link; see link_tx.h.
 */

#include "link_tx.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#define STAGE_BYTES   (LINK_H * (LINK_OVERHEAD + LINK_ROW_MAX) + 2 * LINK_PKT_MAX)
#define FLUSH_MIN     512       /* hand staged bytes to the port in chunks of at least this much */
#define DELTA_START   2
#define DELTA_MAX     8         /* beyond this the picture shows contours; fewer fps is better */

static link_tx_port_t s_port;
static uint8_t  s_stage[STAGE_BYTES];
static size_t   s_len, s_sent;              /* staged, handed to the port */
static link_encoder_t s_enc;
static uint8_t  s_mode = LINK_MODE_AUTO;
static int      s_auto_delta = DELTA_START;
static uint32_t s_bytes_per_s = 400000u;
static uint8_t  s_frame;
static float    s_period_ms = 40.0f;        /* smoothed CPU time per frame: grab + encoding after it */

/* Rows captured but not yet encoded. */
static const uint8_t *s_q_row[LINK_H];
static uint8_t  s_q_y[LINK_H];
static int      s_q_head, s_q_tail;

static link_tx_stats_t s_cur, s_last;

void link_tx_init(const link_tx_port_t *port, uint32_t baud)
{
    s_port = *port;
    s_bytes_per_s = baud / 10u;
    link_encoder_set_delta(&s_enc, DELTA_START);
}

void link_tx_set_mode(uint8_t mode)
{
    if (mode < LINK_MODE_COUNT) s_mode = mode;
}

uint8_t link_tx_mode(void)
{
    return s_mode;
}

int link_tx_delta(void)
{
    return s_mode == LINK_MODE_RAW ? -1 : s_mode == LINK_MODE_FINE ? 1 : s_auto_delta;
}

/* ------------------------------------------------------------ staging */

static IRAM_ATTR void flush(bool block)
{
    size_t pending = s_len - s_sent;
    if (pending == 0u) return;
    if (block) {
        int64_t t0 = esp_timer_get_time();
        s_port.tx_write(s_port.ctx, s_stage + s_sent, pending);
        s_cur.flush_us += (uint32_t)(esp_timer_get_time() - t0);
        s_sent = s_len;
        return;
    }
    /* A write of at most half the free space never waits, even when the
     * port's buffer has its free space split in two (ESP-IDF's UART TX ring
     * buffer keeps items contiguous). */
    size_t free = s_port.tx_free(s_port.ctx);
    if (free < 2u * FLUSH_MIN + 128u) return;
    size_t n = free / 2u - 64u;
    if (n > pending) n = pending;
    SIM_COST(400 + n / 2);                          /* UART driver: mutex, ring buffer copy */
    s_port.tx_write(s_port.ctx, s_stage + s_sent, n);
    s_sent += n;
}

static IRAM_ATTR void stage_row(int y, const uint8_t *row)
{
    if (s_len + LINK_OVERHEAD + LINK_ROW_MAX > sizeof(s_stage)) flush(true);   /* cannot happen: one row per y */
    if (s_sent == s_len) s_sent = s_len = 0u;
    uint8_t *pkt = s_stage + s_len;
    int64_t t0 = esp_timer_get_time();
    int len = link_row_encode(&s_enc, row, pkt + LINK_HEADER, NULL);
    int size = link_packet_seal(pkt, LINK_T_ROW, s_frame, (uint8_t)y, len);
    SIM_COST(300 + 30 * LINK_W + 6 * size);        /* encoder ~30 per pixel, CRC ~6 per byte */
    s_len += (size_t)size;
    s_cur.bytes += (uint32_t)size;
    s_cur.encode_us += (uint32_t)(esp_timer_get_time() - t0);
}

/* ------------------------------------------------------------ frames */

void link_tx_frame_begin(uint8_t frame_id)
{
    s_frame = frame_id;
    s_q_head = s_q_tail = 0;
    if (s_sent == s_len) s_sent = s_len = 0u;
    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.delta = link_tx_delta();
    link_encoder_set_delta(&s_enc, s_cur.delta);
}

IRAM_ATTR void link_tx_on_row(void *ctx, int y, const uint8_t *row)
{
    (void)ctx;
    if (s_q_tail < LINK_H) {
        s_q_row[s_q_tail] = row;
        s_q_y[s_q_tail] = (uint8_t)y;
        s_q_tail++;
    }
}

IRAM_ATTR bool link_tx_on_idle(void *ctx)
{
    (void)ctx;
    if (s_q_head < s_q_tail) {
        stage_row(s_q_y[s_q_head], s_q_row[s_q_head]);
        s_q_head++;
        s_cur.rows_idle++;
        if (s_len - s_sent >= FLUSH_MIN) flush(false);
        return true;
    }
    if (s_len > s_sent) {
        size_t before = s_sent;
        flush(false);
        return s_sent != before;
    }
    return false;
}

static uint8_t error_code(const char *e)
{
    if (!e) return LINK_E_NONE;
    if (strstr(e, "horizontal")) return LINK_E_NO_HSYNC;
    if (strstr(e, "vertical sync")) return LINK_E_LOST_VSYNC;
    if (strstr(e, "vertical")) return LINK_E_NO_VSYNC;
    if (strstr(e, "lost")) return LINK_E_LOST;
    if (strstr(e, "timeout")) return LINK_E_TIMEOUT;
    return LINK_E_OTHER;
}

static uint8_t sat8(float v)
{
    return (uint8_t)(v <= 0.0f ? 0 : v >= 255.0f ? 255 : (int)(v + 0.5f));
}

void link_tx_build_info(uint8_t *in, int rows, const grab_info_t *info, const link_tx_status_t *st,
                        uint8_t mode, int delta)
{
    memset(in, 0, LINK_INFO_LEN);
    int ms = info->grab_ms > 65535 ? 65535 : info->grab_ms < 0 ? 0 : info->grab_ms;
    int ns = (int)(info->line_us * 1000.0f + 0.5f) - 60000;
    if (ns < 0) ns = 0;
    if (ns > 65535) ns = 65535;
    in[LINK_INFO_VERSION] = (uint8_t)LINK_VERSION;
    in[LINK_INFO_ROWS] = (uint8_t)(rows > 255 ? 255 : rows < 0 ? 0 : rows);
    in[LINK_INFO_FLAGS] = (uint8_t)((info->pal ? LINK_F_PAL : 0u) | (rows > 0 ? LINK_F_VIDEO : 0u) |
                                    ((unsigned)(mode & 3u) << LINK_F_MODE_SHIFT) |
                                    (st->scanning ? LINK_F_SCANNING : 0u) | (st->smoothing ? LINK_F_SMOOTHING : 0u));
    in[LINK_INFO_MHZ] = (uint8_t)(st->mhz & 0xffu);
    in[LINK_INFO_MHZ + 1] = (uint8_t)(st->mhz >> 8);
    in[LINK_INFO_GAIN] = st->gain;
    in[LINK_INFO_ERROR] = error_code(info->error);
    in[LINK_INFO_FIELDS] = (uint8_t)(info->fields > 255 ? 255 : info->fields);
    in[LINK_INFO_GRAB_MS] = (uint8_t)(ms & 0xff);
    in[LINK_INFO_GRAB_MS + 1] = (uint8_t)(ms >> 8);
    in[LINK_INFO_LINE_NS] = (uint8_t)(ns & 0xff);
    in[LINK_INFO_LINE_NS + 1] = (uint8_t)(ns >> 8);
    in[LINK_INFO_CHANNEL] = st->channel;
    in[LINK_INFO_LOCKS] = st->scan_locks;
    in[LINK_INFO_POWER] = sat8(info->power);
    in[LINK_INFO_CLIP] = sat8(info->clip * 100.0f);
    in[LINK_INFO_DELTA] = (uint8_t)(delta < 0 ? 0xff : delta);
    in[LINK_INFO_JITTER] = sat8(info->jitter_ns / 4.0f);
    in[LINK_INFO_LATE] = (uint8_t)(info->late > 255 ? 255 : info->late);
    in[LINK_INFO_NOSYNC] = (uint8_t)(info->nosync > 255 ? 255 : info->nosync);
}

static void stage_info(uint8_t frame_id, int rows, const grab_info_t *info, const link_tx_status_t *st, int delta)
{
    if (s_sent == s_len) s_sent = s_len = 0u;
    if (s_len + LINK_OVERHEAD + LINK_INFO_LEN > sizeof(s_stage)) flush(true);
    if (s_sent == s_len) s_sent = s_len = 0u;
    uint8_t *pkt = s_stage + s_len;
    link_tx_build_info(pkt + LINK_HEADER, rows, info, st, s_mode, delta);
    int size = link_packet_seal(pkt, LINK_T_INFO, frame_id, 0u, LINK_INFO_LEN);
    s_len += (size_t)size;
    s_cur.bytes += (uint32_t)size;
}

/* AUTO: size the next frame for the CPU time a frame takes (the grab, two
 * fields, plus the rows still to encode after it). Not the frame interval:
 * that grows when the link is the bottleneck, and would chase itself. */
static void rate_control(int rows, const grab_info_t *info, uint32_t bytes, uint32_t after_us)
{
    if (s_mode != LINK_MODE_AUTO || rows < LINK_H / 2) return;
    float nominal = info->pal ? 40.0f : 33.4f;          /* two fields */
    float cpu_ms = (float)info->grab_ms + (float)after_us * 0.001f;
    if (info->grab_ms > 0) s_period_ms += 0.25f * (cpu_ms - s_period_ms);
    float period = s_period_ms < nominal ? nominal : s_period_ms > 2.0f * nominal ? 2.0f * nominal : s_period_ms;
    float budget = (float)s_bytes_per_s * period * 0.001f * 0.92f;
    float full = (float)bytes * (float)LINK_H / (float)rows;
    if (full > budget * 1.25f) s_auto_delta += 2;
    else if (full > budget) s_auto_delta += 1;
    else if (full < budget * 0.80f) s_auto_delta -= 1;
    if (s_auto_delta < 0) s_auto_delta = 0;
    if (s_auto_delta > DELTA_MAX) s_auto_delta = DELTA_MAX;
}

void link_tx_frame_end(int rows, const grab_info_t *info, const link_tx_status_t *st)
{
    int64_t t0 = esp_timer_get_time();
    while (s_q_head < s_q_tail) {
        stage_row(s_q_y[s_q_head], s_q_row[s_q_head]);
        s_q_head++;
        s_cur.rows_after++;
    }
    stage_info(s_frame, rows, info, st, s_cur.delta);
    uint32_t after_us = (uint32_t)(esp_timer_get_time() - t0);
    flush(true);
    s_last = s_cur;
    rate_control(rows, info, s_cur.bytes, after_us);
}

void link_tx_info(uint8_t frame_id, int rows, const grab_info_t *info, const link_tx_status_t *st)
{
    stage_info(frame_id, rows, info, st, link_tx_delta());
    flush(true);
}

void link_tx_last_stats(link_tx_stats_t *out)
{
    *out = s_last;
}
