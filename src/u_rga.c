#include "u_rga.h"

int rga_resize_nv12(int src_fd, int src_w, int src_h, int src_wstride, 
    int dst_fd, int dst_w, int dst_h, int dst_wstride){
    
    rga_buffer_t src = wrapbuffer_fd(src_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP, src_wstride, src_h);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, dst_w, dst_h, RK_FORMAT_YCbCr_420_SP, dst_wstride, dst_h);

    IM_STATUS ret = imresize(src, dst);
    
    if (ret != IM_STATUS_SUCCESS) {
        fprintf(stderr, "rga resize failed: %s (%d)\n", imStrError(ret), ret);
        return -1;
    }
    return 0;
}
