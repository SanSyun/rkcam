#include "save_utils.h"

#include <errno.h>
#include <string.h>

int file_saver_open(file_saver_t *s, const char *path, int append)
{
    if (s == NULL || path == NULL || path[0] == '\0') {
        fprintf(stderr, "file_saver_open: invalid argument\n");
        return -1;
    }

    memset(s, 0, sizeof(*s));
    if (snprintf(s->path, sizeof(s->path), "%s", path) >= (int)sizeof(s->path)) {
        fprintf(stderr, "file_saver_open: path too long: %s\n", path);
        return -1;
    }

    s->fp = fopen(path, append ? "ab" : "wb");
    if (s->fp == NULL) {
        fprintf(stderr, "file_saver_open: fopen %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    /* 大缓冲减少写盘次数；失败也不影响正确性 */
    setvbuf(s->fp, NULL, _IOFBF, 1024 * 1024);

    return 0;
}

int file_saver_write(file_saver_t *s, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;

    if (s == NULL || s->fp == NULL) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (p == NULL) {
        return -1;
    }

    while (left > 0) {
        size_t n = fwrite(p, 1, left, s->fp);

        if (n == 0) {
            fprintf(stderr, "file_saver_write: write %s failed: %s\n",
                    s->path, strerror(errno));
            return -1;
        }
        p += n;
        left -= n;
        s->bytes += n;
    }

    return 0;
}

int file_saver_flush(file_saver_t *s)
{
    if (s == NULL || s->fp == NULL) {
        return -1;
    }

    if (fflush(s->fp) != 0) {
        fprintf(stderr, "file_saver_flush: flush %s failed: %s\n",
                s->path, strerror(errno));
        return -1;
    }

    return 0;
}

void file_saver_close(file_saver_t *s)
{
    if (s == NULL || s->fp == NULL) {
        return;
    }

    if (fflush(s->fp) != 0) {
        fprintf(stderr, "file_saver_close: flush %s failed: %s\n",
                s->path, strerror(errno));
    }
    if (fclose(s->fp) != 0) {
        fprintf(stderr, "file_saver_close: fclose %s failed: %s\n",
                s->path, strerror(errno));
    }
    s->fp = NULL;
}

int save_buffer_to_file(const char *path, const void *data, size_t len, int append)
{
    file_saver_t saver;
    int ret = 0;

    if (file_saver_open(&saver, path, append) != 0) {
        return -1;
    }

    if (file_saver_write(&saver, data, len) != 0) {
        ret = -1;
    }

    file_saver_close(&saver);
    return ret;
}