#include "v4l2_capture.h"

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
	unsigned int fps = 0;
    unsigned int mode = 4;
    char name[40] = {0};
	
    if (argc > 1)
        fps = atoi(argv[1]);

    open_device(camdev); 
    get_capabilities(camdev);
    set_fmt(camdev);              
    require_buf(camdev);       
    alloc_buf(camdev);          
    queue_buf(camdev);          
    set_fps(camdev, fps);

    stream_on(camdev);      
    get_frame(camdev, 20); 
    
    snprintf(name, sizeof(name), "/root/%s.%s", camdev->name, camdev->out_type);
    save_picture(name, camdev->out_data, camdev->data_len, 1);

    stream_off(camdev);
    close_device(camdev); 
    return 0;
}