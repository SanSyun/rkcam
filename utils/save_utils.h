#ifndef SAVE_UTILS_H
#define SAVE_UTILS_H

#include <stddef.h>
#include <stdio.h>

#define SAVE_PATH_MAX 512

typedef struct {
    FILE *fp;
    char  path[SAVE_PATH_MAX];
    unsigned long long bytes;
} file_saver_t;

/* 打开一个流式保存器：append=0 覆盖，append=1 追加 */
int  file_saver_open(file_saver_t *s, const char *path, int append);
int  file_saver_write(file_saver_t *s, const void *data, size_t len);
int  file_saver_flush(file_saver_t *s);
void file_saver_close(file_saver_t *s);

/* 一次性保存：给路径 + 数据，内部 open/write/close，后续存 JPEG/PNG/裸流也能复用 */
int  save_buffer_to_file(const char *path, const void *data, size_t len, int append);

#endif