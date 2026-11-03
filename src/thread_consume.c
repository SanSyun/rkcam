#include "thread_consume.h"
#include "pipeline.h"
#include "save_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void consume_tick_stats(pipeline_t *p)
{
    pipeline_stats_counters_t c;

    pipeline_collect_stats(p, &c);
    pipeline_stats_tick(&p->stats, &c);
}

/*
 * 落盘失败的处理：
 * - 还有推流（-or / -o -r）：只关本地文件、降级为继续推流，不中断采集；
 * - 仅落盘（-r）：没有别的 sink，保持整条流水线 abort。
 * 返回 1 表示已降级还能继续，0 表示已 abort、调用方应退出循环。
 */
static int consume_drop_file(pipeline_t *p, int *file_on,
                             file_saver_t *saver, int *saver_open)
{
    file_saver_close(saver);
    *saver_open = 0;

    if (p->ff.configured) {
        *file_on = 0;
        p->output_path[0] = '\0';
        fprintf(stderr, "[consume] recording disabled, streaming continues\n");
        return 1;
    }

    pipeline_abort(p);
    return 0;
}

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
            if (p->ff.configured) {
                /* -or：落盘打不开时降级为只推流 */
                fprintf(stderr,
                        "[consume] recording disabled, streaming continues\n");
                file_on = 0;
                p->output_path[0] = '\0';
            } else {
                pipeline_abort(p);
                return NULL;
            }
        } else {
            saver_open = 1;
        }
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
            consume_tick_stats(p);
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
                    if (!consume_drop_file(p, &file_on, &saver, &saver_open)) {
                        break;
                    }
                } else {
                    p->saved_packets++;
                    p->saved_bytes += len;
                    /* SPS/PPS 头包没有采集时间戳，不参与延迟统计 */
                    if (!(flags & (PKT_FLAG_HEADER | PKT_FLAG_EOS))) {
                        pipeline_stats_on_file(&p->stats, pts_us);
                    }

                    /* 定期刷盘，兼顾性能与掉电可恢复性 */
                    if ((p->saved_packets % 100) == 0) {
                        if (file_saver_flush(&saver) != 0) {
                            fprintf(stderr, "[consume] flush output failed\n");
                            if (!consume_drop_file(p, &file_on, &saver,
                                                   &saver_open)) {
                                break;
                            }
                        }
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
                    } else if (m == 1) {
                        pipeline_stats_on_mux(&p->stats, pts_us);
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

        consume_tick_stats(p);
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
