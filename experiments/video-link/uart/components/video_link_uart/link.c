/**
 * link.c - the UART video link (experiments/video-link/uart): stream the
 * grabbed frames to a display board over UART, scan for video, and the host
 * debug dumps. main/video.c calls video_link_start() and video_link_console()
 * (declared in video.h) in a build without the analog output.
 *
 * Streaming: a task grabs frames (grab.c) and hands every captured row to
 * the link sender (link_tx.c), which encodes rows while the grabber waits
 * for lines and sends them through the UART (uart_link.c) as the grab goes
 * on; the INFO packet closes each frame. The display board sends commands
 * back (link_proto.h: scan, channel steps, picture mode, smoothing).
 *
 * Gain: the video receiver's gain control is switched to manual; the
 * gain is adjusted between frames from the power and clipping of the
 * grabbed lines (the FM phase demodulator is indifferent to amplitude, but
 * mid-frame gain jumps broke lines on hardware).
 *
 * Scan: on command, step through every channel the receiver can tune
 * (bands R, A, B, E, F, L), probe each for about 40 ms for horizontal sync,
 * and stay on the first one that has it; the lock counter in the INFO
 * packet tells the display to beep.
 *
 * Host dumps on the console: 'v' raw ring snapshot plus its BitScrambler
 * loopback demodulation (tools/link_dump.py), 'g' one frame
 * (tools/link_frame.py).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/bitscrambler_loopback.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/bitscrambler_peri_select.h"
#include "sdkconfig.h"

#include "grab.h"
#include "link_tx.h"
#include "rf.h"
#include "uart_link.h"
#include "video.h"

#if CONFIG_C5VRX_LINK_DEBUG
BITSCRAMBLER_PROGRAM(s_fm_loop_program, "fm_loop");

/* One snapshot: the three newest completed 4092-byte descriptors, plus the
 * ring's short tail descriptor when it lies among them. */
#define IN_BYTES   (3u * 4092u + VIDEO_RX_RING_BYTES % 4092u)
#define OUT_BYTES  (IN_BYTES + 64u)                 /* 1:1 bytes plus trailing garbage */
#endif

#define GRAB_TIMEOUT_MS 300
#define PROBE_MS        40      /* per channel while scanning */
#define SETTLE_MS       3       /* after a retune, before probing */
#define SCAN_GAIN       56      /* sensitive; the gain control takes over after the lock */
#define START_GAIN      56
#define SCAN_MAX_OFFSET 10.0f   /* MHz: a carrier further off belongs to a neighbouring channel */
#define CHANNELS        48

#if CONFIG_C5VRX_LINK_DEBUG
static bitscrambler_handle_t s_bs;
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_in[IN_BYTES];
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_out[OUT_BYTES];
#endif
static SemaphoreHandle_t s_grab_lock;         /* grab_frame() is not reentrant; also guards s_img */
static uint8_t s_img[GRAB_H][GRAB_W];         /* the frame being grabbed and sent */
static volatile bool s_dumping;
static volatile bool s_streaming = CONFIG_C5VRX_LINK_STREAM;

/* Requests from the console and from the display, served by the stream task
 * (it owns the tuner while streaming). */
static volatile bool s_req_scan_start, s_req_scan_stop;
static volatile int  s_req_step;                  /* channels to step, +/- */
static volatile int  s_req_channel = -1;          /* channel index to tune */
static bool    s_scanning;
static uint8_t s_scan_locks;
static int     s_scan_left;                       /* channels left in this pass */
static bool    s_untunable[CHANNELS];

static void stream_task(void *arg);

static const char *mode_name(uint8_t mode)
{
    return mode == LINK_MODE_AUTO ? "auto" : mode == LINK_MODE_FINE ? "fine" : "raw";
}

/* ------------------------------------------------------------ commands */

static void on_command(uint8_t cmd, uint8_t arg)
{
    switch (cmd) {
    case LINK_CMD_SCAN_START: s_req_scan_start = true; break;
    case LINK_CMD_SCAN_STOP:  s_req_scan_stop = true; break;
    case LINK_CMD_CHANNEL_STEP: s_req_step += (int8_t)arg; break;
    case LINK_CMD_MODE: link_tx_set_mode(arg); break;
    case LINK_CMD_SMOOTHING: grab_set_smoothing(arg != 0u); break;
    case LINK_CMD_CHANNEL_SET: s_req_channel = arg; break;
    default: break;
    }
}

static void link_stream_toggle(void)
{
    s_streaming = !s_streaming;
    printf("[LINK] streaming %s\n", s_streaming ? "on" : "off");
}

static void link_mode_cycle(void)
{
    uint8_t mode = (uint8_t)((link_tx_mode() + 1u) % LINK_MODE_COUNT);
    link_tx_set_mode(mode);
    printf("[LINK] picture mode %s\n", mode_name(mode));
}

static void link_scan_toggle(void)
{
    if (s_scanning) s_req_scan_stop = true;
    else s_req_scan_start = true;
}

static void link_smoothing_toggle(void)
{
    grab_set_smoothing(!grab_smoothing());
    printf("[LINK] horizontal smoothing %s\n", grab_smoothing() ? "on" : "off");
}

/* ------------------------------------------------------------ init */

#if CONFIG_C5VRX_LINK_DEBUG
#define LINK_DEBUG_KEYS ", 'g' dump a frame, 'v' dump raw I/Q, [STREAM] statistics"
#else
#define LINK_DEBUG_KEYS ""
#endif

static esp_err_t link_init(void)
{
    printf("[LINK] free heap %u bytes, largest DMA block %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    esp_err_t err;
#if CONFIG_C5VRX_LINK_DEBUG
    /* Any idle peripheral works as the attachment; I2S0 is unused here and
     * is what the IDF loopback example uses. PARLIO must stay free for RX. */
    err = bitscrambler_loopback_create(&s_bs, SOC_BITSCRAMBLER_ATTACH_I2S0, OUT_BYTES);
    if (err != ESP_OK) {
        printf("[LINK] bitscrambler_loopback_create: %s\n", esp_err_to_name(err));
        return err;
    }
    err = bitscrambler_load_program(s_bs, s_fm_loop_program);
    if (err != ESP_OK) {
        printf("[LINK] bitscrambler_load_program: %s\n", esp_err_to_name(err));
        return err;
    }
#endif
    grab_init();
    s_grab_lock = xSemaphoreCreateMutex();
    if (!s_grab_lock) return ESP_ERR_NO_MEM;
    if ((err = uart_link_init(on_command)) != ESP_OK) {
        printf("[LINK] UART link: %s\n", esp_err_to_name(err));
        return err;
    }
    link_tx_set_mode((uint8_t)CONFIG_C5VRX_LINK_PICTURE_MODE);
    video_set_rx_gain(START_GAIN);   /* manual gain from here on, see link_gain_control() */
    if (xTaskCreate(stream_task, "link_stream", 4096, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    printf("[LINK] frame grabber %ux%u, streaming %s, picture mode %s; keys: 'S' stream, 'P' picture mode, "
           "'N' scan, 'M' smoothing%s\n",
           (unsigned)GRAB_W, (unsigned)GRAB_H, s_streaming ? "on" : "off", mode_name(link_tx_mode()),
           LINK_DEBUG_KEYS);
    return ESP_OK;
}

/* ------------------------------------------------------------ host dumps */

#if CONFIG_C5VRX_LINK_DEBUG

static void print_hex_block(const char *name, const uint8_t *p, size_t n)
{
    static const char k_hex[] = "0123456789abcdef";
    static char row[64u * 2u + 2u];
    printf("[DUMP %s len=%u gain=%u freq=%u bw40=%d]\n", name, (unsigned)n,
           (unsigned)(rf_get_rx_gain_reg() >> 24), (unsigned)rf_get_frequency_mhz(),
           rf_get_analog_bandwidth() ? 1 : 0);
    for (size_t off = 0u; off < n; off += 64u) {
        size_t len = n - off < 64u ? n - off : 64u;
        for (size_t k = 0u; k < len; ++k) {
            row[2u * k]      = k_hex[p[off + k] >> 4];
            row[2u * k + 1u] = k_hex[p[off + k] & 0x0fu];
        }
        row[2u * len] = '\n';
        row[2u * len + 1u] = '\0';
        fputs(row, stdout);
    }
    printf("[DUMP end %s]\n", name);
    fflush(stdout);
}

static void link_dump(void)
{
    if (!s_bs) {
        printf("[DUMP error link not initialised]\n");
        return;
    }
    s_dumping = true;
    vTaskDelay(pdMS_TO_TICKS(60));   /* let a status line already in flight finish */

    size_t n = video_copy_recent_rx(s_in, IN_BYTES);
    if (n == 0u) {
        printf("[DUMP error no completed RX descriptors]\n");
        s_dumping = false;
        return;
    }
    n &= ~(size_t)1u;                /* whole 16-bit reads for the core */
    print_hex_block("raw", s_in, n);

    size_t written = 0u;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = bitscrambler_loopback_run(s_bs, s_in, n, s_out, OUT_BYTES, &written);
    int64_t dt = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        printf("[DUMP error loopback %s after %lld us, written=%u]\n", esp_err_to_name(err), dt, (unsigned)written);
        fflush(stdout);
        s_dumping = false;
        return;
    }
    printf("[DUMP info loopback %u -> %u bytes in %lld us]\n", (unsigned)n, (unsigned)written, dt);
    print_hex_block("cvbs", s_out, written < n ? written : n);
    s_dumping = false;
}

#endif /* CONFIG_C5VRX_LINK_DEBUG (dump helpers) */

/* The grabber busy-waits on the ring; run it above the AGC task so no line
 * is lost to preemption. The caller holds s_grab_lock. */
static int grab_high(grab_info_t *info, int timeout_ms, bool stream)
{
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 10);
    int rows = grab_frame(&s_img[0][0], info, timeout_ms, stream ? link_tx_on_row : NULL,
                          stream ? link_tx_on_idle : NULL, NULL);
    vTaskPrioritySet(NULL, prio);
    return rows;
}

static bool locked_probe(int timeout_ms, grab_info_t *info)
{
    xSemaphoreTake(s_grab_lock, portMAX_DELAY);
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 10);
    bool video = grab_probe(timeout_ms, info);
    vTaskPrioritySet(NULL, prio);
    xSemaphoreGive(s_grab_lock);
    return video;
}

#if CONFIG_C5VRX_LINK_DEBUG
static void link_grab(void)
{
    s_dumping = true;
    vTaskDelay(pdMS_TO_TICKS(60));   /* let a status line already in flight finish */

    xSemaphoreTake(s_grab_lock, portMAX_DELAY);
    grab_info_t info;
    int rows = grab_high(&info, 400, false);

    printf("[FRAME w=%u h=%u rows=%d std=%s line_us=%.3f fields=%d acquire_ms=%d grab_ms=%d late=%d nosync=%d "
           "sync=%.2f blank=%.2f jitter_ns=%.0f windows=%d window_us=%d row_us=%d cpu_us=%d carrier_mhz=%.2f "
           "freq=%u gain=%u error=%s]\n",
           (unsigned)GRAB_W, (unsigned)GRAB_H, rows, info.pal ? "PAL" : "NTSC", (double)info.line_us,
           info.fields, info.acquire_ms, info.grab_ms, info.late, info.nosync,
           (double)info.sync_level, (double)info.blank_level, (double)info.jitter_ns,
           info.windows, info.windows ? info.window_us / info.windows : 0,
           rows ? info.row_us / rows : 0, rows ? info.cpu_us / rows : 0, (double)info.carrier_mhz,
           (unsigned)rf_get_frequency_mhz(), (unsigned)(rf_get_rx_gain_reg() >> 24),
           info.error ? info.error : "none");
    if (rows > 0) {
        static const char k_hex[] = "0123456789abcdef";
        static char line[4u + 2u * GRAB_W + 2u];
        for (int y = 0; y < GRAB_H; ++y) {
            line[0] = (char)('0' + y / 100);
            line[1] = (char)('0' + (y / 10) % 10);
            line[2] = (char)('0' + y % 10);
            line[3] = ':';
            for (int x = 0; x < GRAB_W; ++x) {
                line[4 + 2 * x] = k_hex[s_img[y][x] >> 4];
                line[5 + 2 * x] = k_hex[s_img[y][x] & 0x0f];
            }
            line[4 + 2 * GRAB_W] = '\n';
            line[5 + 2 * GRAB_W] = '\0';
            fputs(line, stdout);
        }
    }
    printf("[FRAME end]\n");
    fflush(stdout);
    xSemaphoreGive(s_grab_lock);
    s_dumping = false;
}
#endif /* CONFIG_C5VRX_LINK_DEBUG */

/* ------------------------------------------------------------ statistics */

#if CONFIG_C5VRX_LINK_DEBUG

typedef struct {
    int frames, complete, partial, empty;
    int err_timeout, err_lost, err_nohsync, err_novsync, err_other;
    int grab_ms_sum, grab_ms_max, fields_sum, late, nosync, acquires, acquire_ms_sum;
    float jitter_sum;
    int jitter_n;
    float power_sum, clip_sum;
    int big_err, max_err, wide_hits, fifo_ovf, vslips, vmisses, skipped, substituted;
    int tb_bad, tb_lead_min_ns, tb_lead_max_ns;
    long rows, cpu_us, bytes, encode_us, rows_idle, rows_after, flush_us, idle_us;
    int delta_sum, delta_n;
} stream_stats_t;

static void stats_reset(stream_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->tb_lead_min_ns = 1 << 30;
    st->tb_lead_max_ns = -(1 << 30);
}

static void stats_add(stream_stats_t *st, int rows, const grab_info_t *info, const link_tx_stats_t *tx)
{
    st->frames++;
    if (rows == GRAB_H) st->complete++;
    else if (rows > 0) st->partial++;
    else st->empty++;
    const char *e = info->error;
    if (e) {
        if (strstr(e, "timeout")) st->err_timeout++;
        else if (strstr(e, "lost")) st->err_lost++;
        else if (strstr(e, "horizontal")) st->err_nohsync++;
        else if (strstr(e, "vertical")) st->err_novsync++;
        else st->err_other++;
    }
    st->grab_ms_sum += info->grab_ms;
    if (info->grab_ms > st->grab_ms_max) st->grab_ms_max = info->grab_ms;
    st->fields_sum += info->fields;
    st->late += info->late;
    st->nosync += info->nosync;
    if (info->acquire_ms > 0 || info->windows > 0) {
        st->acquires++;
        st->acquire_ms_sum += info->acquire_ms;
    }
    if (info->jitter_ns > 0.0f) {
        st->jitter_sum += info->jitter_ns;
        st->jitter_n++;
    }
    st->power_sum += info->power;
    st->clip_sum += info->clip;
    st->big_err += info->big_err;
    if (info->max_err > st->max_err) st->max_err = info->max_err;
    st->wide_hits += info->wide_hits;
    st->fifo_ovf += info->fifo_ovf;
    st->vslips += info->vslips;
    st->vmisses += info->vmisses;
    st->skipped += info->skipped;
    st->substituted += info->substituted;
    if (info->tb_bad) st->tb_bad++;
    if (info->tb_lead_min_ns < st->tb_lead_min_ns) st->tb_lead_min_ns = info->tb_lead_min_ns;
    if (info->tb_lead_max_ns > st->tb_lead_max_ns) st->tb_lead_max_ns = info->tb_lead_max_ns;
    st->rows += rows;
    st->cpu_us += info->cpu_us;
    st->idle_us += info->idle_us;
    st->bytes += (long)tx->bytes;
    st->encode_us += (long)tx->encode_us;
    st->rows_idle += tx->rows_idle;
    st->rows_after += tx->rows_after;
    st->flush_us += (long)tx->flush_us;
    if (rows > 0 && tx->delta >= 0) {
        st->delta_sum += tx->delta;
        st->delta_n++;
    }
}

static void stats_print(const stream_stats_t *st, int ms)
{
    int n = st->frames ? st->frames : 1;
    long enc_rows = st->rows_idle + st->rows_after;
    uint32_t rx_packets, rx_errors;
    uart_link_rx_stats(&rx_packets, &rx_errors);
    printf("[STREAM] %d ms: frames=%d complete=%d partial=%d empty=%d | timeout=%d lost=%d nohsync=%d novsync=%d other=%d"
           " | grab avg=%d max=%d ms fields/frame=%.1f late=%d sub=%d nosync=%d skipped=%d acquires=%d (avg %d ms) jitter=%.0f ns"
           " | cpu=%ld us/row idle=%ld us/frame | %s delta=%.1f bytes=%ld/frame encode=%ld us/row (%ld%% while waiting)"
           " flush=%ld us/frame | gain=%u power=%.1f clip=%.1f%% | big_err=%d max_err=%d wide=%d fifo_ovf=%d vslip=%d"
           " vmiss=%d | timebase bad=%d lead=%d..%d ns | cmds rx=%lu err=%lu\n",
           ms, st->frames, st->complete, st->partial, st->empty,
           st->err_timeout, st->err_lost, st->err_nohsync, st->err_novsync, st->err_other,
           st->grab_ms_sum / n, st->grab_ms_max, (double)st->fields_sum / n, st->late, st->substituted, st->nosync,
           st->skipped,
           st->acquires, st->acquires ? st->acquire_ms_sum / st->acquires : 0,
           st->jitter_n ? (double)(st->jitter_sum / st->jitter_n) : 0.0,
           st->rows ? st->cpu_us / st->rows : 0L, st->idle_us / n,
           mode_name(link_tx_mode()), st->delta_n ? (double)st->delta_sum / st->delta_n : -1.0,
           st->bytes / n, enc_rows ? st->encode_us / enc_rows : 0L, enc_rows ? 100L * st->rows_idle / enc_rows : 0L,
           st->flush_us / n, (unsigned)(rf_get_rx_gain_reg() >> 24),
           (double)(st->power_sum / n), (double)(st->clip_sum * 100.0f / n),
           st->big_err, st->max_err, st->wide_hits, st->fifo_ovf, st->vslips, st->vmisses,
           st->tb_bad, st->tb_lead_min_ns, st->tb_lead_max_ns, (unsigned long)rx_packets, (unsigned long)rx_errors);
}

#endif /* CONFIG_C5VRX_LINK_DEBUG (statistics) */

/* ------------------------------------------------------------ gain, tuning, scan */

/* Between frames only, small steps, wide window (see the file comment). */
static void link_gain_control(const grab_info_t *info)
{
    int g = video_rx_gain(), ng = g;
    if (info->power <= 0.0f) return;
    if (info->clip > 0.05f || info->power > 55.0f) ng -= 4;
    else if (info->power > 40.0f) ng -= 2;
    else if (info->power < 5.0f) ng += 4;
    else if (info->power < 12.0f) ng += 2;
    if (ng < 16) ng = 16;
    if (ng > 62) ng = 62;
    if (ng != g) video_set_rx_gain((uint8_t)ng);
}

/* Tune channel index idx; false for channels this receiver cannot tune
 * (remembered, so the scan does not ask the driver again). */
static bool tune(int idx)
{
    int count = (int)rf_get_channel_count();
    if (count > CHANNELS) count = CHANNELS;
    if (idx < 0 || idx >= count || s_untunable[idx]) return false;
    esp_err_t err = rf_set_channel((size_t)idx);
    if (err == ESP_ERR_NOT_SUPPORTED) s_untunable[idx] = true;
    if (err != ESP_OK) return false;
    grab_unlock();
    return true;
}

/* Step to the next tunable channel in direction dir. */
static void step_channel(int dir)
{
    int count = (int)rf_get_channel_count();
    if (count > CHANNELS) count = CHANNELS;
    int idx = (int)rf_get_channel_index();
    for (int tries = 0; tries < count; ++tries) {
        idx = (idx + (dir > 0 ? 1 : -1) + count) % count;
        if (tune(idx)) break;
    }
}

static link_tx_status_t link_status(void)
{
    link_tx_status_t st;
    memset(&st, 0, sizeof(st));
    st.mhz = rf_get_frequency_mhz();
    st.gain = (uint8_t)(rf_get_rx_gain_reg() >> 24);
    st.channel = (uint8_t)rf_get_channel_index();
    st.scan_locks = s_scan_locks;
    st.scanning = s_scanning;
    st.smoothing = grab_smoothing();
    return st;
}

static void report_channel(void)
{
    const fpv_channel_t *ch = rf_get_current_channel();
    printf("[LINK] tuned to %s (%u MHz)\n", ch ? ch->name : "?", (unsigned)rf_get_frequency_mhz());
}

static void serve_requests(void)
{
    if (s_req_scan_start) {
        s_req_scan_start = false;
        s_scanning = true;
        s_scan_left = (int)rf_get_channel_count();
        printf("[SCAN] started\n");
    }
    if (s_req_scan_stop) {
        s_req_scan_stop = false;
        if (s_scanning) {
            s_scanning = false;
            printf("[SCAN] stopped, staying on %s (%u MHz)\n", rf_get_current_channel()->name,
                   (unsigned)rf_get_frequency_mhz());
        }
    }
    if (s_req_channel >= 0) {
        int idx = s_req_channel;
        s_req_channel = -1;
        s_scanning = false;
        if (tune(idx)) report_channel();
    }
    int step = s_req_step;
    if (step != 0) {
        s_req_step -= step;
        s_scanning = false;
        for (int k = 0; k < (step > 0 ? step : -step); ++k) step_channel(step);
        report_channel();
    }
}

/* One scan step: next tunable channel, probe it, stay if it has video whose
 * carrier sits near the tuned frequency. */
static void scan_step(uint8_t frame)
{
    step_channel(+1);
    video_set_rx_gain(SCAN_GAIN);
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    grab_info_t info;
    bool video = locked_probe(PROBE_MS, &info);
    bool near = fabsf(info.carrier_mhz) <= SCAN_MAX_OFFSET;
    if (video && near) {
        s_scanning = false;
        s_scan_locks++;
        printf("[SCAN] video on %s (%u MHz), %s, carrier %+.1f MHz\n", rf_get_current_channel()->name,
               (unsigned)rf_get_frequency_mhz(), info.pal ? "PAL" : "NTSC", (double)info.carrier_mhz);
    } else if (video) {
        printf("[SCAN] %u MHz: sync, but the carrier is %+.1f MHz off; scanning on\n",
               (unsigned)rf_get_frequency_mhz(), (double)info.carrier_mhz);
    }
    if (s_scanning && --s_scan_left <= 0) {
        s_scan_left = (int)rf_get_channel_count();
        printf("[SCAN] no video in a full pass, scanning on\n");
    }
    link_tx_status_t st = link_status();
    link_tx_info(frame, 0, &info, &st);
}

/* ------------------------------------------------------------ stream task */

static void stream_task(void *arg)
{
    (void)arg;
    uint8_t frame = 0u;
#if CONFIG_C5VRX_LINK_DEBUG
    stream_stats_t st;
    stats_reset(&st);
    int64_t t_stats = esp_timer_get_time();
#endif
    for (;;) {
        serve_requests();
        if (!s_streaming || s_dumping) {
            vTaskDelay(pdMS_TO_TICKS(50));
#if CONFIG_C5VRX_LINK_DEBUG
            t_stats = esp_timer_get_time();
            stats_reset(&st);
#endif
            continue;
        }
        if (s_scanning) {
            scan_step(frame++);
            continue;
        }

        grab_info_t info;
        link_tx_stats_t tx;
        xSemaphoreTake(s_grab_lock, portMAX_DELAY);
        link_tx_frame_begin(frame);
        int rows = grab_high(&info, GRAB_TIMEOUT_MS, true);
        link_tx_status_t lst = link_status();
        link_tx_frame_end(rows, &info, &lst);
        xSemaphoreGive(s_grab_lock);
        link_tx_last_stats(&tx);
        frame++;
#if CONFIG_C5VRX_LINK_DEBUG
        stats_add(&st, rows, &info, &tx);
        if (info.error && strstr(info.error, "lost")) {
            /* Show how the lock was lost, at most once per second. */
            static int64_t t_last_dump;
            int64_t tn = esp_timer_get_time();
            if (tn - t_last_dump > 1000000) {
                t_last_dump = tn;
                grab_trace_t tr[GRAB_TRACE_LEN];
                int nt = grab_trace(tr, GRAB_TRACE_LEN);
                printf("[LOST] %s, last %d rows (line:err/result):", info.error, nt);
                for (int k = 0; k < nt; ++k) printf(" %ld:%d%c", (long)tr[k].line, tr[k].err, tr[k].result);
                printf("\n");
            }
        }
#else
        (void)tx;
#endif
        link_gain_control(&info);
        /* The grab busy-waits at priority 10 and the sender no longer blocks
         * on the UART, so without this the console (priority 1) and the idle
         * task would never run. One tick per frame; the next grab starts in
         * whatever field is on air, so it costs about a millisecond. */
        vTaskDelay(rows == 0 ? pdMS_TO_TICKS(20) : 1);

#if CONFIG_C5VRX_LINK_DEBUG
        int64_t now = esp_timer_get_time();
        if (now - t_stats >= 1000000) {
            stats_print(&st, (int)((now - t_stats) / 1000));
            stats_reset(&st);
            t_stats = now;
        }
#endif
    }
}

/* ------------------------------------------------------------ main/video.c hooks */

esp_err_t video_link_start(void)
{
    esp_err_t err = link_init();
    if (err != ESP_OK) return err;
    /* The exact VTX frequency: hardware captures showed a 2 MHz offset
     * crushing the demodulated range. */
    if (rf_set_channel((size_t)CONFIG_C5VRX_LINK_CHANNEL) == ESP_OK) report_channel();
    return ESP_OK;
}

bool video_link_console(int key)
{
    switch (key) {
#if CONFIG_C5VRX_LINK_DEBUG
    case 'v': link_dump(); return true;
    case 'g': link_grab(); return true;
#endif
    case 'S': link_stream_toggle(); return true;
    case 'P': link_mode_cycle(); return true;
    case 'N': link_scan_toggle(); return true;
    case 'M': link_smoothing_toggle(); return true;
    default:  return false;
    }
}
