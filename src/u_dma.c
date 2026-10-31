#include "u_dma.h"

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