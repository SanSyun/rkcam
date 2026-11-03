#ifndef PIPELINE_STATS_H
#define PIPELINE_STATS_H

#include <stdint.h>

/*
 * 板端运行统计：默认 1 Hz 打印一行帧率/延迟，退出时打印分位数汇总。
 *
 * 延迟口径：样本 = clock_gettime(CLOCK_MONOTONIC) - V4L2 buf.timestamp。
 * V4L2 时间戳同样基于 CLOCK_MONOTONIC，因此不需要和 PC 对时。
 * 如果驱动给的时间戳不是单调时钟，首次采样会检测出来并关闭延迟统计。
 *
 * 统计是纯旁路：不改码流、不改时间戳语义，只读计数器。
 */

#define PIPE_STAT_LAT_SAMPLES  300      /* 延迟窗口样本数，30 fps 下约 10 秒 */
#define PIPE_STAT_INTERVAL_MS  1000     /* 打印周期 */
#define PIPE_STAT_CLOCK_LIMIT_US (10ULL * 1000ULL * 1000ULL)  /* 时间戳合理性阈值 */

/* pipeline_collect_stats() 填好的累计计数快照 */
typedef struct {
    unsigned long long captured;       /* 已采集帧数 */
    unsigned long long encoded;        /* 已编码帧数 */
    unsigned long long saved_packets;  /* 已落盘包数 */
    unsigned long long saved_bytes;    /* 已落盘字节数 */
    unsigned long long sent;           /* RTMP 已发送帧数 */
    unsigned long long dropped;        /* RTMP 丢帧数 */
    unsigned long long sent_bytes;     /* RTMP 已发送字节数 */
    int file_on;                       /* 落盘是否仍有效（-or 出错会降级） */
    int rtmp_on;                       /* 推流是否配置 */
    unsigned int frame_ring_pct;       /* frame_rb 占用百分比 */
    unsigned int packet_ring_pct;      /* packet_rb 占用百分比 */
} pipeline_stats_counters_t;

typedef struct {
    uint32_t count;                    /* 有效样本数，最多 PIPE_STAT_LAT_SAMPLES */
    uint32_t next;                     /* 下一个写入位置 */
    uint32_t buf[PIPE_STAT_LAT_SAMPLES];
} pipeline_stats_lat_t;

typedef struct {
    int64_t  start_us;
    int64_t  last_print_us;

    unsigned long long prev_captured;
    unsigned long long prev_encoded;
    unsigned long long prev_saved_packets;
    unsigned long long prev_saved_bytes;
    unsigned long long prev_sent;
    unsigned long long prev_dropped;
    unsigned long long prev_sent_bytes;

    int checked;                       /* 是否已做过时间戳基准校验 */
    int clock_bad;                     /* 时间戳不是单调时钟，关闭延迟统计 */

    pipeline_stats_lat_t lat_cap2enc;  /* 采集 -> 编码完成 */
    pipeline_stats_lat_t lat_cap2file; /* 采集 -> 落盘完成 */
    pipeline_stats_lat_t lat_cap2mux;  /* 采集 -> RTMP 发送完成 */
} pipeline_stats_t;

/* 初始化：清空窗口、记录起始时间 */
void pipeline_stats_init(pipeline_stats_t *st);

/* 延迟采样。pts_us 为 pipeline 里透传的 V4L2 时间戳 */
void pipeline_stats_on_encode(pipeline_stats_t *st, uint64_t pts_us);
void pipeline_stats_on_file(pipeline_stats_t *st, uint64_t pts_us);
void pipeline_stats_on_mux(pipeline_stats_t *st, uint64_t pts_us);

/* 每秒最多打印一次；到点返回 1。由消费线程周期调用 */
int  pipeline_stats_tick(pipeline_stats_t *st,
                         const pipeline_stats_counters_t *c);

/* 退出汇总：累计量 + 各延迟口径的 p50/p95/p99/max */
void pipeline_stats_summary(pipeline_stats_t *st,
                            const pipeline_stats_counters_t *c);

#endif
