#include "u_ffmpeg.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>

#define U_FFMPEG_CONNECT_TIMEOUT_US 1000000
#define U_FFMPEG_RETRY_MS           2000

static void ff_log_av(const char *what, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];

    if (av_strerror(err, buf, sizeof(buf)) < 0)
        snprintf(buf, sizeof(buf), "error %d", err);
    fprintf(stderr, "[rtmp] %s: %s\n", what, buf);
}

/*
 * 从 Annex-B 码流中取下一个 NAL。
 * 兼容 3/4 字节起始码；*pos 传入上次位置，返回 1 表示取到。
 */
static int next_nal(const uint8_t *data, size_t len, size_t *pos,
                    const uint8_t **nal, size_t *nal_len)
{
    size_t i = *pos;

    for (;;) {
        size_t payload;
        size_t j;
        size_t end;

        while (i + 3 <= len) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                break;
            i++;
        }
        if (i + 3 > len)
            return 0;

        payload = i + 3;
        if (payload > len)
            return 0;

        j = payload;
        while (j + 3 <= len) {
            if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1)
                break;
            j++;
        }

        if (j + 3 <= len) {
            end = j;
            /* 4 字节起始码（00 00 00 01）最前面的 0 不属于上一个 NAL */
            if (end > payload && data[end - 1] == 0)
                end--;
        } else {
            end = len;
        }

        if (end > payload) {
            *nal = data + payload;
            *nal_len = end - payload;
            *pos = (j + 3 <= len) ? j : len;
            return 1;
        }

        if (j + 3 > len)
            return 0;
        i = j;   /* 空 NAL，跳过继续 */
    }
}

/* 从 SPS/PPS（Annex-B）生成 AVCDecoderConfigurationRecord */
static int build_avcc(const uint8_t *data, size_t len,
                      uint8_t **out, size_t *out_len)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    const uint8_t *nal;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t nal_len;
    size_t pos = 0;
    size_t size;
    size_t o = 0;
    uint8_t *buf;

    while (next_nal(data, len, &pos, &nal, &nal_len)) {
        int type = nal[0] & 0x1f;

        if (type == 7 && sps == NULL) {
            sps = nal;
            sps_len = nal_len;
        } else if (type == 8 && pps == NULL) {
            pps = nal;
            pps_len = nal_len;
        }
    }

    if (sps == NULL || pps == NULL || sps_len < 4)
        return -1;

    size = 11 + sps_len + pps_len;
    buf = (uint8_t *)malloc(size);
    if (buf == NULL)
        return -1;

    buf[o++] = 1;                         /* configurationVersion */
    buf[o++] = sps[1];                    /* AVCProfileIndication */
    buf[o++] = sps[2];                    /* profile_compatibility */
    buf[o++] = sps[3];                    /* AVCLevelIndication */
    buf[o++] = 0xFC | 3;                  /* lengthSizeMinusOne = 3 */
    buf[o++] = 0xE0 | 1;                  /* numOfSequenceParameterSets = 1 */
    buf[o++] = (uint8_t)(sps_len >> 8);
    buf[o++] = (uint8_t)(sps_len & 0xff);
    memcpy(buf + o, sps, sps_len);
    o += sps_len;
    buf[o++] = 1;                         /* numOfPictureParameterSets */
    buf[o++] = (uint8_t)(pps_len >> 8);
    buf[o++] = (uint8_t)(pps_len & 0xff);
    memcpy(buf + o, pps, pps_len);
    o += pps_len;

    *out = buf;
    *out_len = o;
    return 0;
}

/* 关闭当前 RTMP 会话；write_trailer 只在正常收尾时置 1 */
static void ff_disconnect(u_ffmpeg_t *ff, int write_trailer)
{
    if (ff == NULL)
        return;

    if (ff->fmt != NULL) {
        if (ff->fmt->pb != NULL) {
            if (write_trailer && ff->connected)
                (void)av_write_trailer(ff->fmt);
            avio_closep(&ff->fmt->pb);
        }
        avformat_free_context(ff->fmt);
    }

    ff->fmt = NULL;
    ff->st = NULL;
    ff->connected = 0;
}

/* 建立一次 FLV/RTMP 会话：open + write_header */
static int ff_connect(u_ffmpeg_t *ff)
{
    AVFormatContext *fmt = NULL;
    AVStream *st;
    AVDictionary *opts = NULL;
    char timeout[32];
    int ret;

    ret = avformat_alloc_output_context2(&fmt, NULL, "flv", ff->url);
    if (ret < 0) {
        ff_log_av("avformat_alloc_output_context2", ret);
        return -1;
    }
    if (fmt == NULL) {
        fprintf(stderr, "[rtmp] no flv muxer\n");
        return -1;
    }

    st = avformat_new_stream(fmt, NULL);
    if (st == NULL) {
        fprintf(stderr, "[rtmp] avformat_new_stream failed\n");
        avformat_free_context(fmt);
        return -1;
    }

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_H264;
    st->codecpar->codec_tag  = 0;
    st->codecpar->width      = ff->width;
    st->codecpar->height     = ff->height;
    st->codecpar->format     = AV_PIX_FMT_YUV420P;

    st->codecpar->extradata =
        (uint8_t *)av_mallocz(ff->extradata_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (st->codecpar->extradata == NULL) {
        fprintf(stderr, "[rtmp] alloc extradata failed\n");
        avformat_free_context(fmt);
        return -1;
    }
    memcpy(st->codecpar->extradata, ff->extradata, ff->extradata_len);
    st->codecpar->extradata_size = (int)ff->extradata_len;

    st->time_base      = (AVRational){1, 1000};
    st->avg_frame_rate = (AVRational){ff->fps, 1};
    st->r_frame_rate   = (AVRational){ff->fps, 1};

    snprintf(timeout, sizeof(timeout), "%d", U_FFMPEG_CONNECT_TIMEOUT_US);
    (void)av_dict_set(&opts, "rw_timeout", timeout, 0);
    ret = avio_open2(&fmt->pb, ff->url, AVIO_FLAG_WRITE, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        ff_log_av("avio_open2", ret);
        avformat_free_context(fmt);
        return -1;
    }

    ret = avformat_write_header(fmt, NULL);
    if (ret < 0) {
        ff_log_av("avformat_write_header", ret);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return -1;
    }

    ff->fmt = fmt;
    ff->st = st;
    ff->connected = 1;
    ff->wait_key = 1;
    return 0;
}

int u_ffmpeg_init(u_ffmpeg_t *ff, const char *url,
                  int width, int height, int fps,
                  const void *sps_pps, size_t sps_pps_len)
{
    int ret;

    if (ff == NULL)
        return -1;

    memset(ff, 0, sizeof(*ff));
    ff->start_pts_us = -1;
    ff->last_dts_ms = -1;

    if (url == NULL || url[0] == '\0' ||
        sps_pps == NULL || sps_pps_len == 0 ||
        width <= 0 || height <= 0) {
        fprintf(stderr, "[rtmp] invalid init args\n");
        return -1;
    }

    if (strlen(url) >= sizeof(ff->url)) {
        fprintf(stderr, "[rtmp] url too long\n");
        return -1;
    }
    snprintf(ff->url, sizeof(ff->url), "%s", url);

    ret = avformat_network_init();
    if (ret < 0) {
        ff_log_av("avformat_network_init", ret);
        return -1;
    }
    ff->net_inited = 1;

    if (build_avcc((const uint8_t *)sps_pps, sps_pps_len,
                   &ff->extradata, &ff->extradata_len) != 0) {
        fprintf(stderr, "[rtmp] no SPS/PPS in encoder header\n");
        goto fail;
    }

    ff->pkt = av_packet_alloc();
    if (ff->pkt == NULL) {
        fprintf(stderr, "[rtmp] av_packet_alloc failed\n");
        goto fail;
    }

    ff->width  = width;
    ff->height = height;
    ff->fps    = (fps > 0) ? fps : 30;
    ff->configured = 1;

    fprintf(stderr, "[rtmp] init %s %dx%d@%d, connect on first frame\n",
            ff->url, ff->width, ff->height, ff->fps);
    return 0;

fail:
    free(ff->extradata);
    ff->extradata = NULL;
    ff->extradata_len = 0;
    if (ff->pkt != NULL)
        av_packet_free(&ff->pkt);
    if (ff->net_inited) {
        avformat_network_deinit();
        ff->net_inited = 0;
    }
    return -1;
}

int u_ffmpeg_push(u_ffmpeg_t *ff)
{
    int64_t now;

    if (ff == NULL || !ff->configured)
        return 0;

    if (ff->connected && ff->fmt != NULL && ff->st != NULL)
        return 1;

    ff->connected = 0;

    now = av_gettime_relative() / 1000;
    if (ff->last_conn_ms != 0 && now - ff->last_conn_ms < U_FFMPEG_RETRY_MS)
        return 0;

    if (ff_connect(ff) != 0) {
        ff->last_conn_ms = av_gettime_relative() / 1000;
        fprintf(stderr, "[rtmp] connect failed, retry in %d ms\n",
                U_FFMPEG_RETRY_MS);
        return 0;
    }

    ff->last_conn_ms = 0;
    fprintf(stderr, "[rtmp] connected: %s\n", ff->url);
    return 1;
}

int u_ffmpeg_mux(u_ffmpeg_t *ff, const void *data, size_t len,
                 uint64_t pts_us)
{
    const uint8_t *buf = (const uint8_t *)data;
    const uint8_t *nal;
    size_t nal_len;
    size_t pos;
    size_t total = 0;
    size_t o;
    int has_idr = 0;
    int has_vcl = 0;
    int64_t pts_ms;
    int64_t dts_ms;
    int ret;

    if (ff == NULL || !ff->configured || !ff->connected ||
        ff->fmt == NULL || ff->st == NULL)
        return 0;
    if (buf == NULL || len == 0)
        return 0;

    /* 第一遍：统计 AVCC 长度、判断 IDR / 是否含 VCL */
    pos = 0;
    while (next_nal(buf, len, &pos, &nal, &nal_len)) {
        int type = nal[0] & 0x1f;

        total += 4 + nal_len;
        if (type == 5)
            has_idr = 1;
        if (type == 1 || type == 5)
            has_vcl = 1;
    }

    if (total == 0 || !has_vcl || total > (size_t)INT_MAX) {
        ff->dropped++;
        return 0;
    }

    /* 重连后先等 IDR，避免新连接从 P 帧开始 */
    if (ff->wait_key && !has_idr) {
        ff->dropped++;
        return 0;
    }

    ret = av_new_packet(ff->pkt, (int)total);
    if (ret < 0) {
        ff_log_av("av_new_packet", ret);
        return -1;
    }

    /* 第二遍：起始码替换成 4 字节大端长度 */
    pos = 0;
    o = 0;
    while (next_nal(buf, len, &pos, &nal, &nal_len)) {
        ff->pkt->data[o++] = (uint8_t)(nal_len >> 24);
        ff->pkt->data[o++] = (uint8_t)(nal_len >> 16);
        ff->pkt->data[o++] = (uint8_t)(nal_len >> 8);
        ff->pkt->data[o++] = (uint8_t)(nal_len & 0xff);
        memcpy(ff->pkt->data + o, nal, nal_len);
        o += nal_len;
    }
    ff->pkt->size = (int)o;

    if (ff->start_pts_us < 0)
        ff->start_pts_us = (int64_t)pts_us;

    pts_ms = (int64_t)((pts_us - (uint64_t)ff->start_pts_us) / 1000);
    if (pts_ms < 0)
        pts_ms = 0;

    dts_ms = pts_ms;
    if (dts_ms <= ff->last_dts_ms)
        dts_ms = ff->last_dts_ms + 1;

    ff->pkt->pts = av_rescale_q(dts_ms, (AVRational){1, 1000},
                                ff->st->time_base);
    ff->pkt->dts = ff->pkt->pts;
    ff->pkt->duration = av_rescale_q(1000 / ff->fps, (AVRational){1, 1000},
                                     ff->st->time_base);
    ff->pkt->stream_index = ff->st->index;
    ff->pkt->flags = has_idr ? AV_PKT_FLAG_KEY : 0;

    ret = av_interleaved_write_frame(ff->fmt, ff->pkt);
    if (ret < 0) {
        ff_log_av("av_interleaved_write_frame", ret);
        av_packet_unref(ff->pkt);
        ff_disconnect(ff, 0);
        ff->last_conn_ms = av_gettime_relative() / 1000;
        ff->dropped++;
        return 0;
    }

    av_packet_unref(ff->pkt);
    ff->wait_key = 0;
    ff->last_dts_ms = dts_ms;
    ff->frames++;
    return 1;
}

void u_ffmpeg_deinit(u_ffmpeg_t *ff)
{
    if (ff == NULL)
        return;

    ff_disconnect(ff, 1);

    if (ff->pkt != NULL)
        av_packet_free(&ff->pkt);

    free(ff->extradata);
    ff->extradata = NULL;
    ff->extradata_len = 0;

    if (ff->configured) {
        fprintf(stderr, "[rtmp] done, sent=%llu dropped=%llu\n",
                (unsigned long long)ff->frames,
                (unsigned long long)ff->dropped);
    }
    ff->configured = 0;

    if (ff->net_inited) {
        avformat_network_deinit();
        ff->net_inited = 0;
    }
}