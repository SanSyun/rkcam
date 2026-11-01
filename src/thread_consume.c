#include "thread_consume.h"
#include "pipeline.h"
#include "save_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *thread_consume(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    file_saver_t saver;
    unsigned char *buf;
    int file_on;
    int saver_open = 0;

    if (p == NULL) {
        return NULL;
    }

    memset(&saver, 0, sizeof(saver));
    file_on = (p->output_path[0] != '\0');

    if (file_on) {
        if (file_saver_open(&saver, p->output_path, 0) != 0) {
            fprintf(stderr, "[consume] open output %s failed\n", p->output_path);
            pipeline_abort(p);
            return NULL;
        }
        saver_open = 1;
    }

    buf = (unsigned char *)malloc(PACKET_MAX_SIZE);
    if (buf == NULL) {
        fprintf(stderr, "[consume] malloc %u bytes failed\n", PACKET_MAX_SIZE);
        if (saver_open) {
            file_saver_close(&saver);
        }
        pipeline_abort(p);
        return NULL;
    }

    while (!p->fatal && (!p->encode_done || rb_used(&p->packet_rb) > 0)) {
        size_t len = 0;
        uint64_t pts_us = 0;
        uint32_t seq = 0;
        uint32_t flags = 0;
        int ret;

        ret = pipeline_read_packet(p, buf, PACKET_MAX_SIZE, &len,
                                   &pts_us, &seq, &flags,
                                   PIPELINE_READ_TIMEOUT_MS);
        if (ret == 0) {
            continue;   /* 超时，回去检查 encode_done/fatal */
        }
        if (ret < 0) {
            fprintf(stderr, "[consume] read packet failed\n");
            pipeline_abort(p);
            break;
        }

        if (len > 0) {
            /* 落盘：HEADER/EOS 也照常写入，保证 .h264 文件完整 */
            if (file_on) {
                if (file_saver_write(&saver, buf, len) != 0) {
                    fprintf(stderr, "[consume] write output failed at seq=%u\n", seq);
                    pipeline_abort(p);
                    break;
                }
                p->saved_packets++;
                p->saved_bytes += len;

                /* 定期刷盘，兼顾性能与掉电可恢复性 */
                if ((p->saved_packets % 100) == 0) {
                    if (file_saver_flush(&saver) != 0) {
                        pipeline_abort(p);
                        break;
                    }
                }
            }

            /* 推流：SPS/PPS 已在 u_ffmpeg_init 里作为 extradata 设置，跳过头包 */
            if (p->ff.configured &&
                !(flags & (PKT_FLAG_HEADER | PKT_FLAG_EOS))) {
                if (u_ffmpeg_push(&p->ff) == 1) {
                    int m = u_ffmpeg_mux(&p->ff, buf, len, pts_us);
                    if (m < 0) {
                        fprintf(stderr, "[consume] rtmp mux failed at seq=%u\n", seq);
                    }
                } else {
                    /* 未连接/重连退避：丢帧，不阻塞采集 */
                    p->ff.dropped++;
                }
            }
        }

        if (flags & PKT_FLAG_EOS) {
            /* EOS 包本身不写 payload；RTMP trailer 由 main 在所有线程退出后做 */
        }
    }

    free(buf);
    if (saver_open) {
        file_saver_close(&saver);
    }

    fprintf(stderr,
            "[consume] done, file_packets=%llu file_bytes=%llu "
            "rtmp_sent=%llu rtmp_dropped=%llu\n",
            p->saved_packets, p->saved_bytes,
            p->ff.frames, p->ff.dropped);
    return NULL;
}