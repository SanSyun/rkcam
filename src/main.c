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

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage:\n"
            "  %s -o <rtmp_url>                 push H.264 to RTMP\n"
            "  %s -r <output.h264>              record H.264 to file\n"
            "  %s -or <rtmp_url> <output.h264>  push and record together\n"
            "\n"
            "-o and -r may be given together (same as -or); at least one sink\n"
            "is required. Runs until SIGINT/SIGTERM.\n"
            "stats: one [stats] line per second on stderr, latency summary at exit\n",
            prog, prog, prog);
}

/*
 * 手写参数解析，不能用 getopt：
 * getopt 会把 "-or url path" 拆成 -o 的参数 "r"，剩下两个变成非法位置参数。
 * 返回 0 继续，1 已打印帮助，-1 参数错误。
 */
static int parse_args(int argc, char *argv[], const char **rtmp_url,
                      const char **record_path)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 1;
        }

        if (strcmp(arg, "-o") == 0 || strcmp(arg, "-or") == 0) {
            int both = (strcmp(arg, "-or") == 0);
            int need = both ? 2 : 1;

            if (i + need >= argc) {
                fprintf(stderr, "%s needs %s\n", arg,
                        both ? "<rtmp_url> <output.h264>" : "<rtmp_url>");
                usage(argv[0]);
                return -1;
            }
            if (*rtmp_url != NULL) {
                fprintf(stderr,
                        "duplicate push url (use only one of -o / -or)\n");
                return -1;
            }
            if (both && *record_path != NULL) {
                fprintf(stderr,
                        "duplicate record path (use only one of -r / -or)\n");
                return -1;
            }

            *rtmp_url = argv[++i];
            if (both) {
                *record_path = argv[++i];
            }
            continue;
        }

        if (strcmp(arg, "-r") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-r needs <output.h264>\n");
                usage(argv[0]);
                return -1;
            }
            if (*record_path != NULL) {
                fprintf(stderr,
                        "duplicate record path (use only one of -r / -or)\n");
                return -1;
            }
            *record_path = argv[++i];
            continue;
        }

        fprintf(stderr, "unknown argument: %s\n", arg);
        usage(argv[0]);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct v4l2_dev *camdev = &imx415;
    pipeline_t p;
    pthread_t tid_cap;
    pthread_t tid_enc;
    pthread_t tid_con;
    const char *rtmp_url = NULL;
    const char *record_path = NULL;
    int cap_started = 0;
    int enc_started = 0;
    int con_started = 0;
    int rb_frame_ok = 0;
    int rb_packet_ok = 0;
    int enc_ok = 0;
    int stream_ok = 0;
    int fps = 30;
    int pr;
    int n;

    pr = parse_args(argc, argv, &rtmp_url, &record_path);
    if (pr != 0) {
        return (pr > 0) ? 0 : 1;
    }

    if (rtmp_url == NULL && record_path == NULL) {
        fprintf(stderr,
                "at least one of -o <rtmp_url>, -r <output.h264>, "
                "-or <rtmp_url> <output.h264> is required\n");
        usage(argv[0]);
        return 1;
    }
    if (rtmp_url != NULL && rtmp_url[0] == '\0') {
        fprintf(stderr, "empty rtmp url\n");
        return 1;
    }
    if (record_path != NULL && record_path[0] == '\0') {
        fprintf(stderr, "empty output path\n");
        return 1;
    }

    memset(&p, 0, sizeof(p));
    p.dev = camdev;
    p.width = ENC_WIDTH;
    p.height = ENC_HEIGHT;
    p.fps = fps;
    pipeline_stats_init(&p.stats);

    if (record_path != NULL) {
        n = snprintf(p.output_path, sizeof(p.output_path), "%s", record_path);
        if (n < 0 || (size_t)n >= sizeof(p.output_path)) {
            fprintf(stderr, "output path too long\n");
            return 1;
        }
    } else {
        p.output_path[0] = '\0';   /* 推流模式不落盘 */
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("mode=%s fps=%d output=%s rtmp=%s (run until SIGINT/SIGTERM)\n",
           (rtmp_url != NULL && record_path != NULL) ? "push+record" :
           ((rtmp_url != NULL) ? "push" : "record"),
           fps,
           (p.output_path[0] != '\0') ? p.output_path : "(disabled)",
           (rtmp_url != NULL) ? rtmp_url : "(disabled)");

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

    //推流模式：编码器已拿到 SPS/PPS，交给 FFmpeg 生成 avcC
    if (rtmp_url != NULL) {
        if (u_ffmpeg_init(&p.ff, rtmp_url, ENC_WIDTH, ENC_HEIGHT, fps,
                          p.enc.header, p.enc.header_len) != 0) {
            fprintf(stderr, "u_ffmpeg_init failed\n");
            goto cleanup;
        }
    }

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

    /* 常驻运行：收到 SIGINT/SIGTERM 后 capture 停，encode/consume 依次排空 */
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

    /* 所有线程已退出：先出统计汇总，再关 RTMP（deinit 会清 configured） */
    {
        pipeline_stats_counters_t sc;

        pipeline_collect_stats(&p, &sc);
        pipeline_stats_summary(&p.stats, &sc);
    }

    /* 所有线程已退出，安全关闭 RTMP 并写 trailer */
    if (p.ff.configured) {
        u_ffmpeg_deinit(&p.ff);
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

    printf("pipeline done: captured=%llu encoded=%llu file_packets=%llu "
           "file_bytes=%llu rtmp_sent=%llu rtmp_dropped=%llu fatal=%d\n",
           p.captured, p.encoded, p.saved_packets, p.saved_bytes,
           p.ff.frames, p.ff.dropped, p.fatal);
    fflush(stdout);

    _exit(0);
    return 0;
}
