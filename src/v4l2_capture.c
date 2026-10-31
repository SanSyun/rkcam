#include "mpp_encode.h"
#include "v4l2_capture.h"
#include "u_rga.h"
#include "u_dma.h"

//关闭设备
void close_device(struct v4l2_dev *dev){
    //取消注册buffer
    if (dev->buffers) {
        for (unsigned int i = 0; i < dev->req_count; ++i) {
            if (dev->buffers[i].start) {
                munmap(dev->buffers[i].start, dev->buffers[i].length);
            }

            //关闭dma-buf fd
            if(dev->buffers[i].fd > 0){
                close(dev->buffers[i].fd);
            }
        }
        free(dev->buffers);
    }
    //关闭设备描述符
    if (-1 != dev->fd) {
        close(dev->fd);
    }
    if (-1 != dev->sub_fd) {
        close(dev->sub_fd);
    }
    return;
}

//退出失败处理
void exit_failure(struct v4l2_dev *dev){
    close_device(dev);
    exit(EXIT_FAILURE);
}

//打开设备
void open_device(struct v4l2_dev *dev){
    dev->fd = open(dev->path, O_RDWR | O_CLOEXEC, 0);
    if (dev->fd < 0) {
        printf("Cannot open %s\n\n", dev->path);
        exit_failure(dev);
    }
    printf("Open %s succeed - %d\n\n", dev->path, dev->fd);

    dev->sub_fd = open(dev->subdev_path, O_RDWR|O_CLOEXEC, 0);
    if (dev->sub_fd < 0) {
        printf("Cannot open %s\n\n", dev->subdev_path);
        exit_failure(dev);
    }
    printf("Open %s succeed\n\n", dev->subdev_path);
    return;
}

//读取采集设备能力
void get_capabilities(struct v4l2_dev *dev){
    struct v4l2_capability cap;
    if (ioctl(dev->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        printf("VIDIOC_QUERYCAP failed\n");
        return;
    }
    printf("------- VIDIOC_QUERYCAP ----\n");
    printf("  driver: %s\n", cap.driver);
    printf("  card: %s\n", cap.card);
    printf("  bus_info: %s\n", cap.bus_info);
    printf("  version: %d.%d.%d\n",
           (cap.version >> 16) & 0xff,
           (cap.version >> 8) & 0xff,
           (cap.version & 0xff));
    printf("  capabilities: %08X\n", cap.capabilities);

    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)
        printf("        Video Capture\n");
    if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)
        printf("        Video Output\n");
    if (cap.capabilities & V4L2_CAP_VIDEO_OVERLAY)
        printf("        Video Overly\n");
    if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        printf("        Video Capture Mplane\n");
    if (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE)
        printf("        Video Output Mplane\n");
    if (cap.capabilities & V4L2_CAP_READWRITE)
        printf("        Read / Write\n");
    if (cap.capabilities & V4L2_CAP_STREAMING)
        printf("        Streaming\n");
    printf("\n");
    return;
}

//设置出图格式
void set_fmt(struct v4l2_dev *dev){ 
    struct v4l2_format fmt;
    struct v4l2_control ctrl;

    //设置出图分辨率
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = dev->buf_type;
    fmt.fmt.pix.pixelformat = dev->format;
    fmt.fmt.pix.width = dev->width;
    fmt.fmt.pix.height = dev->height;
    if (ioctl(dev->fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("VIDIOC_S_FMT failed - [%d]!\n", errno);
        exit_failure(dev);
    }
    printf("VIDIOC_S_FMT succeed!\n");
    dev->data_len = fmt.fmt.pix.sizeimage;
    
    //设置出图方向
    ctrl.id = V4L2_CID_VFLIP;
    ctrl.value = 1;
    if(ioctl(dev->fd, VIDIOC_S_CTRL, &ctrl) < 0){
        printf("V4L2_CID_VFLIP failed - [%d]!\n", errno);
        exit_failure(dev);
    }
    
    return;
}

//申请缓冲区
void require_buf(struct v4l2_dev *dev){
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = dev->req_count;
    req.type = dev->buf_type;
    req.memory = dev->memory_type;
    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) == -1) {
        printf("VIDIOC_REQBUFS failed!\n\n");
        exit_failure(dev);
    }
    if (dev->req_count != req.count) {
        printf("!!! req count = %d\n", req.count);
        dev->req_count = req.count;
    }
    printf("VIDIOC_REQBUFS succeed!\n\n");
    return;
}

//分配缓冲区
void alloc_buf(struct v4l2_dev *dev){
    dev->buffers = (struct buffer *)calloc(dev->req_count, sizeof(*(dev->buffers)));
    for (unsigned int i = 0; i < dev->req_count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        struct v4l2_exportbuffer expbuf;
        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));
        buf.type = dev->buf_type;
        buf.memory = dev->memory_type;
        buf.index = i;

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            printf("VIDIOC_QUERYBUF failed!\n\n");
            exit_failure(dev);
        }
        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type) {
            dev->buffers[i].length = buf.m.planes[0].length;
            dev->buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, buf.m.planes[0].m.mem_offset);
        } else {
            dev->buffers[i].length = buf.length;
            dev->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, buf.m.offset);
        }

        //把buffer导出为dma-buf fd
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type = dev->buf_type;
        expbuf.index = i;
        expbuf.plane = 0;
        expbuf.flags = O_CLOEXEC;
        if(ioctl(dev->fd, VIDIOC_EXPBUF, &expbuf) == -1){
            printf("VIDIOC_EXPBUF failed!\n\n");
        }
        dev->buffers[i].fd = expbuf.fd;

        if (dev->buffers[i].start == MAP_FAILED) {
            printf("Memory map failed!\n\n");
            exit_failure(dev);
        }
    }
    printf("Memory map succeed!\n\n");
    return;
}

//将缓冲区入队
void queue_buf(struct v4l2_dev *dev){
    for (unsigned int i = 0; i < dev->req_count; ++i) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));
        buf.type = dev->buf_type;
        buf.memory = dev->memory_type;
        buf.index = i;

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
            printf("VIDIOC_QBUF failed!\n\n");
            exit_failure(dev);
        }
    }
    printf("VIDIOC_QBUF succeed!\n\n");
    return;
}

//开启图像帧
void stream_on(struct v4l2_dev *dev){
    enum v4l2_buf_type type = dev->buf_type;
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) == -1) {
        printf("VIDIOC_STREAMON failed!\n\n");
        exit_failure(dev);
    }
    printf("VIDIOC_STREAMON succeed!\n\n");
}

//预热图像帧
void ready_frame(struct v4l2_dev *dev, int skip_frame){
    struct v4l2_buffer buf;
    struct v4l2_plane planes[FMT_NUM_PLANES];
    for (int i = 0; i <= skip_frame; ++i) {
        memset(&buf, 0, sizeof(buf));
        buf.type = dev->buf_type;
        buf.memory = dev->memory_type;

        if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type) {
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) == -1) {
            printf("VIDIOC_DQBUF failed!\n\n");
            exit_failure(dev);
        }

        dev->out_data = (unsigned char *)dev->buffers[buf.index].start;
        dev->timestamp = buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
        // printf("image: sequence = %d, timestamp = %lu\n", buf.sequence, dev->timestamp);

        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) == -1) {
            printf("VIDIOC_QBUF failed!\n");
            exit_failure(dev);
        }
    }
}

//保存图像
void save_picture(const char *filename, unsigned char *file_data, unsigned int len, int is_overwrite){
    FILE *fp;
    if (is_overwrite)
        fp = fopen(filename, "wb");
    else
        fp = fopen(filename, "ab");
    if (fp < 0) {
        printf("Open frame data file failed\n\n");
        return;
    }
    if (fwrite(file_data, 1, len, fp) < len) {
        printf("Out of memory!\n");
    }
    fflush(fp);
    fclose(fp);
    printf("Save one frame to %s succeed!\n\n", filename);
}

//关闭图像帧输出
void stream_off(struct v4l2_dev *dev){
    enum v4l2_buf_type type;
    type = dev->buf_type;
    if (ioctl(dev->fd, VIDIOC_STREAMOFF, &type) == -1) {
        printf("VIDIOC_STREAMOFF failed!\n\n");
        exit_failure(dev);
    }
    printf("VIDIOC_STREAMOFF succeed!\n\n");
}

//设置帧率
void set_fps(struct v4l2_dev *dev, unsigned int fps){
	int ret;
	struct v4l2_subdev_frame_interval frame_int;

    if (fps == 0) return;

	memset(&frame_int, 0x00, sizeof(frame_int));

	frame_int.interval.numerator = 10000;
	frame_int.interval.denominator = fps * 10000;	

	ret = ioctl(dev->sub_fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &frame_int);
	if (ret < 0) {
		printf("VIDIOC_SUBDEV_S_FRAME_INTERVAL error\n");
        goto set_fps_err;
	}
    printf("VIDIOC_SUBDEV_S_FRAME_INTERVAL [%u fps] OK\n", fps);
set_fps_err:
    return;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

//采集缩放并编码frame_num帧 
void encode_frame(struct v4l2_dev *dev, void *enc, int frame_num, int use_dmaheap){
    int i = 0;
    double t0 = now_sec();

    //为RGA输出申请一个dmabuf-fd
    int dst_fd = -1;
    MppBuffer dst_buf = NULL;   //mpp_buffer_get得到的句柄
    MppBuffer enc_buf = NULL;   //交给编码器的句柄，整个循环复用
    size_t rga_dst_w = 1920;
    size_t rga_dst_h = 1080;
    size_t rga_dst_size = (size_t)(rga_dst_h * rga_dst_w * 3 / 2);

    if(use_dmaheap){
        dst_fd = dma_heap_alloc("/dev/dma_heap/cma", rga_dst_size);
        if(dst_fd < 0){
            return;
        }

        //dma-buf fd 只在初始化时导入一次，循环里不再 import
        if(mpp_init_buffer(dst_fd, &enc_buf, rga_dst_size, MPP_BUFFER_TYPE_DMA_HEAP) != 0){
            printf("mpp_init_buffer failed\n");
            close(dst_fd);
            return;
        }
    }
    else{
        MPP_RET mret;

        mret = mpp_buffer_get(NULL, &dst_buf, rga_dst_size);
        if(mret != MPP_OK || dst_buf == NULL){
            printf("mpp_buffer_get failed: %d\n", mret);
            return;
        }

        dst_fd = mpp_buffer_get_fd(dst_buf);
        if(dst_fd < 0){
            printf("mpp_buffer_get_fd failed\n");
            mpp_buffer_put(dst_buf);
            return;    
        }

        //mpp_buffer_get返回的已经是MppBuffer，直接复用，无需再次import
        enc_buf = dst_buf;
    }

    //采集-RGA-MPP
    for(i = 0; i < frame_num; i++){
        struct v4l2_buffer buf;
        struct v4l2_plane planes[FMT_NUM_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = dev->buf_type;
        buf.memory = dev->memory_type;

        if(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == dev->buf_type){
            buf.m.planes = planes;
            buf.length = FMT_NUM_PLANES;
        }

        //V4L2 DQBUF
        if(ioctl(dev->fd, VIDIOC_DQBUF, &buf) == -1){
            printf("VIDIOC_DQBUF failed!\n\n");
            break;
        }

        //rga resize
        if(rga_resize_nv12(dev->buffers[buf.index].fd, dev->width, dev->height, dev->width, 
            dst_fd, rga_dst_w, rga_dst_h, rga_dst_w
        ) != 0){
            printf("rga_resize_nv12 failed at frame %d\n", i);
            ioctl(dev->fd, VIDIOC_QBUF, &buf);
            break;
        }
        
        //mpp编码：循环里只换buffer，不再每帧import/put
        if(mpp_encoder_encode(enc, enc_buf) != 0){
            printf("mpp_encoder_encode failed at frame %d\n", i);
            ioctl(dev->fd, VIDIOC_QBUF, &buf);
            break;
        }

        if(ioctl(dev->fd, VIDIOC_QBUF, &buf) == -1){
            printf("VIDIOC_QBUF failed!\n\n");
            break;
        }
    }
    double elapsed = now_sec() - t0;

    //析构：释放导入的MppBuffer，再按分配方式释放底层dma-buf
    if(use_dmaheap){
        mpp_buffer_put(enc_buf);
        close(dst_fd);
    }
    else{
        mpp_buffer_put(dst_buf); //enc_buf与dst_buf是同一个句柄
    }

    printf("encode %d frames in %.3f s\n", frame_num, elapsed);
    printf("fps_ave = %.2f\n", elapsed > 0 ? frame_num / elapsed : 0.0);
}
