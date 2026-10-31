#include "pipeline.h"

#include <stdio.h>
#include <string.h>

volatile sig_atomic_t g_pipeline_stop = 0;

void pipeline_abort(pipeline_t *p)
{
    if (p == NULL) {
        return;
    }

    p->fatal = 1;
    rb_stop(&p->frame_rb);
    rb_stop(&p->packet_rb);
}

int pipeline_write_frame(pipeline_t *p, const void *data, size_t len,
                         uint64_t pts_us, uint32_t seq, int timeout_ms)
{
    frame_record_t h;

    if (p == NULL || data == NULL || len == 0 || len > UINT32_MAX) {
        return -1;
    }

    memset(&h, 0, sizeof(h));
    h.magic  = FRAME_MAGIC;
    h.size   = (uint32_t)len;
    h.width  = (uint32_t)p->width;
    h.height = (uint32_t)p->height;
    h.pts_us = pts_us;
    h.seq    = seq;

    if (rb_write_block(&p->frame_rb, &h, sizeof(h), timeout_ms) != 0) {
        fprintf(stderr, "pipeline_write_frame: write header failed (fatal=%d)\n",
                p->fatal);
        return -1;
    }
    if (rb_write_block(&p->frame_rb, data, len, timeout_ms) != 0) {
        fprintf(stderr, "pipeline_write_frame: write payload failed (fatal=%d)\n",
                p->fatal);
        return -1;
    }

    return 0;
}

int pipeline_read_frame(pipeline_t *p, void *dst, size_t cap, size_t *out_len,
                        uint64_t *pts_us, uint32_t *seq, int timeout_ms)
{
    frame_record_t h;

    if (p == NULL || dst == NULL || out_len == NULL) {
        return -1;
    }

    if (rb_read_block(&p->frame_rb, &h, sizeof(h), timeout_ms) != 0) {
        return 0;   /* 超时/已 stop，由调用方检查 done/fatal */
    }

    if (h.magic != FRAME_MAGIC || h.size == 0 || h.size > cap) {
        fprintf(stderr, "pipeline_read_frame: bad record magic=0x%08x size=%u cap=%zu\n",
                h.magic, h.size, cap);
        return -1;
    }

    if (rb_read_block(&p->frame_rb, dst, h.size, PIPELINE_PAYLOAD_TIMEOUT_MS) != 0) {
        fprintf(stderr, "pipeline_read_frame: read payload failed\n");
        return -1;
    }

    *out_len = h.size;
    if (pts_us != NULL) {
        *pts_us = h.pts_us;
    }
    if (seq != NULL) {
        *seq = h.seq;
    }

    return 1;
}

int pipeline_write_packet(pipeline_t *p, const void *data, size_t len,
                          uint64_t pts_us, uint32_t seq, uint32_t flags, int timeout_ms)
{
    packet_record_t h;

    if (p == NULL || (data == NULL && len > 0) || len > PACKET_MAX_SIZE) {
        return -1;
    }

    memset(&h, 0, sizeof(h));
    h.magic  = PACKET_MAGIC;
    h.size   = (uint32_t)len;
    h.pts_us = pts_us;
    h.seq    = seq;
    h.flags  = flags;

    if (rb_write_block(&p->packet_rb, &h, sizeof(h), timeout_ms) != 0) {
        fprintf(stderr, "pipeline_write_packet: write header failed (fatal=%d)\n",
                p->fatal);
        return -1;
    }
    if (len > 0 && rb_write_block(&p->packet_rb, data, len, timeout_ms) != 0) {
        fprintf(stderr, "pipeline_write_packet: write payload failed (fatal=%d)\n",
                p->fatal);
        return -1;
    }

    return 0;
}

int pipeline_read_packet(pipeline_t *p, void *dst, size_t cap, size_t *out_len,
                         uint64_t *pts_us, uint32_t *seq, uint32_t *flags, int timeout_ms)
{
    packet_record_t h;

    if (p == NULL || dst == NULL || out_len == NULL) {
        return -1;
    }

    if (rb_read_block(&p->packet_rb, &h, sizeof(h), timeout_ms) != 0) {
        return 0;
    }

    if (h.magic != PACKET_MAGIC || h.size > cap || h.size > PACKET_MAX_SIZE) {
        fprintf(stderr, "pipeline_read_packet: bad record magic=0x%08x size=%u cap=%zu\n",
                h.magic, h.size, cap);
        return -1;
    }

    if (h.size > 0 &&
        rb_read_block(&p->packet_rb, dst, h.size, PIPELINE_PAYLOAD_TIMEOUT_MS) != 0) {
        fprintf(stderr, "pipeline_read_packet: read payload failed\n");
        return -1;
    }

    *out_len = h.size;
    if (pts_us != NULL) {
        *pts_us = h.pts_us;
    }
    if (seq != NULL) {
        *seq = h.seq;
    }
    if (flags != NULL) {
        *flags = h.flags;
    }

    return 1;
}