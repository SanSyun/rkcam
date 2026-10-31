#include "mpp_encode.h"

//初始化编码器：创建实例、配置参数、写 SPS/PPS 头
int mpp_encoder_init(MppEncoder *enc, const char *output, int width, int height, int fps){
    MPP_RET ret;
    int rc_fps = (fps > 0) ? fps : 30;

    memset(enc, 0, sizeof(*enc));
    enc->width  = width;
    enc->height = height;

    enc->fp_out = fopen(output, "wb");
    if (!enc->fp_out) { 
        printf("open %s failed\n", output); 
        return -1; 
    }

    //创建编码器实例
    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret != MPP_OK) {
        printf("mpp_create failed %d\n", ret);
        return -1; 
    }
    ret = mpp_init(enc->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);//编码为h264
    if(ret != MPP_OK){
        printf("mpp_init failed %d\n", ret); 
        return -1; 
    }

    //配置编码参数
    mpp_enc_cfg_init(&enc->cfg);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:width",       width); //输入参数
    mpp_enc_cfg_set_s32(enc->cfg, "prep:height",      height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:hor_stride",  width);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:ver_stride",  height);
    mpp_enc_cfg_set_s32(enc->cfg, "prep:format",      MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc->cfg, "rc:mode",           MPP_ENC_RC_MODE_CBR); //固定码率模式
    mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target",     20 * 1000 * 1000);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_flex",    0); //输入帧率固定
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_num",     rc_fps); //帧率分子
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_in_denorm",  1); //帧率分母
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_flex",   0); //输出帧率固定
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_num",    rc_fps);
    mpp_enc_cfg_set_s32(enc->cfg, "rc:fps_out_denorm", 1); //全为I帧
    mpp_enc_cfg_set_s32(enc->cfg, "rc:gop",            rc_fps * 2); //I帧帧间隔

    mpp_enc_cfg_set_s32(enc->cfg, "codec:type",        MPP_VIDEO_CodingAVC); //输出参数
    mpp_enc_cfg_set_s32(enc->cfg, "h264:profile",      100); //High profile
    mpp_enc_cfg_set_s32(enc->cfg, "h264:level",        51); //4K@30fps
    mpp_enc_cfg_set_s32(enc->cfg, "h264:cabac_en",     1); //使用的熵编码格式

    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg);
    if(ret != MPP_OK){
        printf("MPP_ENC_SET_CFG failed %d\n", ret); 
        return -1;
    }

    //取SPS/PPS头写入文件
    {
        MppPacket header = NULL;
        MppBuffer header_buf = NULL;

        ret = mpp_buffer_get(NULL, &header_buf, 4096);
        if (ret != MPP_OK) {
            printf("mpp_buffer_get failed: %d\n", ret);
            return -1;
        }

        ret = mpp_packet_init_with_buffer(&header, header_buf);
        if (ret != MPP_OK) {
            printf("mpp_packet_init_with_buffer failed: %d\n", ret);
            mpp_buffer_put(header_buf);
            return -1;
        }

        mpp_packet_set_length(header, 0);   

        ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_HDR_SYNC, header); //获取码流数据头
        if (ret == MPP_OK) {
            void *ptr = mpp_packet_get_pos(header);
            size_t len = mpp_packet_get_length(header);

            if (ptr && len > 0 && len <= 4096)
                fwrite(ptr, 1, len, enc->fp_out);
            else
                ret = MPP_NOK;
        }

        mpp_packet_deinit(&header);
        mpp_buffer_put(header_buf);

        if (ret != MPP_OK){
            return -1;
        }
    }

    return 0;
}

//初始化mpp buffer：把外部dma-buf fd导入成MppBuffer，调用方负责最终mpp_buffer_put
int mpp_init_buffer(int dmabuf_fd, MppBuffer *buf, size_t size, MppBufferType type){
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
    if (ret != MPP_OK || *buf == NULL){
        printf("mpp_buffer_import failed %d\n", ret);
        *buf = NULL;
        return -1;
    }

    return 0;
}

//零拷贝编码一帧：把已导入的MppBuffer喂给编码器，取出码流写文件
//注意：buf的引用归调用方所有，本函数不调用mpp_buffer_put
int mpp_encoder_encode(MppEncoder *enc, MppBuffer buf){
    MppFrame  frame = NULL;
    MPP_RET   ret;
    int retry = 0;

    if (!enc || !buf) {
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

    //阻塞输入编码器
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);   
    if (ret != MPP_OK) {
        printf("encode_put_frame failed %d\n", ret);
        mpp_frame_deinit(&frame);
        return -1;   //buf引用归调用方，不在这里put
    }

    //取码流写文件
    while (1) {
        MppPacket packet = NULL;
        enc->mpi->encode_get_packet(enc->ctx, &packet);
        if (packet) {
            size_t len = mpp_packet_get_length(packet);
            if (len){
                fwrite(mpp_packet_get_pos(packet), 1, len, enc->fp_out);
            }
            mpp_packet_deinit(&packet);
            break;
        }
        if (++retry > 10000) {
            printf("get packet timeout\n");
            break;
        }
        usleep(1000);
    }

    mpp_frame_deinit(&frame);
    //不再mpp_buffer_put：buffer由调用方缓存，并在析构时统一释放
    enc->frame_cnt++;
    return 0;
}

//收尾：送 EOS 空帧冲刷编码器，取完剩余码流，销毁实例
void mpp_encoder_deinit(MppEncoder *enc){
    MppFrame frame = NULL;
    int retry = 0;

    mpp_frame_init(&frame);
    mpp_frame_set_eos(frame, 1); //不带buffer的空帧EOS
    enc->mpi->encode_put_frame(enc->ctx, frame);

    while (1) {
        MppPacket packet = NULL;
        enc->mpi->encode_get_packet(enc->ctx, &packet);
        if (packet) {
            size_t len = mpp_packet_get_length(packet);
            int eos = mpp_packet_get_eos(packet);
            if (len){
                fwrite(mpp_packet_get_pos(packet), 1, len, enc->fp_out);
            }
            mpp_packet_deinit(&packet);
            if (eos){
                break;
            }
            retry = 0;
        } else {
            if(++retry > 10000){
                printf("encode_get_packet timeout\n");
                return;
            }            
            usleep(1000);
        }
    }
    mpp_frame_deinit(&frame);

    mpp_enc_cfg_deinit(enc->cfg);
    enc->mpi->reset(enc->ctx);
    mpp_destroy(enc->ctx);
    fclose(enc->fp_out);
    printf("mpp encoder deinit, total %d frames\n", enc->frame_cnt);
}
