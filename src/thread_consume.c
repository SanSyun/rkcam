#include "thread_consume.h"
#include "pipeline.h"
#include "save_utils.h"

#include <stdio.h>
#include <stdlib.h>

void *thread_consume(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    file_saver_t saver;
    unsigned char *buf;

    if (p == NULL) {
        return NULL;
    }

    if (file_saver_open(&saver, p->output_path, 0) != 0) {
        fprintf(stderr, "[consume] open output %s failed\n", p->output_path);
        pipeline_abort(p);
        return NULL;
    }

    buf = (unsigned char *)malloc(PACKET_MAX_SIZE);
    if (buf == NULL) {
        fprintf(stderr, "[consume] malloc %u bytes failed\n", PACKET_MAX_SIZE);
        file_saver_close(&saver);
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

        if (flags & PKT_FLAG_EOS) {
            /* EOS 包本身不写 payload；文件完整性由 encode_done + fclose 保证 */
        }
    }

    free(buf);
    file_saver_close(&saver);

    fprintf(stderr, "[consume] done, packets=%llu bytes=%llu\n",
            p->saved_packets, p->saved_bytes);
    return NULL;
}