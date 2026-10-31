#include "u_dma.h"

#include <linux/dma-buf.h>

int dma_heap_alloc(const char *path, size_t size){
    struct dma_heap_allocation_data data;
    int heap_fd;
    int fd;

    memset(&data, 0, sizeof(data));
    data.len       = size;
    data.fd_flags  = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;

    heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC failed: %s\n", strerror(errno));
        close(heap_fd);
        return -1;
    }

    close(heap_fd);  //分配完可以关掉
    fd = (int)data.fd;

    if (fd < 0) {
        fprintf(stderr, "invalid dma-buf fd: %d\n", fd);
        return -1;
    }
    return fd;
}

static int dma_buf_sync_flag(int fd, uint32_t flags)
{
    struct dma_buf_sync sync;
    int ret;

    if (fd < 0) {
        return -1;
    }

    memset(&sync, 0, sizeof(sync));
    sync.flags = flags;
    ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret < 0) {
        if (errno == ENOTTY || errno == EINVAL) {
            return 0;   /* 该内核/驱动不支持显式同步 */
        }
        return -1;
    }

    return 0;
}

int dma_buf_cpu_begin(int fd, int write_access)
{
    uint32_t dir = write_access ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;

    return dma_buf_sync_flag(fd, DMA_BUF_SYNC_START | dir);
}

int dma_buf_cpu_end(int fd, int write_access)
{
    uint32_t dir = write_access ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;

    return dma_buf_sync_flag(fd, DMA_BUF_SYNC_END | dir);
}