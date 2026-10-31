#include "thread_capture.h"
#include "pipeline.h"
#include "u_rga.h"
#include "u_dma.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

void *thread_capture(void *arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    struct v4l2_dev *dev;
    int dst_fd;
    void *dst_ptr;

    if (p == NULL || p->dev == NULL || p->rga_buf == NULL) {
        fprintf(stderr, "[capture] invalid pipeline\n");
        return NULL;
    }

    dev = p->dev;
    dst_fd = mpp_buffer_get_fd(p->rga_buf);
    dst_ptr = mpp_buffer_get_ptr(p->rga_buf);
    if (dst_fd < 0 || dst_ptr == NULL) {
        fprintf(stderr, "[capture] get RGA buffer fd/ptr failed\n");
        pipeline_abort(p);
        p->capture_done = 1;
        return NULL;
    }

    while (!g_pipeline_stop && !p->fatal &&
           p->captured < (unsigned long long)p->frame_num) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        struct pollfd pfd;
        int pr;
        int failed = 0;
        int cpu_began = 0;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = dev->fd;
        pfd.events = POLLIN;

        pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[capture] poll failed: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        if (pr == 0) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[capture] poll revents=0x%x\n", pfd.revents);
            failed = 1;
            break;
        }
        if (p->fatal || g_pipeline_stop) {
            break;
        }

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = dev->buf_type;
        buf.memory = dev->memory_type;
        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        //取原始视频流
        if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "[capture] VIDIOC_DQBUF failed: %s\n", strerror(errno));
            failed = 1;
            break;
        }

        if (buf.index >= dev->req_count) {
            fprintf(stderr, "[capture] invalid buffer index %u\n", buf.index);
            failed = 1;
            goto requeue;
        }

        //RGA调整大小
        if (rga_resize_nv12(dev->buffers[buf.index].fd,
                            dev->width, dev->height, dev->width,
                            dst_fd, ENC_WIDTH, ENC_HEIGHT, ENC_WIDTH) != 0) {
            fprintf(stderr, "[capture] rga_resize_nv12 failed at seq=%u\n", buf.sequence);
            failed = 1;
            goto requeue;
        }

        if (dma_buf_cpu_begin(dst_fd, 0) != 0) {
            fprintf(stderr, "[capture] dma_buf_cpu_begin failed\n");
            failed = 1;
            goto requeue;
        }
        cpu_began = 1;

        {
            uint64_t pts_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL
                            + (uint64_t)buf.timestamp.tv_usec;

            if (pipeline_write_frame(p, dst_ptr, ENC_FRAME_SIZE,
                                     pts_us, (uint32_t)buf.sequence,
                                     PIPELINE_WRITE_TIMEOUT_MS) != 0) {
                fprintf(stderr, "[capture] write frame failed at seq=%u\n", buf.sequence);
                failed = 1;
            } else {
                p->captured++;
            }
        }

requeue:
        if (cpu_began) {
            if (dma_buf_cpu_end(dst_fd, 0) != 0) {
                fprintf(stderr, "[capture] dma_buf_cpu_end failed\n");
                failed = 1;
            }
        }

        /* DQBUF 成功后无论处理是否成功都必须归还 buffer */
        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[capture] VIDIOC_QBUF failed: %s\n", strerror(errno));
            failed = 1;
        }

        if (failed) {
            pipeline_abort(p);
            break;
        }
    }

    p->capture_done = 1;
    fprintf(stderr, "[capture] done, captured=%llu\n", p->captured);
    return NULL;
}