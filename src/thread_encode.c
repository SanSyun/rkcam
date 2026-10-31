#include "thread_encode.h"
#include "pipeline.h"
#include "u_dma.h"

#include <stdio.h>
#include <string.h>

/*
 * 取完当前可用的码流包并写入 packet_rb。
 * wait_for_eos=0：取到第一包后不再等待；=1：一直等到 EOS 包。
 * 返回写入的包数，-1 表示写环失败。eos_seen 返回是否看到 EOS。
 */
static int drain_packets(pipeline_t *p, uint64_t pts_us, uint32_t seq,
                         int wait_for_eos, int *eos_seen)
{
    int packets = 0;
    int timeout = PIPELINE_WRITE_TIMEOUT_MS;

    if (eos_seen != NULL) {
        *eos_seen = 0;
    }

    while (!p->fatal) {
        MppPacket pkt = NULL;
        size_t len;
        const void *data;
        uint32_t flags = 0;
        int eos;

        if (mpp_encoder_get_packet(&p->enc, &pkt, timeout) != 0) {
            break;
        }

        len = mpp_packet_get_length(pkt);
        data = mpp_packet_get_pos(pkt);
        eos = mpp_packet_get_eos(pkt) ? 1 : 0;
        if (eos) {
            flags |= PKT_FLAG_EOS;
        }

        if (len > 0) {
            if (pipeline_write_packet(p, data, len, pts_us, seq, flags,
                                      PIPELINE_WRITE_TIMEOUT_MS) != 0) {
                fprintf(stderr, "[encode] write packet failed, len=%zu\n", len);
                mpp_packet_deinit(&pkt);
                return -1;
            }
            packets++;
        }

        mpp_packet_deinit(&pkt);

        if (eos) {
            if (eos_seen != NULL) {
                *eos_seen = 1;
            }
            return packets;
        }

        /* 第一包已经拿到，后续只取当前已经就绪的包，不再空等 */
        timeout = wait_for_eos ? PIPELINE_WRITE_TIMEOUT_MS : 0;
    }

    return packets;
}

void *thread_encode(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    unsigned char *dst;
    int dst_fd;
    const unsigned char *header = NULL;
    size_t header_len = 0;

    if (p == NULL || p->enc_buf == NULL) {
        fprintf(stderr, "[encode] invalid pipeline\n");
        return NULL;
    }

    dst = mpp_buffer_get_ptr(p->enc_buf);
    dst_fd = mpp_buffer_get_fd(p->enc_buf);
    if (dst == NULL || dst_fd < 0) {
        fprintf(stderr, "[encode] get MPP input buffer ptr/fd failed\n");
        pipeline_abort(p);
        p->encode_done = 1;
        return NULL;
    }

    /* SPS/PPS 作为第一条包写入码流环，消费线程统一写盘 */
    if (mpp_encoder_get_header(&p->enc, &header, &header_len) != 0 ||
        header == NULL || header_len == 0) {
        fprintf(stderr, "[encode] get SPS/PPS header failed\n");
        pipeline_abort(p);
        p->encode_done = 1;
        return NULL;
    }
    if (pipeline_write_packet(p, header, header_len, 0, 0, PKT_FLAG_HEADER,
                              PIPELINE_WRITE_TIMEOUT_MS) != 0) {
        fprintf(stderr, "[encode] write SPS/PPS header failed\n");
        pipeline_abort(p);
        p->encode_done = 1;
        return NULL;
    }

    while (!p->fatal) {
        size_t len = 0;
        uint64_t pts_us = 0;
        uint32_t seq = 0;
        int eos_seen = 0;
        int got;
        int ret;

        if (p->capture_done && rb_is_empty(&p->frame_rb)) {
            break;
        }

        if (dma_buf_cpu_begin(dst_fd, 1) != 0) {
            fprintf(stderr, "[encode] dma_buf_cpu_begin failed\n");
            pipeline_abort(p);
            break;
        }

        ret = pipeline_read_frame(p, dst, ENC_FRAME_SIZE, &len, &pts_us, &seq,
                                  PIPELINE_READ_TIMEOUT_MS);

        if (dma_buf_cpu_end(dst_fd, 1) != 0) {
            fprintf(stderr, "[encode] dma_buf_cpu_end failed\n");
            pipeline_abort(p);
            break;
        }

        if (ret == 0) {
            continue;   /* 读超时，回去检查 capture_done/fatal */
        }
        if (ret < 0) {
            fprintf(stderr, "[encode] read frame failed\n");
            pipeline_abort(p);
            break;
        }
        if (len != ENC_FRAME_SIZE) {
            fprintf(stderr, "[encode] bad frame size %zu, expect %zu\n",
                    len, ENC_FRAME_SIZE);
            pipeline_abort(p);
            break;
        }

        //MPP编码
        if (mpp_encoder_encode(&p->enc, p->enc_buf) != 0) {
            fprintf(stderr, "[encode] mpp_encoder_encode failed at seq=%u\n", seq);
            pipeline_abort(p);
            break;
        }

        /* 必须取到这一帧的包后才能覆盖 enc_buf */
        got = drain_packets(p, pts_us, seq, 0, &eos_seen);
        if (got <= 0) {
            fprintf(stderr, "[encode] no packet for frame seq=%u\n", seq);
            pipeline_abort(p);
            break;
        }

        p->encoded++;
    }

    /* 排空 rb_frame 后发 EOS，把编码器内部缓存的帧也吐到 packet_rb */
    if (!p->fatal) {
        int eos_seen = 0;
        int got;

        if (mpp_encoder_send_eos(&p->enc) != 0) {
            fprintf(stderr, "[encode] mpp_encoder_send_eos failed\n");
        } else {
            got = drain_packets(p, 0, (uint32_t)p->encoded, 1, &eos_seen);
            if (!eos_seen) {
                fprintf(stderr, "[encode] EOS not seen, packets=%d\n", got);
            }
        }
    }

    p->encode_done = 1;
    fprintf(stderr, "[encode] done, encoded=%llu\n", p->encoded);
    return NULL;
}