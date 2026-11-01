#ifndef U_FFMPEG_H
#define U_FFMPEG_H

#include <stddef.h>
#include <stdint.h>

#define U_FFMPEG_URL_MAX 512

/* 前置声明，避免业务层直接依赖 FFmpeg 头文件 */
struct AVFormatContext;
struct AVStream;
struct AVPacket;

typedef struct {
    struct AVFormatContext *fmt;   /* FLV/RTMP 输出上下文 */
    struct AVStream        *st;    /* H.264 视频流 */
    struct AVPacket        *pkt;   /* 复用的发送包 */

    char url[U_FFMPEG_URL_MAX];
    int  width;
    int  height;
    int  fps;

    int  configured;               /* 本地初始化完成 */
    int  net_inited;               /* avformat_network_init 已调用 */
    int  connected;                /* avio_open + write_header 成功 */
    int  wait_key;                 /* 重连后等下一个 IDR 再发 */
    int64_t last_conn_ms;          /* 单调时钟，用于重连退避 */

    uint8_t *extradata;            /* avcC，重连时复用 */
    size_t   extradata_len;

    int64_t  start_pts_us;         /* 首帧 PTS，用于时间戳归零 */
    int64_t  last_dts_ms;          /* 上次发送的 dts，保证单调 */
    unsigned long long frames;     /* 已发送帧数 */
    unsigned long long dropped;    /* 丢帧数 */
} u_ffmpeg_t;

/*
 * 初始化封装环境：解析 SPS/PPS 生成 avcC extradata，创建 AVPacket。
 * 不建立网络连接，首次连接/重连由 u_ffmpeg_push 负责。
 * 返回 0 成功，-1 参数/内存/SPS-PPS 错误。
 */
int  u_ffmpeg_init(u_ffmpeg_t *ff, const char *url,
                   int width, int height, int fps,
                   const void *sps_pps, size_t sps_pps_len);

/*
 * 封装并发送一帧 Annex-B H.264：
 * 把起始码替换成 4 字节大端长度（AVCC），填 PTS/DTS/关键帧标志，
 * 内部调用 av_interleaved_write_frame 完成发送。
 * 返回 1 已发送；0 跳过/丢帧（未连接、等 IDR、无 VCL）；-1 封装失败。
 */
int  u_ffmpeg_mux(u_ffmpeg_t *ff, const void *data, size_t len,
                  uint64_t pts_us);

/*
 * 连接管理：未连接时尝试 avio_open + write_header，失败按 2s 退避。
 * 返回 1 已连接可发送；0 未连接（调用方本轮丢帧）。
 */
int  u_ffmpeg_push(u_ffmpeg_t *ff);

/* 收尾：写 trailer、关闭 RTMP、释放 extradata/AVPacket */
void u_ffmpeg_deinit(u_ffmpeg_t *ff);

#endif