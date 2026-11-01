#include "mpp_encode.h"

/* 初始化编码器：创建实例、配置参数、缓存 SPS/PPS 头；不再打开输出文件 */
int mpp_encoder_init(MppEncoder *enc, int width, int height, int fps)
{
    MPP_RET ret;
    int rc_fps = (fps > 0) ? fps : 30;

    if (enc == NULL) {
        return -1;
    }

    memset(enc, 0, sizeof(*enc));
    enc->width  = width;
    enc->height = height;

    /* 创建编码器实例 */
    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret != MPP_OK) {
        printf("mpp_create failed %d\n", ret);
        goto fail;
    }
    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC); // 编码为 h264
    if (ret != MPP_OK) {
        printf("mpp_init failed %d\n", ret);
        goto fail;
    }

    /* 配置编码参数 */
    mpp_enc_cfg_init(&enc->cfg);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:width",       width); // 输入参数
    mpp_enc_cfg_set_s32(enc->cfg, "prep:height",      height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:hor_stride",  width);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:ver_stride",  height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:format",      MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc->cfg, "rc:mode",           MPP_ENC_RC_MODE_CBR); // 固定码率模式
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target",     8 * 1000 * 1000);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_flex",    0); // 输入帧率固定
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_num",     rc_fps); // 帧率分子
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_denorm",  1); // 帧率分母
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_flex",   0); // 输出帧率固定
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_num",    rc_fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:gop",            rc_fps * 2); // I 帧帧间隔

    mpp_enc_cfg_set_s32(enc->cfg, "codec:type",        MPP_VIDEO_CodingAVC); // 输出参数
    mpp_enc_cfg_set_s32(enc->cfg, "h264:profile",      100); // High profile
    mpp_enc_cfg_set_s32(enc->cfg, "h264:level",        51);  // 4K@30fps
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_en",     1);   // 使用的熵编码格式

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if (ret != MPP_OK) {
        printf("MPP_ENC_SET_CFG failed %d\n", ret);
        goto fail;
    }

    /* 取 SPS/PPS 头缓存起来，由编码线程作为第一条包头写入码流环 */
    {
        MppPacket header = NULL;
        MppBuffer header_buf = NULL;
        void *ptr;
        size_t len;

        ret = mpp_buffer_get(NULL, &header_buf, 4096);
        if (ret != MPP_OK) {
            printf("mpp_buffer_get header failed: %d\n", ret);
            goto fail;
        }

        ret = mpp_packet_init_with_buffer(&header, header_buf);
        if (ret != MPP_OK) {
            printf("mpp_packet_init_with_buffer failed: %d\n", ret);
            mpp_buffer_put(header_buf);
            goto fail;
        }

        mpp_packet_set_length(header, 0);

        ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, header); // 获取码流数据头
        if (ret == MPP_OK) {
            ptr = mpp_packet_get_pos(header);
            len = mpp_packet_get_length(header);
            if (ptr != NULL && len > 0 && len <= 4096) {
                enc->header = (unsigned char *)malloc(len);
                if (enc->header == NULL) {
                    printf("malloc encoder header failed\n");
                    ret = MPP_NOK;
                } else {
                    memcpy(enc->header, ptr, len);
                    enc->header_len = len;
                }
            } else {
                ret = MPP_NOK;
            }
        }

        mpp_packet_deinit(&header);
        mpp_buffer_put(header_buf);

        if (ret != MPP_OK) {
            printf("get SPS/PPS header failed\n");
            goto fail;
        }
    }

    return 0;

fail:
    mpp_encoder_deinit(enc);
    return -1;
}

int mpp_encoder_get_header(MppEncoder *enc, const unsigned char **data, size_t *len)
{
    if (enc == NULL || data == NULL || len == NULL ||
        enc->header == NULL || enc->header_len == 0) {
        return -1;
    }

    *data = enc->header;
    *len  = enc->header_len;
    return 0;
}

/* 初始化 mpp buffer：把外部 dma-buf fd 导入成 MppBuffer，调用方负责最终 mpp_buffer_put */
int mpp_init_buffer(int dmabuf_fd, MppBuffer *buf, size_t size, MppBufferType type)
{
    MPP_RET ret;
    MppBufferInfo info;

    if (!buf || dmabuf_fd < 0 || size == 0) {
        printf("mpp_init_buffer: invalid argument\n");
        return -1;
    }

    memset(&info, 0, sizeof(info));
    info.type = type;
    info.fd   = dmabuf_fd;
    info.size = size;

    *buf = NULL;
    ret = mpp_buffer_import(buf, &info);
    if (ret != MPP_OK || *buf == NULL) {
        printf("mpp_buffer_import failed %d\n", ret);
        *buf = NULL;
        return -1;
    }

    return 0;
}

/* 只把一帧送进编码器；取包由调用方通过 mpp_encoder_get_packet 完成 */
int mpp_encoder_encode(MppEncoder *enc, MppBuffer buf)
{
    MppFrame frame = NULL;
    MPP_RET  ret;

    if (enc == NULL || buf == NULL) {
        printf("mpp_encoder_encode: invalid argument\n");
        return -1;
    }

    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->width);
    mpp_frame_set_ver_stride(frame, enc->height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buf);

    // 阻塞输入编码器
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    if (ret != MPP_OK) {
        printf("encode_put_frame failed %d\n", ret);
        mpp_frame_deinit(&frame);
        return -1;
    }

    mpp_frame_deinit(&frame);
    enc->frame_cnt++;
    return 0;
}

/*
 * 取一个码流包。返回 0 表示取到 packet，-1 表示超时或失败。
 * timeout_ms = 0 只试一次；> 0 最多轮询 timeout_ms 毫秒。
 */
int mpp_encoder_get_packet(MppEncoder *enc, MppPacket *packet, int timeout_ms)
{
    int waited = 0;

    if (enc == NULL || packet == NULL || enc->mpi == NULL) {
        return -1;
    }

    *packet = NULL;

    while (1) {
        MppPacket pkt = NULL;

        /* 与单线程版本保持一致：无包时 packet 为 NULL，继续轮询 */
        enc->mpi->encode_get_packet(enc->ctx, &pkt);

        if (pkt != NULL) {
            *packet = pkt;
            return 0;
        }

        if (timeout_ms <= 0 || waited >= timeout_ms) {
            return -1;
        }
        usleep(1000);
        waited++;
    }
}

/* 送 EOS 空帧，让编码器把内部缓存的帧全部吐出来 */
int mpp_encoder_send_eos(MppEncoder *enc)
{
    MppFrame frame = NULL;
    MPP_RET  ret;

    if (enc == NULL || enc->mpi == NULL) {
        return -1;
    }

    mpp_frame_init(&frame);
    mpp_frame_set_eos(frame, 1);
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);

    return (ret == MPP_OK) ? 0 : -1;
}

/* 收尾：不再发 EOS、不再写文件，只释放实例和缓存 */
void mpp_encoder_deinit(MppEncoder *enc)
{
    if (enc == NULL) {
        return;
    }

    if (enc->header != NULL) {
        free(enc->header);
        enc->header = NULL;
        enc->header_len = 0;
    }

    if (enc->cfg != NULL) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = NULL;
    }

    if (enc->ctx != NULL && enc->mpi != NULL) {
        enc->mpi->reset(enc->ctx);
    }

    if (enc->ctx != NULL) {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
    }

    printf("mpp encoder deinit, total %d frames\n", enc->frame_cnt);
    memset(enc, 0, sizeof(*enc));
}