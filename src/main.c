#include "v4l2_capture.h"
#include "pipeline.h"
#include "thread_capture.h"
#include "thread_encode.h"
#include "thread_consume.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct v4l2_dev imx415 = {
    .fd = -1,
    .sub_fd = -1,
    .path = "/dev/video0",
    .name = "imx415",
    .subdev_path = "/dev/v4l-subdev3",
    .out_type = "nv12",
    .buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .format = V4L2_PIX_FMT_NV12,
    .width = 3840,
    .height = 2160,
    .req_count = 4,
    .memory_type = V4L2_MEMORY_MMAP,
    .buffers = NULL,
    .timestamp = 0,
    .data_len = 0,
    .out_data = NULL,
};

static void on_signal(int sig)
{
    (void)sig;
    g_pipeline_stop = 1;
}

int main(int argc, char *argv[])
{
    struct v4l2_dev *camdev = &imx415;
    pipeline_t p;
    pthread_t tid_cap;
    pthread_t tid_enc;
    pthread_t tid_con;
    int cap_started = 0;
    int enc_started = 0;
    int con_started = 0;
    int rb_frame_ok = 0;
    int rb_packet_ok = 0;
    int enc_ok = 0;
    int stream_ok = 0;
    int fps = 30;
    int seconds;
    int n;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <seconds> [output.h264]\n", argv[0]);
        return 1;
    }

    seconds = atoi(argv[1]);
    if (seconds <= 0) {
        fprintf(stderr, "invalid seconds: %s\n", argv[1]);
        return 1;
    }

    memset(&p, 0, sizeof(p));
    p.dev = camdev;
    p.width = ENC_WIDTH;
    p.height = ENC_HEIGHT;
    p.fps = fps;
    p.frame_num = fps * seconds;

    if (argc >= 3) {
        n = snprintf(p.output_path, sizeof(p.output_path), "%s", argv[2]);
    } else {
        n = snprintf(p.output_path, sizeof(p.output_path), "%s", "/root/imx415.h264");
    }
    if (n < 0 || (size_t)n >= sizeof(p.output_path)) {
        fprintf(stderr, "output path too long\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("fps=%d frame_num=%d output=%s\n", fps, p.frame_num, p.output_path);

    open_device(camdev);
    get_capabilities(camdev);
    set_fmt(camdev);
    require_buf(camdev);
    alloc_buf(camdev);
    queue_buf(camdev);
    stream_on(camdev);
    stream_ok = 1;
    ready_frame(camdev, 20);  // 预热等 AE 稳定

    //初始化frame和packet环形缓冲区
    if (rb_init(&p.frame_rb, FRAME_RING_SIZE) != 0) {
        fprintf(stderr, "rb_init frame failed\n");
        goto cleanup;
    }
    rb_frame_ok = 1;

    if (rb_init(&p.packet_rb, PACKET_RING_SIZE) != 0) {
        fprintf(stderr, "rb_init packet failed\n");
        goto cleanup;
    }
    rb_packet_ok = 1;

    //初始化编码器
    if (mpp_encoder_init(&p.enc, ENC_WIDTH, ENC_HEIGHT, fps) != 0) {
        fprintf(stderr, "mpp_encoder_init failed\n");
        goto cleanup;
    }
    enc_ok = 1;

    //申请两个mppbuffer
    if (mpp_buffer_get(NULL, &p.rga_buf, ENC_FRAME_SIZE) != MPP_OK ||
        p.rga_buf == NULL) {
        fprintf(stderr, "mpp_buffer_get rga_buf failed\n");
        goto cleanup;
    }

    if (mpp_buffer_get(NULL, &p.enc_buf, ENC_FRAME_SIZE) != MPP_OK ||
        p.enc_buf == NULL) {
        fprintf(stderr, "mpp_buffer_get enc_buf failed\n");
        goto cleanup;
    }

    //创建采集/编码/消费线程
    if (pthread_create(&tid_cap, NULL, thread_capture, &p) != 0) {
        fprintf(stderr, "pthread_create capture failed\n");
        goto cleanup;
    }
    cap_started = 1;

    if (pthread_create(&tid_enc, NULL, thread_encode, &p) != 0) {
        fprintf(stderr, "pthread_create encode failed\n");
        goto cleanup;
    }
    enc_started = 1;

    if (pthread_create(&tid_con, NULL, thread_consume, &p) != 0) {
        fprintf(stderr, "pthread_create consume failed\n");
        goto cleanup;
    }
    con_started = 1;

    /* capture 到目标帧数/收到信号后结束；encode/consume 依次排空 */
    pthread_join(tid_cap, NULL);
    cap_started = 0;
    pthread_join(tid_enc, NULL);
    enc_started = 0;
    pthread_join(tid_con, NULL);
    con_started = 0;

cleanup:
    if (cap_started || enc_started || con_started) {
        g_pipeline_stop = 1;
        pipeline_abort(&p);   // 唤醒还阻塞在环上的线程
        if (cap_started) {
            pthread_join(tid_cap, NULL);
            cap_started = 0;
        }
        if (enc_started) {
            pthread_join(tid_enc, NULL);
            enc_started = 0;
        }
        if (con_started) {
            pthread_join(tid_con, NULL);
            con_started = 0;
        }
    }

    if (p.enc_buf != NULL) {
        mpp_buffer_put(p.enc_buf);
        p.enc_buf = NULL;
    }
    if (p.rga_buf != NULL) {
        mpp_buffer_put(p.rga_buf);
        p.rga_buf = NULL;
    }
    if (enc_ok) {
        mpp_encoder_deinit(&p.enc);
    }
    if (rb_packet_ok) {
        rb_deinit(&p.packet_rb);
    }
    if (rb_frame_ok) {
        rb_deinit(&p.frame_rb);
    }
    if (stream_ok) {
        stream_off(camdev);
    }
    close_device(camdev);

    printf("pipeline done: captured=%llu encoded=%llu packets=%llu bytes=%llu fatal=%d\n",
           p.captured, p.encoded, p.saved_packets, p.saved_bytes, p.fatal);
    fflush(stdout);

    _exit(0);
    return 0;
}