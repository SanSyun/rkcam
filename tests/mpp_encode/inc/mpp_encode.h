#ifndef MPP_ENCODE_H
#define MPP_ENCODE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mpp_encode.h"

#include "rk_mpi.h"
#include "rk_venc_cfg.h"
#include "rk_venc_rc.h"
#include "mpp_frame.h"
#include "mpp_buffer.h"
#include "mpp_packet.h"

typedef struct {
    MppCtx     ctx; //编码器实例
    MppApi    *mpi; //mpp api接口
    MppEncCfg  cfg; //编码器配置
    FILE      *fp_out; //输出文件
    int        width;
    int        height;
    int        frame_cnt; //编码帧计数
} MppEncoder;

int  mpp_encoder_init(MppEncoder *enc, const char *output, int width, int height, int fps);
int  mpp_encoder_encode_fd(MppEncoder *enc, int dmabuf_fd, size_t size); 
void mpp_encoder_deinit(MppEncoder *enc);                                 

#endif 