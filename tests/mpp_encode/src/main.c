#include "v4l2_capture.h"
#include "mpp_encode.h"

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

int main(int argc, char *argv[])
{
    struct v4l2_dev *camdev = &imx415;
	int fps = 19;
    int cap_num = 0;
    open_device(camdev); 
    get_capabilities(camdev);
    require_buf(camdev);       
    alloc_buf(camdev);          
    queue_buf(camdev);          
    stream_on(camdev);      
    ready_frame(camdev, 20);  //预热等AE稳定
    cap_num = fps*atoi(argv[1]);
    printf("fps = %d \ncapture frame num = %d\n", fps, cap_num);

    {
        MppEncoder enc;
        if (mpp_encoder_init(&enc, "/root/imx415.h264", camdev->width, camdev->height, fps) == 0) {
            encode_frame(camdev, &enc, cap_num);
            mpp_encoder_deinit(&enc);
        }
    }


    stream_off(camdev);
    close_device(camdev);
    _exit(0);
    return 0;
}