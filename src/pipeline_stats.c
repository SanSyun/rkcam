#include "pipeline_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t mono_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void lat_add(pipeline_stats_lat_t *l, uint64_t us)
{
    if (us > 0xffffffffULL) {
        us = 0xffffffffULL;
    }

    l->buf[l->next] = (uint32_t)us;
    l->next = (l->next + 1) % PIPE_STAT_LAT_SAMPLES;
    if (l->count < PIPE_STAT_LAT_SAMPLES) {
        l->count++;
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

typedef struct {
    uint32_t n;
    uint32_t p50;
    uint32_t p95;
    uint32_t p99;
    uint32_t max;
} lat_summary_t;

static lat_summary_t lat_summary(const pipeline_stats_lat_t *l)
{
    lat_summary_t s;
    uint32_t tmp[PIPE_STAT_LAT_SAMPLES];
    uint32_t n = l->count;
    uint32_t i;

    memset(&s, 0, sizeof(s));
    if (n == 0) {
        return s;
    }
    if (n > PIPE_STAT_LAT_SAMPLES) {
        n = PIPE_STAT_LAT_SAMPLES;
    }

    memcpy(tmp, l->buf, sizeof(tmp[0]) * n);
    qsort(tmp, n, sizeof(tmp[0]), cmp_u32);

    s.n = n;
    s.max = tmp[n - 1];

    i = (uint32_t)(((uint64_t)n * 50) / 100);
    if (i >= n) {
        i = n - 1;
    }
    s.p50 = tmp[i];

    i = (uint32_t)(((uint64_t)n * 95) / 100);
    if (i >= n) {
        i = n - 1;
    }
    s.p95 = tmp[i];

    i = (uint32_t)(((uint64_t)n * 99) / 100);
    if (i >= n) {
        i = n - 1;
    }
    s.p99 = tmp[i];

    return s;
}

/*
 * 时间戳基准校验：V4L2 默认给 CLOCK_MONOTONIC，和 clock_gettime 同源。
 * 如果驱动用的是别的时钟基准，第一次采样就会发现，之后关闭延迟统计。
 */
static int stats_latency_ok(pipeline_stats_t *st, uint64_t pts_us, uint64_t now_us)
{
    if (!st->checked) {
        st->checked = 1;
        if (now_us < pts_us || now_us - pts_us > PIPE_STAT_CLOCK_LIMIT_US) {
            st->clock_bad = 1;
            fprintf(stderr,
                    "[stats] V4L2 timestamp is not CLOCK_MONOTONIC based "
                    "(pts=%llu now=%llu), latency disabled\n",
                    (unsigned long long)pts_us,
                    (unsigned long long)now_us);
        }
    }

    return !st->clock_bad;
}

static void lat_sample(pipeline_stats_t *st, pipeline_stats_lat_t *lat,
                       uint64_t pts_us)
{
    uint64_t now = mono_now_us();

    if (!stats_latency_ok(st, pts_us, now)) {
        return;
    }
    if (now < pts_us || now - pts_us > PIPE_STAT_CLOCK_LIMIT_US) {
        return;   /* 个别帧时间戳异常，丢弃该样本而不是污染窗口 */
    }

    lat_add(lat, now - pts_us);
}

void pipeline_stats_on_encode(pipeline_stats_t *st, uint64_t pts_us)
{
    if (st == NULL) {
        return;
    }
    lat_sample(st, &st->lat_cap2enc, pts_us);
}

void pipeline_stats_on_file(pipeline_stats_t *st, uint64_t pts_us)
{
    if (st == NULL) {
        return;
    }
    lat_sample(st, &st->lat_cap2file, pts_us);
}

void pipeline_stats_on_mux(pipeline_stats_t *st, uint64_t pts_us)
{
    if (st == NULL) {
        return;
    }
    lat_sample(st, &st->lat_cap2mux, pts_us);
}

static double rate(unsigned long long cur, unsigned long long prev, double secs)
{
    if (secs <= 0.0 || cur < prev) {
        return 0.0;
    }

    return (double)(cur - prev) / secs;
}

/* 安全地 sprintf 到定长缓冲，截断不越界 */
static void line_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (buf == NULL || off == NULL || *off >= cap) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);

    if (n > 0) {
        *off += (size_t)n;
        if (*off >= cap) {
            *off = cap - 1;
        }
    }
}

void pipeline_stats_init(pipeline_stats_t *st)
{
    if (st == NULL) {
        return;
    }

    memset(st, 0, sizeof(*st));
    st->start_us = (int64_t)mono_now_us();
    st->last_print_us = st->start_us;
}

int pipeline_stats_tick(pipeline_stats_t *st, const pipeline_stats_counters_t *c)
{
    uint64_t now;
    double secs;
    char lat[240];
    char line[512];
    size_t off = 0;
    size_t lat_off = 0;

    if (st == NULL || c == NULL) {
        return 0;
    }

    now = mono_now_us();
    if (now < (uint64_t)st->last_print_us) {
        return 0;
    }
    if (now - (uint64_t)st->last_print_us <
        (uint64_t)PIPE_STAT_INTERVAL_MS * 1000ULL) {
        return 0;
    }

    secs = (double)(now - (uint64_t)st->last_print_us) / 1000000.0;

    lat[0] = '\0';
    if (st->clock_bad) {
        line_append(lat, sizeof(lat), &lat_off, " lat=off(clock)");
    } else {
        if (c->rtmp_on && st->lat_cap2mux.count > 0) {
            lat_summary_t s = lat_summary(&st->lat_cap2mux);

            line_append(lat, sizeof(lat), &lat_off,
                        " lat_mux p50=%.1fms p95=%.1fms max=%.1fms",
                        (double)s.p50 / 1000.0,
                        (double)s.p95 / 1000.0,
                        (double)s.max / 1000.0);
        }
        if (c->file_on && st->lat_cap2file.count > 0) {
            lat_summary_t s = lat_summary(&st->lat_cap2file);

            line_append(lat, sizeof(lat), &lat_off,
                        " lat_file p50=%.1fms p95=%.1fms max=%.1fms",
                        (double)s.p50 / 1000.0,
                        (double)s.p95 / 1000.0,
                        (double)s.max / 1000.0);
        }
    }

    line_append(line, sizeof(line), &off,
                "[stats] %llus cap=%.1f enc=%.1f sent=%.1f file=%.1f fps "
                "drop=%llu rb_f=%u%% rb_p=%u%%",
                (unsigned long long)((now - (uint64_t)st->start_us) / 1000000ULL),
                rate(c->captured, st->prev_captured, secs),
                rate(c->encoded, st->prev_encoded, secs),
                rate(c->sent, st->prev_sent, secs),
                rate(c->saved_packets, st->prev_saved_packets, secs),
                c->dropped,
                c->frame_ring_pct,
                c->packet_ring_pct);

    if (c->rtmp_on) {
        line_append(line, sizeof(line), &off, " br=%.2fMbps",
                    rate(c->sent_bytes, st->prev_sent_bytes, secs) * 8.0 / 1000000.0);
    }
    if (c->file_on) {
        line_append(line, sizeof(line), &off, " fw=%.2fMB/s",
                    rate(c->saved_bytes, st->prev_saved_bytes, secs) / 1000000.0);
    }

    line_append(line, sizeof(line), &off, "%s\n", lat);
    fputs(line, stderr);

    st->last_print_us = (int64_t)now;
    st->prev_captured = c->captured;
    st->prev_encoded = c->encoded;
    st->prev_saved_packets = c->saved_packets;
    st->prev_saved_bytes = c->saved_bytes;
    st->prev_sent = c->sent;
    st->prev_dropped = c->dropped;
    st->prev_sent_bytes = c->sent_bytes;

    return 1;
}

static void print_lat(const char *name, const pipeline_stats_lat_t *l)
{
    lat_summary_t s = lat_summary(l);

    if (s.n == 0) {
        return;
    }

    fprintf(stderr,
            "[stats] lat_%-8s p50=%.1fms p95=%.1fms p99=%.1fms "
            "max=%.1fms n=%u\n",
            name,
            (double)s.p50 / 1000.0,
            (double)s.p95 / 1000.0,
            (double)s.p99 / 1000.0,
            (double)s.max / 1000.0,
            s.n);
}

void pipeline_stats_summary(pipeline_stats_t *st, const pipeline_stats_counters_t *c)
{
    uint64_t now;
    double secs;

    if (st == NULL || c == NULL) {
        return;
    }

    now = mono_now_us();
    secs = (now > (uint64_t)st->start_us)
         ? (double)(now - (uint64_t)st->start_us) / 1000000.0
         : 0.0;

    fprintf(stderr,
            "[stats] total %.1fs cap=%llu enc=%llu saved=%llu sent=%llu "
            "drop=%llu file=%.2fMB rtmp=%.2fMB\n",
            secs,
            c->captured,
            c->encoded,
            c->saved_packets,
            c->sent,
            c->dropped,
            (double)c->saved_bytes / (1024.0 * 1024.0),
            (double)c->sent_bytes / (1024.0 * 1024.0));

    if (st->clock_bad) {
        fprintf(stderr,
                "[stats] latency disabled: V4L2 timestamp is not monotonic\n");
        return;
    }

    print_lat("cap2enc", &st->lat_cap2enc);
    print_lat("cap2file", &st->lat_cap2file);
    print_lat("cap2mux", &st->lat_cap2mux);
}
