#ifndef PIPELINE_H
#define PIPELINE_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include "v4l2_capture.h"
#include "mpp_encode.h"
#include "u_ffmpeg.h"
#include "ringbuffer.h"

/* 编码分辨率：RGA 输出 / MPP 输入 */
#define ENC_WIDTH       1920
#define ENC_HEIGHT      1080
#define ENC_FRAME_SIZE  ((size_t)ENC_WIDTH * ENC_HEIGHT * 3 / 2)

/* 环形缓冲区容量与超时 */
#define FRAME_RING_FRAMES          4
#define FRAME_RING_SIZE            (FRAME_RING_FRAMES * (sizeof(frame_record_t) + ENC_FRAME_SIZE))
#define PACKET_RING_SIZE           (8u * 1024u * 1024u)
#define PACKET_MAX_SIZE            (4u * 1024u * 1024u)

#define PIPELINE_WRITE_TIMEOUT_MS  1000
#define PIPELINE_READ_TIMEOUT_MS   100
#define PIPELINE_PAYLOAD_TIMEOUT_MS 1000

#define PIPELINE_OUTPUT_PATH_MAX   512

#define FRAME_MAGIC   0x314D5246u   /* "FRM1" */
#define PACKET_MAGIC  0x31544B50u   /* "PKT1" */

#define PKT_FLAG_HEADER 0x01u
#define PKT_FLAG_EOS    0x02u
#define PKT_FLAG_KEY    0x04u

typedef struct {
    uint32_t magic;
    uint32_t size;        /* NV12 字节数 */
    uint32_t width;
    uint32_t height;
    uint64_t pts_us;      /* V4L2 时间戳，后续做 MP4/网络需要 */
    uint32_t seq;
    uint32_t reserved;
} frame_record_t;

typedef struct {
    uint32_t magic;
    uint32_t size;        /* H.264 包字节数 */
    uint64_t pts_us;
    uint32_t seq;
    uint32_t flags;       /* HEADER / EOS / 预留 KEY */
    uint32_t reserved;
} packet_record_t;

typedef struct {
    struct v4l2_dev *dev;
    MppEncoder       enc;

    MppBuffer rga_buf;    /* RGA 输出用 dma-buf */
    MppBuffer enc_buf;    /* MPP 输入用 dma-buf */

    struct ringbuffer frame_rb;
    struct ringbuffer packet_rb;

    int width;
    int height;
    int fps;
    char output_path[PIPELINE_OUTPUT_PATH_MAX];  /* 空串表示只推流、不落盘 */

    u_ffmpeg_t ff;        /* RTMP 推流上下文，落盘模式下全 0 */

    volatile int capture_done;
    volatile int encode_done;
    volatile int fatal;

    unsigned long long captured;
    unsigned long long encoded;
    unsigned long long saved_packets;
    unsigned long long saved_bytes;
} pipeline_t;

/* 信号处理函数里只能置这个标志，不能调用 pthread/rb_stop */
extern volatile sig_atomic_t g_pipeline_stop;

/* 任一环节出错：置 fatal 并唤醒阻塞在两个环上的线程 */
void pipeline_abort(pipeline_t *p);

/* 写入/读取一条完整记录。返回 1 成功，0 读超时，-1 数据损坏或写失败 */
int pipeline_write_frame(pipeline_t *p, const void *data, size_t len,
                         uint64_t pts_us, uint32_t seq, int timeout_ms);
int pipeline_read_frame(pipeline_t *p, void *dst, size_t cap, size_t *out_len,
                        uint64_t *pts_us, uint32_t *seq, int timeout_ms);

int pipeline_write_packet(pipeline_t *p, const void *data, size_t len,
                          uint64_t pts_us, uint32_t seq, uint32_t flags, int timeout_ms);
int pipeline_read_packet(pipeline_t *p, void *dst, size_t cap, size_t *out_len,
                         uint64_t *pts_us, uint32_t *seq, uint32_t *flags, int timeout_ms);

#endif