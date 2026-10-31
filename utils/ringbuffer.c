#include "ringbuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* 计算绝对超时时间点，必须和 cond 的 CLOCK_MONOTONIC 属性一致 */
static void rb_set_deadline(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec  += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* 锁内等待条件变量，返回 0 被唤醒，-1 超时。调用前需持有锁 */
static int rb_wait_locked(struct ringbuffer *rb, const struct timespec *deadline)
{
    int ret;

    if (deadline == NULL) {
        ret = pthread_cond_wait(&rb->cond, &rb->lock);
    } else {
        ret = pthread_cond_timedwait(&rb->cond, &rb->lock, deadline);
    }

    return (ret == ETIMEDOUT) ? -1 : 0;
}

/* 从 pos 处写入 len 字节，自动处理回绕，调用前需持有锁 */
static void rb_store(struct ringbuffer *rb, size_t pos, const unsigned char *src, size_t len)
{
    size_t first = rb->size - pos;

    if (first > len) {
        first = len;
    }

    memcpy(rb->data + pos, src, first);
    if (len > first) {
        memcpy(rb->data, src + first, len - first);   //绕回缓冲区头部
    }
}

/* 从 pos 处读出 len 字节，自动处理回绕，调用前需持有锁 */
static void rb_load(struct ringbuffer *rb, size_t pos, unsigned char *dst, size_t len)
{
    size_t first = rb->size - pos;

    if (first > len) {
        first = len;
    }

    memcpy(dst, rb->data + pos, first);
    if (len > first) {
        memcpy(dst + first, rb->data, len - first);   //绕回缓冲区头部
    }
}

/* 写入 len 字节，返回实际写入量，调用前需持有锁 */
static size_t rb_write_locked(struct ringbuffer *rb, const unsigned char *src, size_t len)
{
    size_t space;
    size_t n;

    if (len > rb->size) {                 //比整个缓冲区还大，只保留最后 size 字节
        rb->dropped += len - rb->size;    //前面这段直接丢掉，计入统计
        src += len - rb->size;
        len  = rb->size;
    }

    if (rb->overwrite && rb->used + len > rb->size) {
        size_t drop = rb->used + len - rb->size;   //腾地方，丢掉最旧的数据

        rb->tail = (rb->tail + drop) % rb->size;
        rb->used -= drop;
        rb->dropped += drop;
    }

    space = rb->size - rb->used;
    n = (len < space) ? len : space;

    rb_store(rb, rb->head, src, n);
    rb->head = (rb->head + n) % rb->size;
    rb->used += n;
    rb->total_in += n;

    return n;
}

/* 读出 len 字节，返回实际读出量，调用前需持有锁 */
static size_t rb_read_locked(struct ringbuffer *rb, unsigned char *dst, size_t len)
{
    size_t n = (len < rb->used) ? len : rb->used;

    rb_load(rb, rb->tail, dst, n);
    rb->tail = (rb->tail + n) % rb->size;
    rb->used -= n;
    rb->total_out += n;

    return n;
}

int rb_init(struct ringbuffer *rb, size_t size){
    pthread_condattr_t attr;
    int ret;

    if (rb == NULL || size == 0) {
        fprintf(stderr, "rb_init: invalid param, size = %zu\n", size);
        return -1;
    }

    memset(rb, 0, sizeof(*rb));

    rb->data = (unsigned char *)malloc(size);
    if (rb->data == NULL) {
        fprintf(stderr, "rb_init: malloc %zu bytes failed\n", size);
        return -1;
    }
    rb->size = size;

    ret = pthread_mutex_init(&rb->lock, NULL);
    if (ret != 0) {
        fprintf(stderr, "rb_init: pthread_mutex_init failed: %s\n", strerror(ret));
        free(rb->data);
        memset(rb, 0, sizeof(*rb));
        return -1;
    }

    /* 用单调时钟，避免系统时间被改后 timedwait 算错超时 */
    ret = pthread_condattr_init(&attr);
    if (ret != 0) {
        fprintf(stderr, "rb_init: pthread_condattr_init failed: %s\n", strerror(ret));
        pthread_mutex_destroy(&rb->lock);
        free(rb->data);
        memset(rb, 0, sizeof(*rb));
        return -1;
    }

    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    ret = pthread_cond_init(&rb->cond, &attr);
    pthread_condattr_destroy(&attr);

    if (ret != 0) {
        fprintf(stderr, "rb_init: pthread_cond_init failed: %s\n", strerror(ret));
        pthread_mutex_destroy(&rb->lock);
        free(rb->data);
        memset(rb, 0, sizeof(*rb));
        return -1;
    }

    return 0;
}

void rb_deinit(struct ringbuffer *rb){
    if (rb == NULL || rb->data == NULL) {
        return;
    }

    pthread_cond_destroy(&rb->cond);
    pthread_mutex_destroy(&rb->lock);
    free(rb->data);
    memset(rb, 0, sizeof(*rb));
}

/* 写入数据，不阻塞。返回实际写入字节数，0 表示写满(或已 stop) */
size_t rb_write(struct ringbuffer *rb, const void *data, size_t len){
    size_t n;

    if (rb == NULL || data == NULL || rb->data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);

    if (rb->stop) {
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }

    n = rb_write_locked(rb, (const unsigned char *)data, len);
    if (n > 0) {
        pthread_cond_broadcast(&rb->cond);
    }

    pthread_mutex_unlock(&rb->lock);

    return n;
}

/* 读出数据，不阻塞。返回实际读出字节数，0 表示没有数据 */
size_t rb_read(struct ringbuffer *rb, void *buf, size_t len){
    size_t n;

    if (rb == NULL || buf == NULL || rb->data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    n = rb_read_locked(rb, (unsigned char *)buf, len);
    if (n > 0) {
        pthread_cond_broadcast(&rb->cond);
    }
    pthread_mutex_unlock(&rb->lock);

    return n;
}

/* 只读不取，用于先看包头再决定读多少 */
size_t rb_peek(struct ringbuffer *rb, void *buf, size_t len){
    size_t n;

    if (rb == NULL || buf == NULL || rb->data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    n = (len < rb->used) ? len : rb->used;
    rb_load(rb, rb->tail, (unsigned char *)buf, n);
    pthread_mutex_unlock(&rb->lock);

    return n;
}

/* 丢弃 len 字节，用于跳过不需要的数据。返回实际丢弃量 */
size_t rb_skip(struct ringbuffer *rb, size_t len){
    size_t n;

    if (rb == NULL || rb->data == NULL || len == 0) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    n = (len < rb->used) ? len : rb->used;
    rb->tail = (rb->tail + n) % rb->size;
    rb->used -= n;
    if (n > 0) {
        pthread_cond_broadcast(&rb->cond);
    }
    pthread_mutex_unlock(&rb->lock);

    return n;
}

/*
 * 阻塞写入 len 字节，直到写完。
 * timeout_ms < 0 一直等；= 0 只试一次；> 0 最多等 timeout_ms 毫秒。
 * 返回 0 成功  -1 超时/已 stop/len 超过缓冲区大小
 */
int rb_write_block(struct ringbuffer *rb, const void *data, size_t len, int timeout_ms){
    const unsigned char *src = (const unsigned char *)data;
    struct timespec deadline;
    int has_deadline = (timeout_ms >= 0);
    size_t off = 0;
    int ret = 0;

    if (rb == NULL || data == NULL || rb->data == NULL) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (len > rb->size) {
        return -1;
    }

    if (has_deadline) {
        rb_set_deadline(&deadline, timeout_ms);
    }

    pthread_mutex_lock(&rb->lock);

    while (off < len) {
        size_t n;

        if (rb->stop) {
            ret = -1;
            break;
        }

        n = rb_write_locked(rb, src + off, len - off);
        off += n;
        if (n > 0) {
            pthread_cond_broadcast(&rb->cond);
        }

        if (off == len) {
            break;
        }

        if (rb_wait_locked(rb, has_deadline ? &deadline : NULL) != 0) {
            ret = -1;
            break;
        }
    }

    pthread_mutex_unlock(&rb->lock);

    return ret;
}

/*
 * 阻塞读出 len 字节，拿够 len 字节才算成功(超时不会读走半截数据)。
 * timeout_ms < 0 一直等；= 0 只试一次；> 0 最多等 timeout_ms 毫秒。
 * 返回 0 成功  -1 超时/已 stop/len 超过缓冲区大小
 */
int rb_read_block(struct ringbuffer *rb, void *buf, size_t len, int timeout_ms){
    struct timespec deadline;
    int has_deadline = (timeout_ms >= 0);
    int ret = 0;

    if (rb == NULL || buf == NULL || rb->data == NULL) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (len > rb->size) {
        return -1;
    }

    if (has_deadline) {
        rb_set_deadline(&deadline, timeout_ms);
    }

    pthread_mutex_lock(&rb->lock);

    while (rb->used < len) {
        if (rb->stop) {
            ret = -1;
            break;
        }

        if (rb_wait_locked(rb, has_deadline ? &deadline : NULL) != 0) {
            ret = -1;
            break;
        }
    }

    if (ret == 0) {
        rb_read_locked(rb, (unsigned char *)buf, len);
        pthread_cond_broadcast(&rb->cond);
    }

    pthread_mutex_unlock(&rb->lock);

    return ret;
}

size_t rb_used(struct ringbuffer *rb){
    size_t used;

    if (rb == NULL || rb->data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    used = rb->used;
    pthread_mutex_unlock(&rb->lock);

    return used;
}

size_t rb_space(struct ringbuffer *rb){
    size_t space;

    if (rb == NULL || rb->data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&rb->lock);
    space = rb->size - rb->used;
    pthread_mutex_unlock(&rb->lock);

    return space;
}

int rb_is_empty(struct ringbuffer *rb){
    return rb_used(rb) == 0;
}

int rb_is_full(struct ringbuffer *rb){
    return rb_space(rb) == 0;
}

/* 清空数据，不释放内存，也不清统计计数 */
void rb_reset(struct ringbuffer *rb){
    if (rb == NULL || rb->data == NULL) {
        return;
    }

    pthread_mutex_lock(&rb->lock);
    rb->head  = 0;
    rb->tail  = 0;
    rb->used  = 0;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}

/* 1: 写满时覆盖最旧数据(适合只关心最新数据的场景)  0: 丢弃新数据(默认) */
void rb_set_overwrite(struct ringbuffer *rb, int enable){
    if (rb == NULL || rb->data == NULL) {
        return;
    }

    pthread_mutex_lock(&rb->lock);
    rb->overwrite = (enable != 0);
    pthread_mutex_unlock(&rb->lock);
}

/* 退出时调用，唤醒所有阻塞在 rb_read_block/rb_write_block 上的线程 */
void rb_stop(struct ringbuffer *rb){
    if (rb == NULL || rb->data == NULL) {
        return;
    }

    pthread_mutex_lock(&rb->lock);
    rb->stop = 1;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}
