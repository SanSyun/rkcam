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

#endif