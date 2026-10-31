#include "mpp_encode.h"

// 把一阵raw编码为h264
int mpp_encode_file(const char *input, const char *output, int width, int height){
    MppCtx    ctx  = NULL;
    MppApi   *mpi  = NULL;
    MppEncCfg cfg  = NULL;
    MppBuffer buf   = NULL;
    MppFrame  frame = NULL;
    MPP_RET   ret;

    FILE     *fp_in = NULL, *fp_out = NULL;
    unsigned char *data = NULL;
    size_t frame_size = (size_t)width * height * 3 / 2;
    int ret_code = -1;

    //读取输入文件
    data = (unsigned char *)malloc(frame_size);
    if(!data){
        printf("failed to malloc!\n");
        return -1;
    }
    fp_in = fopen(input, "rb");
    if(!fp_in){
        printf("failed to open input file!\n");
        goto EXIT;
    }
    if (fread(data, 1, frame_size, fp_in) != frame_size) {
        printf("read %s failed, size not match (expect %zu)\n", input, frame_size);
        goto EXIT;
    }
    fclose(fp_in);
    fp_in = NULL;

    fp_out = fopen(output, "wb");
    if(!fp_out){
        printf("failed to open output file!\n");
        goto EXIT;
    }

    //创建编码器实例
    ret = mpp_create(&ctx, &mpi);
    if(ret != MPP_OK){
        printf("mpp_create failed %d\n", ret); 
        goto EXIT;
    }
    
    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC); //编码为h264
    if(ret != MPP_OK){
        printf("mpp_init failed %d\n", ret); 
        goto EXIT;
    }

    //配置编码参数
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width",       width); //输入参数
    mpp_enc_cfg_set_s32(cfg, "prep:height",      height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",  width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",  height);
    mpp_enc_cfg_set_s32(cfg, "prep:format",      MPP_FMT_YUV420SP); 

    mpp_enc_cfg_set_s32(cfg, "rc:mode",          MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target",    20 * 1000 * 1000);   /* 20Mbps */
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex",   0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",    30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex",  0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",   30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm",1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",           60);

    mpp_enc_cfg_set_s32(cfg, "codec:type",       MPP_VIDEO_CodingAVC); //输出参数
    mpp_enc_cfg_set_s32(cfg, "h264:profile",     100); 
    mpp_enc_cfg_set_s32(cfg, "h264:level",       51);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en",    1);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    if(ret != MPP_OK){
        printf("MPP_ENC_SET_CFG failed %d\n", ret); 
        goto EXIT;
    }

    //取SPS/PPS头写入文件
    {
        MppPacket header = NULL;
        MppBuffer header_buf = NULL;

        ret = mpp_buffer_get(NULL, &header_buf, 4096);
        if (ret != MPP_OK) {
            printf("mpp_buffer_get failed: %d\n", ret);
            goto EXIT;
        }

        ret = mpp_packet_init_with_buffer(&header, header_buf);
        if (ret != MPP_OK) {
            printf("mpp_packet_init_with_buffer failed: %d\n", ret);
            mpp_buffer_put(header_buf);
            goto EXIT;
        }

        mpp_packet_set_length(header, 0);   

        ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, header);
        if (ret == MPP_OK) {
            void *ptr = mpp_packet_get_pos(header);
            size_t len = mpp_packet_get_length(header);

            if (ptr && len > 0 && len <= 4096)
                fwrite(ptr, 1, len, fp_out);
            else
                ret = MPP_NOK;
        }

        mpp_packet_deinit(&header);
        mpp_buffer_put(header_buf);

        if (ret != MPP_OK){
            goto EXIT;  
        }
    }

    //内部分配MppBuffer并封输入帧
    ret = mpp_buffer_get(NULL, &buf, frame_size);
    if (ret != MPP_OK){
        printf("mpp_buffer_get failed %d\n", ret); 
        goto EXIT; 
    }
    memcpy(mpp_buffer_get_ptr(buf), data, frame_size); //拷贝到mpp内部buffer
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, width);
    mpp_frame_set_ver_stride(frame, height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_eos(frame, 1);   

    //阻塞输入编码器
    ret = mpi->encode_put_frame(ctx, frame);
    if(ret != MPP_OK){
        printf("encode_put_frame failed %d\n", ret); 
        goto EXIT;     
    }

    //取码流
    {
        size_t total = 0;
        int retry = 0;    
        while (1){
            MppPacket packet = NULL;
            ret = mpi->encode_get_packet(ctx, &packet);
            if(packet){
                size_t len = mpp_packet_get_length(packet);
                int eos = mpp_packet_get_eos(packet);
                if(len){
                    fwrite(mpp_packet_get_pos(packet), 1, len, fp_out);
                }
                total += len;
                mpp_packet_deinit(&packet);
                if(eos){
                    break;
                }
                retry = 0;
            }
            else{
                if(++retry > 10000){
                    printf("encode_get_packet timeout\n");
                    goto EXIT;
                }
                usleep(1000);
            }
        }
        printf("encode done: %s, %zu bytes\n", output, total);
    }

    ret_code = 0;

EXIT:
    if (frame)  mpp_frame_deinit(&frame);
    if (buf)    mpp_buffer_put(buf);
    if (cfg)    mpp_enc_cfg_deinit(cfg);
    if (ctx) {
        mpi->reset(ctx);
        mpp_destroy(ctx);
    }
    if (fp_in)  fclose(fp_in);
    if (fp_out) fclose(fp_out);
    if (data)   free(data);
    return ret_code;
}
