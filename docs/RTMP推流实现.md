# RTMP 推流实现

> 工程：`mediaCam`（RK3568 + IMX415 + RGA + MPP）
> 代码：`inc/u_ffmpeg.h`、`src/u_ffmpeg.c`，以及三线程流水线的接入
> 配套操作文档：[RTMP 预览操作](RTMP预览操作.md)

---

## 一、目标与边界

板端 MPP 编码器输出 H.264 裸流（Annex-B）。目标是把每一帧封装成 FLV，通过 RTMP 推到服务器。

本文只讨论**板端推流实现**：

- H.264 Annex-B / AVCC / avcC 的背景；
- `u_ffmpeg` 模块每个函数怎么设计、为什么这样设计；
- 与采集/编码/消费三线程、环形缓冲区的衔接；
- 断线重连、时间戳、关键帧、内存生命周期；
- 构建与验证。

WSL 里怎么起 MediaMTX、Windows 怎么做 1935 转发、PyQt 怎么预览，见 [RTMP 预览操作](RTMP预览操作.md)。

---

## 二、背景知识

### 2.1 H.264 的 NAL、SPS、PPS、IDR

H.264 码流由一个个 **NAL unit** 组成。每个 NAL 的第一个字节是 NAL header：

```text
+---------------+
|0| nal_ref_idc | nal_unit_type |
+---------------+
  1 bit   2 bits       5 bits
```

常见 `nal_unit_type`：

| type | 名称 | 说明 |
|---|---|---|
| 1 | non-IDR slice | 普通 P/B 帧切片 |
| 5 | IDR slice | 关键帧，随机访问点 |
| 6 | SEI | 补充增强信息，可带编码器信息 |
| 7 | SPS | Sequence Parameter Set，分辨率、profile、level 等 |
| 8 | PPS | Picture Parameter Set，熵编码、QP 等 |
| 9 | AUD | Access Unit Delimiter，访问单元分隔 |

解码器必须先拿到 SPS/PPS，才能解后面的 slice；IDR 则保证从此处开始可以独立解码。

MPP 的行为：

- 编码器初始化后，`MPP_ENC_GET_HDR_SYNC` 可以拿到 SPS/PPS 头；
- 编码输出的每个 packet 一般是一个 NAL（带起始码）；
- GOP 配置为 `fps * 2`，1080p30 下约 2 秒一个 IDR；
- 当前编码码率：CBR 8 Mbps（1080p30）；
- 默认不开 B 帧，所以 PTS 和 DTS 可以认为相等。

### 2.2 Annex-B 与 AVCC

**Annex-B** 用起始码分隔 NAL：

```text
00 00 01 67 ...   SPS
00 00 01 68 ...   PPS
00 00 01 65 ...   IDR
00 00 01 41 ...   P
```

起始码可能是 3 字节 `00 00 01`，也可能是 4 字节 `00 00 00 01`。裸 `.h264` 文件、MPEG-TS 常见这种格式。

**AVCC** 不用起始码，而是在每个 NAL 前放固定长度的长度字段：

```text
00 00 00 1B  67 ...   (4 字节大端长度 + SPS)
00 00 00 05  68 ...   (4 字节大端长度 + PPS)
00 00 01 2C  65 ...   (4 字节大端长度 + IDR)
```

FLV/RTMP、MP4 都要求 AVCC（长度前缀）加 `avcC` 解码配置。**MPP 输出的是 Annex-B，所以必须转换。**

转换只做两件事：

1. 找到每个 NAL 的边界；
2. 把起始码替换成 4 字节大端长度。

NAL 内部的字节（包括防竞争字节 `00 00 03`）原样保留，不需要解码 RBSP。

### 2.3 avcC（AVCDecoderConfigurationRecord）

FLV 的 AVC sequence header 里放的是 avcC，结构如下：

```text
configurationVersion            1 byte   = 1
AVCProfileIndication            1 byte   = SPS[1]
profile_compatibility           1 byte   = SPS[2]
AVCLevelIndication              1 byte   = SPS[3]
lengthSizeMinusOne              1 byte   = 0xFC | 3
numOfSequenceParameterSets      1 byte   = 0xE0 | 1
sequenceParameterSetLength      2 bytes  大端
sequenceParameterSetNALUnit     N bytes
numOfPictureParameterSets       1 byte   = 1
pictureParameterSetLength       2 bytes  大端
pictureParameterSetNALUnit      N bytes
```

本工程总长度是 `11 + len(SPS) + len(PPS)`。

为什么要在 `u_ffmpeg_init` 里生成 avcC：

- FLV muxer 在 `avformat_write_header` 时就需要 `codecpar->extradata`；
- MPP 在编码器初始化后已经缓存了 SPS/PPS（`enc->header`），不需要等第一帧；
- 提前生成可以在 `main` 里尽早发现 SPS/PPS 缺失、URL 过长等本地错误。

### 2.4 FLV 与 RTMP

RTMP 是传输协议，FLV 是容器格式。推流链路里：

```text
H.264 NAL
  -> AVCC
  -> FLV video tag
  -> RTMP chunk/message
  -> TCP
```

FLV video tag 的结构：

```text
byte 0:  FrameType(4bit) | CodecID(4bit)
         1 = key frame, 2 = inter frame
         7 = AVC
byte 1:  AVCPacketType
         0 = AVC sequence header (avcC)
         1 = AVC NALU (AVCC)
         2 = AVC end of sequence
byte 2-4: CompositionTime，24 位有符号
byte 5+: avcC 或 AVCC 数据
```

实际项目里不需要手写 FLV/RTMP：

- `avformat_alloc_output_context2(..., "flv", url)` 选择 FLV muxer；
- `avio_open2` 建立 RTMP 连接；
- `avformat_write_header` 写 FLV header、onMetaData；AVC sequence header 由 muxer 在首个视频包前写出（依赖 `codecpar->extradata`）；
- `av_interleaved_write_frame` 把 AVPacket 写成 FLV tag 并通过 RTMP 发出。

FFmpeg 负责 RTMP 握手、chunk 分片、消息封装、TCP 发送，业务层只负责把 H.264 变成正确的 AVPacket。

### 2.5 时间戳：PTS / DTS / time_base

- **PTS**：显示时间；
- **DTS**：解码时间；
- 没有 B 帧时 PTS = DTS；
- FLV 的时间单位是毫秒，stream time_base 通常是 `1/1000`。

V4L2 的 `buf.timestamp` 是**系统启动以来的绝对时间**，可能已经是几十万秒。如果直接写进 FLV，播放器会认为流从很久以前开始。因此：

1. 记录第一帧的 `start_pts_us`；
2. 后续 `pts_ms = (pts_us - start_pts_us) / 1000`；
3. 用 `av_rescale_q` 从 `1/1000` 换算到 `st->time_base`（muxer 可能调整 time_base，不能写死）；
4. 用 `last_dts_ms` 保证 dts 单调，不倒退。

为什么不用“帧序号 / fps”算时间：

- 采集有抖动，PTS 更接近真实节奏；
- V4L2 时间戳一路透传到 `packet_record_t`，不额外引入时钟；
- 代价是时间戳依赖采集端，异常时间戳需要 clamp。

### 2.6 关键帧与 GOP

FLV 的 video tag 第一位要标记 key frame。播放器/服务器通常要求：

- 新连接从关键帧开始，否则花屏/黑屏，直到下一个 IDR；
- 预览端断开重连后也一样。

本工程：

- `u_ffmpeg_mux` 扫描 NAL，发现 type 5 就置 `AV_PKT_FLAG_KEY`；
- 每次重新连接后 `wait_key=1`，丢掉非 IDR 的包，直到遇到 IDR；
- MPP GOP 约 2 秒，所以重连最多等 2 秒左右。

### 2.7 内存与生命周期

MPP 的 packet 数据在 `mpp_packet_deinit` 后失效。当前流水线的处理：

```text
MPP packet
  -> encode 线程 pipeline_write_packet 拷贝进 packet_rb
  -> mpp_packet_deinit
  -> consume 线程读到自己的 buf
  -> u_ffmpeg_mux 再拷贝进 AVPacket
  -> av_interleaved_write_frame
  -> av_packet_unref
```

`av_interleaved_write_frame` 会接管 AVPacket 的引用；发送后仍然调用 `av_packet_unref`，释放本地引用，避免泄漏。

`u_ffmpeg_mux` 复用 `ff->pkt`，`av_new_packet` 内部会先释放上一包的数据，避免每帧重新分配 AVPacket 结构本身。

---

## 三、模块结构

### 3.1 文件与依赖

| 文件 | 作用 |
|---|---|
| `inc/u_ffmpeg.h` | 对外类型和接口，只前置声明 libav 结构体 |
| `src/u_ffmpeg.c` | NAL 扫描、avcC、FLV/RTMP、连接管理 |
| `inc/pipeline.h` | `pipeline_t` 内嵌 `u_ffmpeg_t ff` |
| `src/main.c` | 初始化、`-o/-r/-or` 参数解析、统计初始化与收尾 |
| `src/thread_consume.c` | 消费线程调用 push/mux |

`u_ffmpeg.h` 不包含 `<libavformat/avformat.h>`，只写：

```c
struct AVFormatContext;
struct AVStream;
struct AVPacket;
```

这样 `pipeline.h`、`main.c`、`thread_consume.c` 不需要直接依赖 FFmpeg 头文件，编译耦合更小。

### 3.2 `u_ffmpeg_t` 字段

| 字段 | 作用 |
|---|---|
| `fmt` | `AVFormatContext`，FLV/RTMP 输出上下文 |
| `st` | `AVStream`，H.264 视频流 |
| `pkt` | 复用的 `AVPacket` |
| `url` | RTMP 地址 |
| `width/height/fps` | 编码参数，建流和 metadata 用 |
| `configured` | 本地初始化完成，允许推流 |
| `net_inited` | `avformat_network_init` 是否已调用 |
| `connected` | 已 `avio_open` + `avformat_write_header` |
| `wait_key` | 重连后等下一个 IDR |
| `last_conn_ms` | 上次连接失败的时间，用于 2 秒退避 |
| `extradata` | avcC 拷贝，重连时复用 |
| `extradata_len` | avcC 长度 |
| `start_pts_us` | 首帧 PTS，用于归零 |
| `last_dts_ms` | 上次发送的 dts，保证单调 |
| `frames` | 成功发送帧数 |
| `dropped` | 丢帧数（未连接、等 IDR、无 VCL、写失败） |

### 3.3 接口与返回值

```c
int  u_ffmpeg_init(u_ffmpeg_t *ff, const char *url,
                   int width, int height, int fps,
                   const void *sps_pps, size_t sps_pps_len);
int  u_ffmpeg_mux(u_ffmpeg_t *ff, const void *data, size_t len,
                  uint64_t pts_us);
int  u_ffmpeg_push(u_ffmpeg_t *ff);
void u_ffmpeg_deinit(u_ffmpeg_t *ff);
```

返回约定：

| 函数 | 返回 | 含义 |
|---|---|---|
| `u_ffmpeg_init` | 0 | 本地初始化成功（不代表网络已连上） |
| `u_ffmpeg_init` | -1 | 参数/URL/SPS-PPS/内存错误 |
| `u_ffmpeg_push` | 1 | 已连接，可以发送 |
| `u_ffmpeg_push` | 0 | 未连接/退避中，本轮丢帧 |
| `u_ffmpeg_mux` | 1 | 已发送 |
| `u_ffmpeg_mux` | 0 | 跳过/丢帧（未连接、等 IDR、无 VCL、写失败） |
| `u_ffmpeg_mux` | -1 | 封装失败（长度/内存异常） |

核心取舍：

- **init 只做本地准备，不建网络连接**；
- **push 只做连接管理**；
- **mux 同时完成封装和发送**（`av_interleaved_write_frame` 本身同时做 mux + write）。

这是按“三个函数”的要求做的职责切分，也避免了把 `AVPacket` 暴露到业务层再单独调用一次发送。

---

## 四、函数设计

### 4.1 `u_ffmpeg_init`

#### 做什么

```c
memset(ff, 0, sizeof(*ff));
ff->start_pts_us = -1;
ff->last_dts_ms = -1;

校验 url / sps_pps / width / height
拷贝 url
avformat_network_init()
build_avcc(sps_pps, sps_pps_len, &ff->extradata, &ff->extradata_len)
ff->pkt = av_packet_alloc()
ff->width / height / fps / configured = 1
```

#### 为什么把 SPS/PPS 作为入参

- MPP 在 `mpp_encoder_init` 时已经通过 `MPP_ENC_GET_HDR_SYNC` 拿到 SPS/PPS；
- FLV 的 `avformat_write_header` 需要 avcC extradata，必须在第一包之前准备好；
- 如果等到第一帧再解析 SPS/PPS，init 和“第一次发送”会耦合在一起，状态更复杂；
- 把 SPS/PPS 作为入参，`main` 可以在启动阶段就发现“编码器没给头”这种错误。

#### 为什么 init 不连接 RTMP

- 程序可能比服务器先启动，服务器没起来不应该阻止采集；
- 连接失败要支持重试，而重试属于 `push` 的职责；
- `main` 里 init 只做本地内存/格式准备，启动快，失败边界清晰；
- 第一帧到达时由 `push` 建连，连接失败只丢帧，不影响采集和本地文件。

#### 失败清理

- 如果 `build_avcc` 或 `av_packet_alloc` 失败，释放 extradata、pkt，并调用 `avformat_network_deinit`（如果已 init）；
- `configured` 保持 0，`main` 看到 -1 后直接清理退出；
- 不会留下半初始化的 `AVFormatContext`，因为 init 阶段根本不创建网络上下文。

### 4.2 `next_nal` 与 `build_avcc`

#### `next_nal` 的算法

```text
从 pos 开始找 00 00 01
  -> payload = 起始码之后
  -> 从 payload 继续找下一个 00 00 01
  -> 下一个起始码的位置就是当前 NAL 的结束
  -> 如果结束前一个字节是 0，说明下一个是 4 字节起始码，去掉这个 0
  -> 返回 NAL 指针和长度
```

边界处理：

- 支持 3 字节和 4 字节起始码；
- 空 NAL 跳过，继续找下一个；
- 找不到下一个起始码时，当前 NAL 一直到 buffer 末尾；
- 4 字节起始码多出来的那个 `00` 不算 NAL 数据；
- 防竞争字节 `00 00 03` 原样保留，不能当起始码。

为什么不用 FFmpeg 内部的 `ff_avc_parse_nal_units` / `avpriv_find_start_code`：

- 这些是内部 API，不在公开头文件里，跨版本不稳定；
- 逻辑很小，自己写 40 行左右即可；
- 这样 `u_ffmpeg.c` 只依赖公开的 libavformat/libavcodec/libavutil。

#### `build_avcc` 的步骤

1. 遍历 SPS/PPS 缓冲区，找到第一个 type 7 和第一个 type 8；
2. 检查 SPS 长度至少 4（要取 profile/compat/level）；
3. 分配 `11 + sps_len + pps_len`；
4. 按 avcC 布局写入；
5. 返回 malloc 的 buffer，由 `u_ffmpeg_deinit` free。

为什么只取第一个 SPS/PPS：

- MPP 的 header 就一组；
- 当前不支持动态分辨率/动态 SPS；
- 如果以后支持，需要在重连时重建 extradata，而不是复用。

### 4.3 `u_ffmpeg_push`

#### 状态机

```text
configured == 0
  -> return 0

connected && fmt && st
  -> return 1

距离上次连接失败 < 2000ms
  -> return 0（退避，丢帧）

ff_connect():
  avformat_alloc_output_context2("flv")
  avformat_new_stream()
  填 codecpar: H264 / 宽高 / avcC extradata
  time_base = 1/1000
  avg_frame_rate / r_frame_rate = fps
  avio_open2(rw_timeout=1s)
  avformat_write_header()
  connected = 1, wait_key = 1
  return 1

失败:
  last_conn_ms = now
  return 0
```

#### 为什么每次重连都重建 AVFormatContext

- FLV muxer 的 `write_header` 写 FLV header/metadata，并在首个视频包前写 AVC sequence header；
- 失败后复用旧 context 容易出现重复 header、旧 stream 状态、旧 IO 指针；
- 直接 `avformat_free_context`，重新 alloc 最干净；
- avcC 存在 `ff->extradata`，重建时拷贝一份即可。

#### 为什么连接超时是 1 秒

- `avio_open2` 的 `rw_timeout` 设为 1 秒；
- 消费线程在 `push` 里最多阻塞 1 秒，不会把 `packet_rb` 拖爆；
- 8 Mbps 下 8 MB 的 `packet_rb` 约 8 秒缓冲，1 秒的连接尝试可以接受；
- 代价是跨公网/高延迟网络可能需要调大。

#### 为什么重连后要等 IDR

- 新 RTMP 会话的 FLV 序列头已经写了，但第一个视频包如果是 P 帧，解码器没有参考帧，会花屏；
- 等 IDR 最多等一个 GOP（约 2 秒），换来新观众/新连接立刻能看；
- `wait_key` 在 `ff_connect` 成功时置 1，成功发出 IDR 后清 0。

#### 为什么网络错误不置 fatal

- 采集/编码链路不应该因为网络抖动整体退出；
- 程序的目标是跑到 SIGINT/SIGTERM；
- 网络错误只增加 `dropped`，连接恢复后继续推；
- 本地文件写失败仍然 fatal，因为那通常意味着磁盘/路径真的有问题。

### 4.4 `u_ffmpeg_mux`

#### 输入

- `data/len`：从 `pipeline_read_packet` 读到的 H.264 包；
- `pts_us`：采集时间戳；
- 调用方已经过滤 `PKT_FLAG_HEADER` / `PKT_FLAG_EOS`，mux 只处理视频包。

#### 两遍扫描

第一遍：

```text
total = 0
has_idr = 0
has_vcl = 0
for each NAL:
    total += 4 + nal_len
    if type == 5: has_idr = 1
    if type == 1 or 5: has_vcl = 1
```

第二遍：

```text
av_new_packet(pkt, total)
for each NAL:
    写 4 字节大端长度
    memcpy NAL
pkt->size = total
```

为什么两遍：

- `av_new_packet` 需要精确大小，避免 `av_grow_packet` 反复 realloc；
- 第一遍顺便判断 IDR 和是否存在 VCL，逻辑清晰；
- 每帧只有几 KB 到几十 KB，两遍扫描开销很小。

#### 为什么丢弃没有 VCL 的包

- 只有 SPS/PPS/SEI 的包不产生画面；
- 如果单独发，可能生成没有图像的 video tag；
- 头包已经在 consume 层跳过，这里再兜底。

#### 时间戳

```c
if (ff->start_pts_us < 0)
    ff->start_pts_us = pts_us;

pts_ms = (pts_us - start_pts_us) / 1000;
if (pts_ms < 0) pts_ms = 0;

dts_ms = pts_ms;
if (dts_ms <= ff->last_dts_ms)
    dts_ms = ff->last_dts_ms + 1;

pkt->pts = av_rescale_q(dts_ms, {1,1000}, ff->st->time_base);
pkt->dts = pkt->pts;
pkt->duration = av_rescale_q(1000 / fps, {1,1000}, ff->st->time_base);
```

- 减去首帧，避免绝对时间戳；
- `av_rescale_q` 不假设 muxer 一定用 1/1000；
- `+1` 保证 dts 严格递增，避免 FLV muxer 警告；
- 同一帧拆成多个包时，可能出现相同 PTS，这里用 +1ms 保护。

#### 发送与错误处理

```c
ret = av_interleaved_write_frame(ff->fmt, ff->pkt);
if (ret < 0) {
    记录日志
    av_packet_unref(pkt)
    ff_disconnect(ff, 0)
    last_conn_ms = now
    dropped++
    return 0
}
av_packet_unref(pkt)
wait_key = 0
last_dts_ms = dts_ms
frames++
return 1
```

为什么用 `av_interleaved_write_frame`：

- 单视频流时和 `av_write_frame` 差别不大；
- interleaved 版本统一处理多流交错、时间戳和 flush，后续加音频也方便；
- mux + write 一次完成，符合“封装函数”包含发送的设计。

### 4.5 `u_ffmpeg_deinit`

```text
ff_disconnect(ff, write_trailer=1)
  -> 如果 connected，av_write_trailer
  -> avio_closep
  -> avformat_free_context
av_packet_free
free(extradata)
打印 sent/dropped
configured = 0
if net_inited: avformat_network_deinit
```

- 在 `main` 里所有线程 join 之后调用，没有并发访问；
- 正常退出写 trailer；网络错误路径 `ff_disconnect(ff, 0)` 不写 trailer，避免在坏连接上阻塞；
- `ff_disconnect` 会重置 `fmt/st/connected`，重连时重新创建。

---

## 五、与流水线集成

### 5.1 `main`

启动顺序：

```text
打开 V4L2 -> set_fmt -> reqbuf -> mmap -> queue -> stream_on
-> 初始化 frame_rb / packet_rb
-> mpp_encoder_init（缓存 SPS/PPS）
-> u_ffmpeg_init（构建 avcC）
-> 申请 rga_buf / enc_buf
-> 创建 capture / encode / consume
```

收尾：

```text
join capture -> join encode -> join consume
-> u_ffmpeg_deinit
-> mpp_buffer_put / mpp_encoder_deinit
-> rb_deinit / stream_off / close_device
-> _exit(0)
```

`_exit(0)` 继续保留，绕过 `librockchip_mpp` 退出阶段约 14 秒的全局清理；FFmpeg 资源已经在 `_exit` 前显式释放。

### 5.2 消费线程

```text
读 packet_rb
  -> 本地文件模式：file_saver_write
  -> 推流模式：
       if HEADER/EOS: 跳过
       if u_ffmpeg_push() == 1:
            u_ffmpeg_mux()
       else:
            dropped++
```

- 推流模式 `output_path` 为空，不打开文件；
- 网络错误不 `pipeline_abort`；
- 文件写失败仍按原有逻辑 fatal。

### 5.3 采集线程

```c
while (!g_pipeline_stop && !p->fatal) { ... }
```

去掉了 `frame_num` 上限，SIGINT/SIGTERM 后：

```text
capture 停
  -> capture_done = 1
  -> encode 排空 frame_rb + 发 EOS
  -> consume 排空 packet_rb
  -> main deinit
```

### 5.4 错误策略总结

| 错误 | 处理 |
|---|---|
| URL/SPS/PPS/内存错误 | init 返回 -1，main 清理退出 |
| RTMP 连接失败 | push 返回 0，丢帧，2 秒后重试 |
| RTMP 写失败 | mux 断开连接，丢帧，下一轮重连 |
| 包格式异常/内存分配失败 | mux 返回 -1，日志，继续 |
| 本地文件写失败 | pipeline_abort，整体退出 |
| 摄像头/编码 fatal | 现有 fatal 流程，整体退出 |

---

## 六、构建与依赖

Makefile 增加：

```make
CFLAGS += -I$(SYSROOT)/usr/include
LDLIBS += -lavformat -lavcodec -lavutil
```

板端依赖：

- `libavformat.so.58`
- `libavcodec.so.58`
- `libavutil.so.56`
- `librtmp.so.1`（FFmpeg 的 RTMP 依赖之一）

编译结果：

```text
0 warning / 0 error
rkCam: ELF 64-bit LSB, ARM aarch64
NEEDED: libavformat.so.58, libavcodec.so.58, libavutil.so.56
```

---

## 七、验证

### 7.1 码流解析

用 `tmpfile/imx415.h264` 验证 NAL 扫描：

```text
总 NAL：778
SPS：1
PPS：1
SEI：26
IDR：13
P：737
```

### 7.2 FLV 封装

按相同的 avcC/AVCC 规则生成测试 FLV：

```text
ffprobe: codec_name=h264, width=1920, height=1080
ffmpeg -f null -: 解码返回 0
```

### 7.3 WSL 端到端

MediaMTX + `test_publish.sh` + PyQt 预览：

```text
预览 image=True，状态“预览中 1920x1080”
```

### 7.4 尚未验证

- 板端真实 IMX415 推流；
- 长时间运行（建议 30 分钟以上，观察内存、重连、丢帧）；
- 弱网/公网 RTMP；
- 服务端需要鉴权/TLS 的场景。

---

## 八、设计取舍

### 8.1 为什么不用 FFmpeg 的 bitstream filter

`h264_mp4toannexb` 是 AVCC → Annex-B，方向相反；FFmpeg 4.4 没有公开的 Annex-B → AVCC 通用 bsf。手写转换只涉及起始码和长度前缀，依赖更少。

### 8.2 为什么不用 librtmp 直接写

- 板端 FFmpeg 已带 FLV muxer 和 RTMP protocol；
- libavformat 负责 FLV metadata、chunk、时间戳；
- 不引入 librtmp 的 API 细节，代码更短；
- 后续加 MP4/文件输出也容易复用。

### 8.3 为什么不用 MP4 代替 FLV

RTMP 只承载 FLV。MP4 适合落盘/点播，不适合直接 RTMP 直播。

### 8.4 为什么不把 SPS/PPS 塞进每个包

- FLV sequence header 已经带 avcC；
- 重复 SPS/PPS 增加码率，部分播放器还会重复初始化；
- 当前 MPP 的第一帧 IDR 可能自带 SPS/PPS，代码不会主动剥离，但也不会依赖它。

### 8.5 为什么不做 B 帧

- MPP 当前配置不开 B 帧；
- 没有 B 帧时 PTS=DTS，时间戳逻辑简单；
- 如果以后开 B 帧，需要重新设计 DTS 和重排，不能用当前的 `dts=pts`。

### 8.6 为什么 init 不连接、push 才连接

- 启动顺序解耦；
- 服务器可以先不起；
- 网络失败可重试；
- init 在 main 里快速返回，失败边界清楚。

### 8.7 为什么网络错误不 fatal

推流是“尽力而为”的旁路，采集/编码是主线。网络断了应该继续采集，恢复后继续推，而不是让整个相机程序退出。

---

## 九、限制与后续

当前限制：

- 只推视频，没有音频；
- 只支持 H.264，不支持 H.265；
- 不支持动态分辨率/SPS 变化；
- 没有 RTMP 鉴权/TLS；
- 单路流；
- 断线重连后时间戳继续增长，不重置为 0；
- 连接超时 1 秒，跨公网可能需要调大；
- 推流模式默认不落盘；`-or` 可同时落盘，落盘失败会自动降级为只推流。

后续可做：

1. 加音频（AAC）并处理音视频交错；
2. 支持 H.265（FLV 的 enhanced RTMP）；
3. 动态分辨率时重建 avcC；
4. 加鉴权、TLS、重连退避指数；
5. 把 `[stats]` 统计接入外部监控/日志系统（本地打印已就绪）；
6. 把 `u_ffmpeg` 抽成 sink 接口，同时支持文件/RTMP/SRT。

---

## 十、相关文档

- [RTMP 预览操作](RTMP预览操作.md) — WSL MediaMTX、Windows 1935 转发、PyQt 预览
- [scripts/rtmp_server/README.md](../scripts/rtmp_server/README.md)
- [scripts/rtmp_preview/README.md](../scripts/rtmp_preview/README.md)
- [三线程采集/编码/消费流水线改造](三线程采集编码消费流水线改造.md)