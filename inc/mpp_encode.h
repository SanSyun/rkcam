#ifndef MPP_ENCODE_H
#define MPP_ENCODE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "rk_mpi.h"
#include "rk_venc_cfg.h"
#include "rk_venc_rc.h"
#include "mpp_frame.h"
#include "mpp_buffer.h"
#include "mpp_packet.h"

typedef struct {
    MppCtx     ctx;         /* 编码器实例 */
    MppApi    *mpi;         /* mpp api 接口 */
    MppEncCfg  cfg;         /* 编码器配置 */
    int        width;
    int        height;
    int        frame_cnt;   /* 编码帧计数 */

    unsigned char *header;  /* SPS/PPS 拷贝，init 时取一次 */
    size_t         header_len;
} MppEncoder;

int  mpp_encoder_init(MppEncoder *enc, int width, int height, int fps);
int  mpp_encoder_get_header(MppEncoder *enc, const unsigned char **data, size_t *len);
int  mpp_encoder_encode(MppEncoder *enc, MppBuffer buf);
int  mpp_encoder_get_packet(MppEncoder *enc, MppPacket *packet, int timeout_ms);
int  mpp_encoder_send_eos(MppEncoder *enc);
void mpp_encoder_deinit(MppEncoder *enc);

int  mpp_init_buffer(int dmabuf_fd, MppBuffer *buf, size_t size, MppBufferType type);

#endif