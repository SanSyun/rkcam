#ifndef U_DMA_H
#define U_DMA_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <linux/stddef.h>
#include <sys/ioctl.h>

#include <linux/dma-heap.h>

int dma_heap_alloc(const char *path, size_t size);

/*
 * CPU 访问 dma-buf 前后的缓存同步。
 * write_access=0：CPU 只读（如 RGA 输出）；=1：CPU 写（如 MPP 输入）。
 * 内核不支持 DMA_BUF_IOCTL_SYNC 时按成功处理，避免在不支持的内核上直接失败。
 */
int dma_buf_cpu_begin(int fd, int write_access);
int dma_buf_cpu_end(int fd, int write_access);

#endif