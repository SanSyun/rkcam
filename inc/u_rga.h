#ifndef U_RGA_H
#define U_RGA_H

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

#include "im2d.h"

//图像缩放
int rga_resize_nv12(int src_fd, int src_w, int src_h, int src_wstride, 
    int dst_fd, int dst_w, int dst_h, int dst_wstride);



#endif 