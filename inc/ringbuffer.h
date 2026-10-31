#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stddef.h>
#include <pthread.h>

/*
 * 通用字节环形缓冲区，线程安全。
 * 一端 rb_write 写、另一端 rb_read 读，读写可以在不同线程。
 * 默认写满后丢弃新数据；rb_set_overwrite(rb, 1) 后改为丢弃最旧的数据。
 * 只处理裸字节流，不关心数据内容，帧/包/命令都可以当字节写进来。
 */
struct ringbuffer{
    unsigned char     *data;       //数据区，rb_init 时分配
    size_t             size;       //数据区总大小
    size_t             head;       //写位置
    size_t             tail;       //读位置
    size_t             used;       //已用字节数
    int                overwrite;  //1:写满时覆盖最旧数据
    int                stop;       //rb_stop 后阻塞的读写立即返回
    unsigned long long dropped;    //丢掉的字节数(写满或单次写入超过缓冲区)
    unsigned long long total_in;   //累计写入字节数
    unsigned long long total_out;  //累计读出字节数
    pthread_mutex_t    lock;
    pthread_cond_t     cond;
};

int    rb_init(struct ringbuffer *rb, size_t size);
void   rb_deinit(struct ringbuffer *rb);

size_t rb_write(struct ringbuffer *rb, const void *data, size_t len);
size_t rb_read(struct ringbuffer *rb, void *buf, size_t len);
size_t rb_peek(struct ringbuffer *rb, void *buf, size_t len);
size_t rb_skip(struct ringbuffer *rb, size_t len);

int    rb_write_block(struct ringbuffer *rb, const void *data, size_t len, int timeout_ms);
int    rb_read_block(struct ringbuffer *rb, void *buf, size_t len, int timeout_ms);

size_t rb_used(struct ringbuffer *rb);
size_t rb_space(struct ringbuffer *rb);
int    rb_is_empty(struct ringbuffer *rb);
int    rb_is_full(struct ringbuffer *rb);

void   rb_reset(struct ringbuffer *rb);
void   rb_set_overwrite(struct ringbuffer *rb, int enable);
void   rb_stop(struct ringbuffer *rb);

#endif
