/**
 * grab_sim.c - host test bench for main/grab.c.
 *
 * Generates PAL (625/50) or NTSC (525/60) interlaced composite video of a
 * test pattern with the full vertical interval, FM-modulates it like an
 * analog VTX (optionally off-nominal line rate, carrier offset, noise),
 * quantizes it to the receiver's Q4/I4 bytes and streams it into the RX
 * ring (VIDEO_RX_RING_BYTES) with the PARLIO RX descriptor layout (4092-byte
 * descriptors, then the short tail: 8 x 4092 + 32 bytes for 32 KiB). Or it
 * replays a raw capture from hardware (tools/link_dump.py) in a loop.
 *
 * Simulated time advances on every esp_timer_get_time() call, so the "DMA"
 * keeps running while grab.c computes and late data is detected like on
 * hardware (though it is not overwritten mid-read). Two clocks:
 *
 *  --cpu model (default): the hot paths of grab.c and link_tx.c report the
 *    RISC-V instructions they execute (SIM_COST, counted from the -O2
 *    disassembly), charged at CPI_C5 cycles each at 240 MHz. Deterministic;
 *    calibrated against hardware: the C5 needs about 255 us per 6144-sample
 *    acquisition window ([FRAME] dumps of 2026-09-19 01:02), which the
 *    model reproduces (run --nosignal --probe and compare "us each").
 *  --cpu host: host CPU time between calls times --slowdown (36, same
 *    calibration), the row encoder at --enc-slowdown. Noisy: host
 *    scheduling and branch behaviour leak in; kept for comparison.
 *
 * The frames go through the link's sender (main/link_tx.c: row queue,
 * encoding while the grabber waits, rate control) into a model of the UART
 * (TX buffer drained at the baud rate in simulated time) and from there
 * into the display side of the protocol (protocol/link_proto.h: parser, row
 * decoder), as on the T-Embed; every decoded row is checked against the
 * grabbed one (within the codec's delta), and the frame rate is measured
 * in simulated time.
 *
 * Every grabbed frame is checked: the row of a white marker line at picture
 * height 0.5 (vertical stability), the column of the black diagonal on a
 * reference row (horizontal stability), the noise in a flat grey bar and the
 * width of a bar edge (sharpness).
 *
 * build: test/run_tests.sh (see there for the include paths)
 *            -lm -o grab_sim
 * usage: grab_sim [--std pal|ntsc] [--frames N] [--noise SIGMA] [--cfo MHZ] [--ppm PPM]
 *                 [--cpu model|host] [--gap-ms MS] [--slowdown K] [--enc-slowdown K]
 *                 [--smooth 0|1] [--out PREFIX]
 *                 [--mode auto|fine|raw] [--baud B] [--encode 0|1] [--nosignal] [--probe]
 *                 [--replay FILE] [--seed N]
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "grab.h"
#include "link_proto.h"
#include "link_tx.h"
#include "video.h"

#define RING VIDEO_RX_RING_BYTES
#define FS   40e6

static uint8_t g_ring[RING];
static int64_t g_written;
static double  g_time_us = 1000.0;
static double  g_phase;                     /* carrier phase, turns */
static bool    g_pal = true;
static double  g_noise = 0.6;
static double  g_cfo = 0.3e6;
static double  g_radius = 5.0;
static double  g_hz_per_volt = 7.8e6;
static double  g_ppm;
static double  g_line_us;
static int     g_lines;
static int64_t g_start_offset = 123457;
static bool    g_nosignal;
static uint8_t *g_replay;
static long    g_replay_len;
static double  g_slowdown = 36.0;
static bool    g_cpu_model = true;
static double  g_cost_instr;                /* instructions charged since the last clock update */
#define CPI_C5       1.73                   /* cycles per instruction, from the window calibration */
#define C5_MHZ       240.0
#define TIMER_INSTR  60                     /* esp_timer_get_time() itself */

void grab_sim_cost(uint32_t instructions)
{
    g_cost_instr += instructions;
}
static double  g_enc_slowdown = 21.0;
static bool    g_trace_late;
static double  g_exit_us;                   /* host clock at the end of the last timer call (host mode) */

/* Host monotonic clock, microseconds. */
static double host_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
#endif
}

/* ------------------------------------------------------------ stub API */

int64_t esp_timer_get_time(void);
static void advance_to(int64_t S);
static void model_clock(void);
const uint8_t *video_rx_ring(void) { return g_ring; }
uint32_t video_rx_max_descriptor(void) { return 4092u; }

bool video_rx_write_offset(uint32_t *offset)
{
    /* A live register on hardware: the work done so far has taken its time. */
    if (g_cpu_model) {
        g_cost_instr += 10;
        model_clock();
    }
    uint32_t pos = (uint32_t)(g_written % RING);
    *offset = pos - pos % 4092u;       /* start of the descriptor being written */
    return true;
}

/* ------------------------------------------------------------ test signal */

static double pattern(double x, double y)       /* 0..1 luma, x, y in [0,1) */
{
    if (fabs(y - 0.5) < 0.004) return 1.0;                  /* marker line */
    double cx = x - 0.5, cy = (y - 0.5) * 0.75;
    double r = sqrt(cx * cx + cy * cy);
    if (fabs(r - 0.28) < 0.012) return 1.0;                 /* white ring */
    if (fabs(x - y) < 0.01) return 0.0;                     /* black diagonal */
    if (y < 0.33) return floor(x * 8.0) / 7.0;              /* 8 grey bars */
    if (y < 0.66) return x;                                 /* horizontal ramp */
    return ((int)(x * 16) + (int)(y * 12)) & 1 ? 0.85 : 0.15;   /* checkerboard */
}

static char pal_half(int L, int h)
{
    if (L == 623) return h ? 'e' : 'n';
    if (L == 624 || L == 625) return 'e';
    if (L == 1 || L == 2) return 'b';
    if (L == 3) return h ? 'e' : 'b';
    if (L == 4 || L == 5) return 'e';
    if (L == 311 || L == 312) return 'e';
    if (L == 313) return h ? 'b' : 'e';
    if (L == 314 || L == 315) return 'b';
    if (L == 316 || L == 317) return 'e';
    if (L == 318) return h ? 'x' : 'e';
    return 'n';
}

static char ntsc_half(int L, int h)
{
    int hl = (L - 1) * 2 + h;
    if (hl < 6) return 'e';
    if (hl < 12) return 'b';
    if (hl < 18) return 'e';
    int h2 = hl - 525;
    if (h2 >= 0 && h2 < 6) return 'e';
    if (h2 >= 6 && h2 < 12) return 'b';
    if (h2 >= 12 && h2 < 18) return 'e';
    return 'n';
}

static double composite(int64_t s)
{
    double t_us = (double)(s + g_start_offset) / 40.0;
    double frame_us = g_line_us * g_lines;
    double tf = fmod(t_us, frame_us);
    int L = (int)(tf / g_line_us) + 1;
    double tl = tf - (L - 1) * g_line_us;
    int h = tl >= g_line_us * 0.5 ? 1 : 0;
    double th = tl - h * g_line_us * 0.5;
    char k = g_pal ? pal_half(L, h) : ntsc_half(L, h);
    const double SYNC = -0.3, BLANK = 0.0;
    switch (k) {
    case 'e': return th < 2.35 ? SYNC : BLANK;
    case 'b': return th < (g_line_us * 0.5 - 4.7) ? SYNC : BLANK;
    case 'x': return BLANK;
    default: break;
    }
    if (tl < 4.7) return SYNC;
    double a0 = g_pal ? 10.5 : 9.4, alen = g_pal ? 52.0 : 52.6;
    int first1 = g_pal ? 23 : 21, last1 = g_pal ? 310 : 262;
    int first2 = g_pal ? 336 : 284, last2 = g_pal ? 622 : 524;
    int row = -1, rows = g_pal ? 576 : 484;
    if (L >= first1 && L <= last1) row = 2 * (L - first1);
    else if (L >= first2 && L <= last2) row = 2 * (L - first2) + 1;
    if (row < 0 || tl < a0 || tl >= a0 + alen) return BLANK;
    return 0.7 * pattern((tl - a0) / alen, (double)row / rows);
}

/* Noise and the carrier come from tables: the receiver sees 4-bit I/Q, so
 * table precision is irrelevant, and Box-Muller plus sin/cos per sample made
 * the signal generator the slowest part of the bench. */
#define GAUSS_N  65536
#define SIN_N    4096
static float    g_gauss[GAUSS_N];
static float    g_sin[SIN_N + SIN_N / 4];           /* sin, and cos at [k + SIN_N / 4] */
static uint32_t g_rng = 2463534242u;

static double gauss_slow(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static void tables_init(void)
{
    for (int k = 0; k < GAUSS_N; ++k) g_gauss[k] = (float)gauss_slow();
    for (int k = 0; k < SIN_N + SIN_N / 4; ++k) g_sin[k] = (float)sin(2.0 * M_PI * k / SIN_N);
}

static inline float gauss(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_gauss[g_rng & (GAUSS_N - 1)];
}

static int q4(double v)
{
    int n = (int)lround(v);
    return n < -8 ? -8 : n > 7 ? 7 : n;
}

static void advance_to(int64_t S)
{
    while (g_written < S) {
        uint8_t byte;
        if (g_replay) {
            byte = g_replay[g_written % g_replay_len];
        } else {
            double amp = g_nosignal ? 0.0 : g_radius;
            if (!g_nosignal) {
                double f = g_cfo + g_hz_per_volt * composite(g_written);
                g_phase += f / FS;                                  /* turns */
                g_phase -= floor(g_phase);
            }
            int k = (int)(g_phase * SIN_N) & (SIN_N - 1);
            int i = q4(amp * g_sin[k + SIN_N / 4] + g_noise * gauss());
            int q = q4(amp * g_sin[k] + g_noise * gauss());
            byte = (uint8_t)(((i & 15) << 4) | (q & 15));
        }
        g_ring[g_written % RING] = byte;
        g_written++;
    }
}

static void model_clock(void)
{
    g_time_us += g_cost_instr * CPI_C5 / C5_MHZ;
    g_cost_instr = 0.0;
    advance_to((int64_t)(g_time_us * 40.0));
}

int64_t esp_timer_get_time(void)
{
    if (g_cpu_model) {
        g_cost_instr += TIMER_INSTR;
        model_clock();
        return (int64_t)g_time_us;
    }
    double now = host_now_us();
    if (g_exit_us > 0.0) {
        /* Host CPU time since the last call, scaled. No code path between two
         * calls takes more than a few microseconds on the host; anything
         * longer is the host scheduler, not the receiver: capped. */
        double host_us = now - g_exit_us;
        if (host_us > 30.0) host_us = 30.0;
        g_time_us += host_us * g_slowdown;
    }
    g_time_us += 0.3;
    advance_to((int64_t)(g_time_us * 40.0));
    g_exit_us = host_now_us();
    return (int64_t)g_time_us;
}

static void idle_ms(double ms)
{
    g_time_us += ms * 1000.0;
    advance_to((int64_t)(g_time_us * 40.0));
    g_exit_us = host_now_us();
}

static void idle_us(double us)
{
    idle_ms(us / 1000.0);
}

/* The sender's idle work, charged at the encoder's slowdown: the clock is
 * brought up to date at the grabber's rate first, and closed at the
 * encoder's rate afterwards. */
static bool sim_on_idle(void *ctx)
{
    esp_timer_get_time();
    double grab_rate = g_slowdown;
    g_slowdown = g_enc_slowdown;
    bool did = link_tx_on_idle(ctx);
    esp_timer_get_time();
    g_slowdown = grab_rate;
    return did;
}

/* ------------------------------------------------------------ UART model and display */

/* The UART: a TX buffer of g_uart_cap bytes drained at baud / 10 bytes per
 * second of simulated time; a write that does not fit waits (simulated time
 * passes). Everything written goes to the "display": the protocol parser and
 * row decoder of link_proto.h, as on the T-Embed. */
static double  g_uart_rate = 0.4;           /* bytes per us */
static double  g_uart_queue, g_uart_t;
static size_t  g_uart_cap = 16384;
static long    g_uart_total;

static void idle_us(double us);

static void uart_update(void)
{
    g_uart_queue -= (g_time_us - g_uart_t) * g_uart_rate;
    if (g_uart_queue < 0.0) g_uart_queue = 0.0;
    g_uart_t = g_time_us;
}

static size_t port_free(void *ctx)
{
    (void)ctx;
    uart_update();
    return g_uart_cap - (size_t)g_uart_queue;
}

typedef struct {
    link_parser_t parser;
    uint8_t fb[LINK_H][LINK_W];         /* what the display shows */
    uint8_t (*grabbed)[LINK_W];         /* what the grabber produced */
    int  frames, rows, bad_rows, max_err, infos;
    int  last_delta;
    long rows_bytes;
} display_t;

static display_t g_disp;

static void display_packet(void *ctx, uint8_t type, uint8_t a, uint8_t b, const uint8_t *p, int len)
{
    display_t *d = (display_t *)ctx;
    (void)a;
    if (type == LINK_T_ROW && b < LINK_H) {
        if (!link_row_decode(p, len, d->fb[b])) { d->bad_rows++; return; }
        d->rows++;
        int lim = p[0] == LINK_CODEC_NL ? (p[1] & 15) : 0;
        for (int x = 0; x < LINK_W; ++x) {
            int e = abs((int)d->fb[b][x] - (int)d->grabbed[b][x]);
            if (e > d->max_err) d->max_err = e;
            if (e > lim) { d->bad_rows++; break; }
        }
    } else if (type == LINK_T_INFO && len >= LINK_INFO_LEN) {
        d->infos++;
        d->last_delta = p[LINK_INFO_DELTA] == 0xff ? -1 : p[LINK_INFO_DELTA];
    }
}

static void port_write(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    /* Like uart_write_bytes(): blocks while the TX buffer is full, taking
     * writes larger than the buffer in pieces. */
    for (size_t done = 0; done < n; ) {
        uart_update();
        double room = (double)g_uart_cap - g_uart_queue;
        if (room < 1.0) {
            idle_us(64.0 / g_uart_rate);
            continue;
        }
        size_t piece = n - done;
        if ((double)piece > room) piece = (size_t)room;
        g_uart_queue += (double)piece;
        done += piece;
    }
    g_uart_total += (long)n;
    /* The display's work is not the receiver's: keep it off the simulated clock. */
    double a = host_now_us();
    link_parser_feed(&g_disp.parser, data, (int)n, display_packet, &g_disp);
    g_exit_us += host_now_us() - a;
}

/* ------------------------------------------------------------ metrics */

typedef struct {
    int    marker_row;      /* row of the white marker line (expected ~84) */
    double diag_col;        /* column of the black diagonal on row 70 */
    double noise;           /* std of a flat bar (bar 6, rows 8..45) */
    double edge_px;         /* 10..90 % width of the bar 5 / bar 6 edge */
} metrics_t;

static metrics_t measure(const uint8_t img[GRAB_H][GRAB_W])
{
    metrics_t m = { -1, -1.0, 0.0, 0.0 };
    double best = -1.0;
    for (int y = 60; y < 110; ++y) {
        double s = 0.0;
        for (int x = 16; x < 48; ++x) s += img[y][x];
        if (s > best) { best = s; m.marker_row = y; }
    }
    /* diagonal x = y crosses the grey ramp: on row 70 (y = 0.417) the dip near column 93 */
    {
        int y = 70, lo = 60, hi = 130, arg = lo;
        for (int x = lo; x < hi; ++x) if (img[y][x] < img[y][arg]) arg = x;
        m.diag_col = arg;
    }
    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (int y = 8; y < 46; ++y)
        for (int x = 172; x < 193; ++x) { sum += img[y][x]; sum2 += (double)img[y][x] * img[y][x]; n++; }
    double mean = sum / n;
    m.noise = sqrt(sum2 / n - mean * mean);
    /* edge between bar 5 (5/7) and bar 6 (6/7) at column ~168: average profile */
    double prof[20] = { 0 };
    for (int y = 8; y < 46; ++y)
        for (int k = 0; k < 20; ++k) prof[k] += img[y][158 + k];
    double lo_v = prof[0], hi_v = prof[19];
    double t10 = lo_v + 0.1 * (hi_v - lo_v), t90 = lo_v + 0.9 * (hi_v - lo_v);
    double x10 = -1, x90 = -1;
    for (int k = 1; k < 20; ++k) {
        if (x10 < 0 && prof[k] >= t10) x10 = k - 1 + (t10 - prof[k - 1]) / (prof[k] - prof[k - 1] + 1e-9);
        if (x90 < 0 && prof[k] >= t90) x90 = k - 1 + (t90 - prof[k - 1]) / (prof[k] - prof[k - 1] + 1e-9);
    }
    m.edge_px = (x10 >= 0 && x90 >= 0) ? x90 - x10 : -1.0;
    return m;
}

static void save_pgm(const char *prefix, int n, const uint8_t img[GRAB_H][GRAB_W])
{
    char name[256];
    snprintf(name, sizeof(name), "%s_%d.pgm", prefix, n);
    FILE *fp = fopen(name, "wb");
    if (!fp) return;
    fprintf(fp, "P5\n%d %d\n255\n", GRAB_W, GRAB_H);
    fwrite(img, 1, (size_t)GRAB_W * GRAB_H, fp);
    fclose(fp);
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int frames = 3, seed = 1, mode = LINK_MODE_AUTO;
    double gap_ms = 0.0, baud = 4e6;
    bool probe = false, encode = true;
    const char *prefix = "grab_sim";
    for (int a = 1; a < argc; ++a) {
        const char *o = argv[a];
        const char *v = a + 1 < argc ? argv[a + 1] : "";
        if (!strcmp(o, "--std")) { g_pal = strcmp(v, "ntsc") != 0; ++a; }
        else if (!strcmp(o, "--frames")) { frames = atoi(v); ++a; }
        else if (!strcmp(o, "--noise")) { g_noise = atof(v); ++a; }
        else if (!strcmp(o, "--cfo")) { g_cfo = atof(v) * 1e6; ++a; }
        else if (!strcmp(o, "--ppm")) { g_ppm = atof(v); ++a; }
        else if (!strcmp(o, "--gap-ms")) { gap_ms = atof(v); ++a; }
        else if (!strcmp(o, "--slowdown")) { g_slowdown = atof(v); ++a; }
        else if (!strcmp(o, "--enc-slowdown")) { g_enc_slowdown = atof(v); ++a; }
        else if (!strcmp(o, "--cpu")) { g_cpu_model = strcmp(v, "host") != 0; ++a; }
        else if (!strcmp(o, "--smooth")) { grab_set_smoothing(atoi(v) != 0); ++a; }
        else if (!strcmp(o, "--out")) { prefix = v; ++a; }
        else if (!strcmp(o, "--seed")) { seed = atoi(v); ++a; }
        else if (!strcmp(o, "--mode")) { mode = !strcmp(v, "raw") ? LINK_MODE_RAW : !strcmp(v, "fine") ? LINK_MODE_FINE : LINK_MODE_AUTO; ++a; }
        else if (!strcmp(o, "--baud")) { baud = atof(v); ++a; }
        else if (!strcmp(o, "--encode")) { encode = atoi(v) != 0; ++a; }
        else if (!strcmp(o, "--nosignal")) { g_nosignal = true; }
        else if (!strcmp(o, "--trace-late")) { g_trace_late = true; }
        else if (!strcmp(o, "--probe")) { probe = true; }
        else if (!strcmp(o, "--replay")) {
            FILE *rf = fopen(v, "rb");
            if (!rf) { perror(v); return 2; }
            fseek(rf, 0, SEEK_END);
            g_replay_len = ftell(rf);
            fseek(rf, 0, SEEK_SET);
            g_replay = malloc((size_t)g_replay_len);
            if (fread(g_replay, 1, (size_t)g_replay_len, rf) != (size_t)g_replay_len) return 2;
            fclose(rf);
            ++a;
        } else { fprintf(stderr, "unknown option %s\n", o); return 2; }
    }
    g_line_us = (g_pal ? 64.0 : 63.5556) * (1.0 + g_ppm * 1e-6);
    g_lines = g_pal ? 625 : 525;
    srand((unsigned)seed);
    tables_init();
    g_rng += (uint32_t)seed * 2654435761u;
    advance_to((int64_t)(g_time_us * 40.0));
    grab_init();

    if (probe) {
        grab_info_t info;
        int64_t t0 = esp_timer_get_time();
        bool found = grab_probe(60, &info);
        printf("probe: %s after %.1f ms (windows=%d, %d us each, power=%.1f, carrier %+.2f MHz, error=%s)\n",
               found ? "VIDEO" : "none", (esp_timer_get_time() - t0) / 1000.0, info.windows,
               info.windows ? info.window_us / info.windows : 0, info.power, info.carrier_mhz,
               info.error ? info.error : "none");
        return found == !g_nosignal ? 0 : 1;
    }

    static uint8_t img[GRAB_H][GRAB_W];
    g_uart_rate = baud / 10.0 / 1e6;
    g_uart_t = g_time_us;
    link_parser_init(&g_disp.parser);
    g_disp.grabbed = img;
    link_tx_port_t port = { port_free, port_write, NULL };
    link_tx_init(&port, (uint32_t)baud);
    link_tx_set_mode((uint8_t)mode);
    link_tx_status_t st = { 5843, 52, 5, 0, false, true };

    int complete = 0, good = 0, marker_first = -1, marker_moves = 0;
    double noise_sum = 0.0, edge_sum = 0.0, diag_first = -1.0, diag_max_dev = 0.0;
    int measured = 0, locked = 0;
    double locked_t0 = 0.0, cpu_sum = 0.0, enc_sum = 0.0, idle_sum = 0.0;
    long rows_sum = 0, bytes_sum = 0, idle_rows = 0, after_rows = 0;
    for (int n = 0; n < frames; ++n) {
        memset(img, 0, sizeof(img));
        grab_info_t info;
        if (encode) link_tx_frame_begin((uint8_t)n);
        int rows = grab_frame(&img[0][0], &info, 400, encode ? link_tx_on_row : NULL,
                              encode ? sim_on_idle : NULL, NULL);
        link_tx_stats_t ts;
        memset(&ts, 0, sizeof(ts));
        if (encode) {
            link_tx_frame_end(rows, &info, &st);
            link_tx_last_stats(&ts);
        }
        printf("frame %d: rows=%d %s T=%.4fus fields=%d acq=%dms grab=%dms late=%d nosync=%d wide=%d big=%d "
               "jit=%.0fns v=%d/%d/%d skip=%d sub=%d gave=%d tb=%d/%d%s win=%dus cpu=%dus/row idle=%d/%dus | d=%d %ub enc=%uus "
               "rows idle/after=%d/%d flush=%uus err=%s",
               n, rows, info.pal ? "PAL" : "NTSC", info.line_us, info.fields, info.acquire_ms, info.grab_ms,
               info.late, info.nosync, info.wide_hits, info.big_err, info.jitter_ns,
               info.vchecks, info.vslips, info.vmisses, info.skipped, info.substituted, info.given_up, info.tb_lead_min_ns, info.tb_lead_max_ns, info.tb_bad ? "!" : "",
               info.windows ? info.window_us / info.windows : 0, rows ? info.cpu_us / rows : 0,
               info.idle_calls, info.idle_us, ts.delta, (unsigned)ts.bytes, (unsigned)ts.encode_us,
               ts.rows_idle, ts.rows_after, (unsigned)ts.flush_us, info.error ? info.error : "none");
        if (rows > 0 && !info.error && info.acquire_ms == 0) {
            if (!locked) locked_t0 = g_time_us - info.grab_ms * 1000.0;
            locked++;
            bytes_sum += ts.bytes;
            idle_rows += ts.rows_idle;
            after_rows += ts.rows_after;
            enc_sum += ts.encode_us;
            idle_sum += info.idle_us;
        }
        cpu_sum += info.cpu_us;
        rows_sum += rows;
        if (rows == GRAB_H && !g_replay) {
            metrics_t m = measure(img);
            printf(" | marker=%d diag=%.0f noise=%.1f edge=%.2fpx", m.marker_row, m.diag_col, m.noise, m.edge_px);
            if (marker_first < 0) { marker_first = m.marker_row; diag_first = m.diag_col; }
            else {
                if (m.marker_row != marker_first) marker_moves++;
                if (fabs(m.diag_col - diag_first) > diag_max_dev) diag_max_dev = fabs(m.diag_col - diag_first);
            }
            noise_sum += m.noise;
            edge_sum += m.edge_px;
            measured++;
        }
        if (rows < GRAB_H) printf(" | missing row %d", info.missing_row);
        printf("\n");
#ifdef GRAB_DEBUG
        { void grab_debug_dump(void); grab_debug_dump(); }
#endif
        if (g_trace_late && info.late) {
            grab_trace_t tr[GRAB_TRACE_LEN];
            int nt = grab_trace(tr, GRAB_TRACE_LEN);
            printf("   trace (line:err result/row):");
            for (int k = 0; k < nt; ++k) printf(" %ld:%d%c/%d", (long)tr[k].line, tr[k].err, tr[k].result, tr[k].row);
            printf("\n");
        }
        save_pgm(prefix, n, img);
        if (rows == GRAB_H) complete++;
        if (rows > 0 && !info.error) good++;          /* complete, or a few rows given up */
        if (gap_ms > 0.0) idle_ms(gap_ms);
        g_exit_us = host_now_us();              /* the bench's own reporting is not receiver time */
    }
    if (measured)
        printf("summary: complete %d/%d (good %d), marker moved in %d frames, diagonal max drift %.0f px, "
               "noise %.2f, edge %.2f px\n", complete, frames, good, marker_moves, diag_max_dev,
               noise_sum / measured, edge_sum / measured);
    if (encode)
        printf("display: %d rows decoded, %d bad, max error %d, %d infos, %u crc errors\n", g_disp.rows,
               g_disp.bad_rows, g_disp.max_err, g_disp.infos, (unsigned)g_disp.parser.crc_errors);
    if (locked > 1) {
        double secs = (g_time_us - locked_t0) / 1e6;
        printf("rate: %.1f fps over %d locked frames, %.0f bytes/frame, cpu %.0f us/row, encode %.0f us/row "
               "(%.0f%% of rows while waiting), idle work %.1f ms/frame\n",
               locked / secs, locked, (double)bytes_sum / locked, rows_sum ? cpu_sum / rows_sum : 0.0,
               (idle_rows + after_rows) ? enc_sum / (idle_rows + after_rows) : 0.0,
               (idle_rows + after_rows) ? 100.0 * idle_rows / (idle_rows + after_rows) : 0.0,
               idle_sum / locked / 1000.0);
    }
    return good == frames && !g_disp.bad_rows ? 0 : 1;
}
