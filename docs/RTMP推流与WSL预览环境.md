# RTMP 推流与 WSL 预览环境记录

> 工程：`mediaCam`（RK3568 + IMX415 + RGA + MPP）
> 目标：板端 `rkCam` 推 RTMP，WSL 里跑 MediaMTX 服务并用 PyQt 预览
> 记录时间：2026-11-01

---

## 一、最终链路

```text
板端 rkCam（局域网 IP，例如 192.168.10.50）
  │
  │  RTMP push: rtmp://192.168.10.31:1935/live/imx415
  ▼
Windows 局域网 IP（例如 192.168.10.31）:1935
  │
  │  netsh interface portproxy
  ▼
WSL2（例如 172.26.177.124）:1935
  │
  │  MediaMTX
  ├── WSL 预览：rtmp://127.0.0.1:1935/live/imx415
  └── 可选：Windows 预览：rtmp://172.26.177.124:1935/live/imx415
```

关键点：

1. **板端只是推流客户端**，`-o` 后面填的是 RTMP 服务器地址，不是板子 IP。
2. **WSL2 默认是 NAT 网络**，板子不能直接访问 WSL 的 `172.x` 地址，所以必须在 Windows 上把 1935 转发到 WSL。
3. **预览脚本也是客户端**，必须连到同一个 RTMP 服务；服务在 WSL 时，WSL 预览用 `127.0.0.1`。
4. Windows 的局域网 IP、WSL 的 IP 都可能变化，以 `setup_windows_portproxy.ps1` 打印的结果为准。

---

## 二、目录与文件

### 2.1 板端源码

| 文件 | 说明 |
|---|---|
| [inc/u_ffmpeg.h](../inc/u_ffmpeg.h) | `u_ffmpeg_t` 和 init/mux/push/deinit 接口 |
| [src/u_ffmpeg.c](../src/u_ffmpeg.c) | avcC 构造、Annex-B→AVCC、FLV/RTMP、断线重连 |
| [inc/pipeline.h](../inc/pipeline.h) | `pipeline_t` 增加 `u_ffmpeg_t ff`，删除 `frame_num` |
| [src/main.c](../src/main.c) | `-o/-r` 参数、FFmpeg 初始化/收尾、常驻运行 |
| [src/thread_capture.c](../src/thread_capture.c) | 采集循环改为只受信号/fatal 控制 |
| [src/thread_consume.c](../src/thread_consume.c) | 消费线程接入 `u_ffmpeg_push` / `u_ffmpeg_mux` |
| [Makefile](../Makefile) | 增加 FFmpeg 头文件路径和 `-lavformat -lavcodec -lavutil` |

### 2.2 scripts 目录

```text
scripts/
├── README.md
├── rtmp_preview/
│   ├── preview_rtmp.py       # PyQt + OpenCV 预览
│   ├── run_preview.sh        # 只在本进程激活 conda，不写 ~/.bashrc
│   ├── environment.yml       # conda 环境描述
│   ├── requirements.txt      # pip 方式（Windows）
│   └── README.md
└── rtmp_server/
    ├── bin/mediamtx          # MediaMTX 二进制，已 gitignore
    ├── mediamtx.yml          # 只开 RTMP 1935
    ├── run_server.sh         # 前台启动服务
    ├── test_publish.sh       # 无板子时用本地视频模拟推流
    ├── download_mediamtx.sh  # 下载/更新二进制
    ├── setup_windows_portproxy.ps1
    ├── remove_windows_portproxy.ps1
    └── README.md
```

说明：

- `bin/mediamtx` 约 56 MB，不提交到 git，`.gitignore` 已忽略；
- 需要重新下载时运行 `./scripts/rtmp_server/download_mediamtx.sh`；
- 当前使用 MediaMTX `v1.21.3`，下载的 tar.gz SHA256 与 GitHub Release 的 digest 一致。

---

## 三、板端 `rkCam` 改造

### 3.1 新增 `u_ffmpeg` 模块

接口：

```c
int  u_ffmpeg_init(u_ffmpeg_t *ff, const char *url,
                   int width, int height, int fps,
                   const void *sps_pps, size_t sps_pps_len);
int  u_ffmpeg_mux(u_ffmpeg_t *ff, const void *data, size_t len,
                  uint64_t pts_us);
int  u_ffmpeg_push(u_ffmpeg_t *ff);
void u_ffmpeg_deinit(u_ffmpeg_t *ff);
```

职责划分：

| 函数 | 调用位置 | 职责 |
|---|---|---|
| `u_ffmpeg_init` | `main`，`mpp_encoder_init` 之后 | 从 SPS/PPS 生成 avcC extradata，创建 AVFormatContext/AVStream/AVPacket；不做网络连接 |
| `u_ffmpeg_mux` | 消费线程 | Annex-B → AVCC、PTS/DTS、关键帧标志，内部调用 `av_interleaved_write_frame` 完成封装+发送 |
| `u_ffmpeg_push` | 消费线程 | 连接管理：首次 `avio_open` + `avformat_write_header`，断线后每 2 秒重连 |
| `u_ffmpeg_deinit` | `main` 收尾 | 写 trailer、关闭 RTMP、释放资源 |

### 3.2 封装细节

MPP 输出的是 Annex-B H.264，FLV/RTMP 需要 AVCC：

- 扫描 3/4 字节起始码 `00 00 01` / `00 00 00 01`；
- 每个 NAL 写成 `4 字节大端长度 + NAL 数据`；
- 扫到 NAL type 5（IDR）时置 `AV_PKT_FLAG_KEY`；
- SPS/PPS 来自 `enc->header`，由 `u_ffmpeg_init` 生成 avcC；
- PTS 从首帧归零，FLV 时间基 1/1000，dts 保证单调；
- 消费线程会跳过 `PKT_FLAG_HEADER`（SPS/PPS 包）和 `PKT_FLAG_EOS`，mux 只处理视频包。

### 3.3 消费线程

[src/thread_consume.c](../src/thread_consume.c) 中：

```c
if (p->ff.configured && !(flags & (PKT_FLAG_HEADER | PKT_FLAG_EOS))) {
    if (u_ffmpeg_push(&p->ff) == 1) {
        int m = u_ffmpeg_mux(&p->ff, buf, len, pts_us);
        if (m < 0) {
            fprintf(stderr, "[consume] rtmp mux failed at seq=%u\n", seq);
        }
    } else {
        p->ff.dropped++;
    }
}
```

- 网络错误只丢帧，不调用 `pipeline_abort()`；
- 本地文件写失败仍然保持 fatal 语义；
- 推流模式不落盘，避免长时间运行写满磁盘。

### 3.4 断线重连

- `u_ffmpeg_push` 连接失败后 2 秒退避重试；
- `u_ffmpeg_mux` 写失败时关闭当前会话，下一轮由 push 重建；
- 重连后 `wait_key=1`，等下一个 IDR 再发，避免新连接从 P 帧开始；
- 预览端也每 2 秒自动重连。

### 3.5 常驻运行与 `-o/-r`

去掉了原来的 `<seconds>` 参数和 `frame_num`，采集线程只由 `g_pipeline_stop` / `fatal` 控制：

```bash
# 推流模式
./rkCam -o rtmp://192.168.10.31:1935/live/imx415

# 落盘模式
./rkCam -r /root/imx415.h264
```

`-o` 和 `-r` 必须且只能给一个；程序运行到 `SIGINT`（Ctrl-C）或 `SIGTERM`，然后：

```text
capture 停
  -> encode 排空 frame_rb + 发 EOS
  -> consume 排空 packet_rb
  -> main 调 u_ffmpeg_deinit 写 trailer
  -> 释放 MPP / ring / V4L2
```

编码码率已改为 8 Mbps（`rc:bps_target`），没有改其他编码参数。

### 3.6 构建

```bash
make clean all
```

结果：

- 0 warning / 0 error；
- 生成 `rkCam`（aarch64 ELF）；
- 动态依赖包含 `libavformat.so.58`、`libavcodec.so.58`、`libavutil.so.56`；
- 板端 target 已有这些库。

---

## 四、WSL 端 MediaMTX 服务

### 4.1 为什么需要 Windows 端口转发

查看 WSL 网络模式：

```bash
wslinfo --networking-mode
# 输出 nat
```

NAT 模式下：

- WSL 有独立的内网 IP，例如 `172.26.177.124`；
- Windows 能访问 WSL；
- **板子不能直接访问这个 172.x 地址**；
- 所以需要在 Windows 上监听 1935，再转发到 WSL 的 1935。

### 4.2 MediaMTX 配置

[scripts/rtmp_server/mediamtx.yml](../scripts/rtmp_server/mediamtx.yml) 只启用 RTMP：

```yaml
logLevel: info
logDestinations: [stdout]

api: false
metrics: false
pprof: false
playback: false

rtsp: false
rtmp: true
rtmpAddress: :1935
hls: false
webrtc: false
srt: false
moq: false

paths:
  all_others:
```

这样只监听 `:1935`，不会额外开 RTSP/HLS/WebRTC/MoQ 端口。

### 4.3 启动与模拟推流

```bash
cd /root/rk3568/rk_project/mediaCam

# 终端 1：服务器
./scripts/rtmp_server/run_server.sh

# 终端 2：没有板子时，用本地文件模拟推流
./scripts/rtmp_server/test_publish.sh
```

`test_publish.sh` 会：

1. 把裸 `.h264` 封装成临时 MP4 补时间戳；
2. 用 `ffmpeg -re -stream_loop -1` 循环推到 `rtmp://127.0.0.1:1935/live/imx415`；
3. 退出时自动删除临时文件。

也可以指定文件和地址：

```bash
./scripts/rtmp_server/test_publish.sh /path/to/video.mp4 rtmp://127.0.0.1:1935/live/imx415
```

### 4.4 Windows 端口转发

**第一次使用、以及每次 `wsl --shutdown` / 重启之后**，以管理员身份运行：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\setup_windows_portproxy.ps1"
```

脚本会：

1. 读取当前 WSL IP；
2. 读取 Windows 默认路由所在网卡的局域网 IP；
3. 建立 `Windows 0.0.0.0:1935 -> WSL_IP:1935`；
4. 添加只允许局域网来源的 1935/TCP 防火墙规则；
5. 打印板端推流地址和 WSL 预览地址。

检查：

```powershell
netsh interface portproxy show all
netstat -ano | findstr :1935
```

删除：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\remove_windows_portproxy.ps1"
```

### 4.5 踩坑：WSL localhost 代理警告导致脚本中断

第一次运行 setup 脚本时，出现类似：

```text
wsl.exe : wsl: 检测到 localhost 代理配置，但未镜像到 WSL。
NAT 模式下的 WSL 不支持 localhost 代理。
+ CategoryInfo : NotSpecified: (...) [NativeCommandError]
```

原因：

- 这段文字是 `wsl.exe` 写到 **stderr** 的警告，不是真正的错误；
- 脚本当时用了 `$ErrorActionPreference = "Stop"`；
- Windows PowerShell 5.1 把 native command 的 stderr 当成了终止错误；
- 脚本在第 26 行 `$wslOut = & wsl.exe ... 2>$null` 就退出了，**还没执行任何 `netsh` 修改**。

修复：

```powershell
# 不直接用 PowerShell 调 wsl，改用 cmd 包一层，只取 stdout
$wslOut = cmd.exe /c "wsl.exe -d $Distro -- hostname -I 2>nul"

# netsh 也走 cmd，避免 stderr 再触发 Stop
cmd.exe /c "netsh interface portproxy delete ... 2>nul" | Out-Null
$addOut = cmd.exe /c "netsh interface portproxy add ... 2>&1"
if ($LASTEXITCODE -ne 0) { throw "netsh 端口转发添加失败：$addOut" }
```

修复后脚本重新执行即可；失败的那次没有留下 portproxy 或防火墙规则。

### 4.6 可选：WSL 镜像网络

如果 Windows 11 支持 WSL 镜像网络，可以在 `%UserProfile%\.wslconfig` 中加：

```ini
[wsl2]
networkingMode=mirrored
```

然后 `wsl --shutdown` 重启。这样 WSL 直接使用 Windows 的网络身份，板端可以直接推 `rtmp://<Windows局域网IP>:1935/...`，不再需要 `netsh` 转发。镜像网络可能影响 Docker/VPN，按需使用。

---

## 五、PyQt 预览脚本

### 5.1 conda 环境

- conda：Miniforge3，安装在 `/root/miniforge3`；
- 环境名：`mediaCam-preview`；
- Python 3.11.18；
- PyQt5 5.15.11 / Qt 5.15.15；
- OpenCV 5.0.0；
- NumPy 2.4.6；
- Noto CJK 字体（解决 WSL 下中文显示方块）。

隔离说明：

- 没有执行 `conda init`，`.bashrc` 未修改；
- 系统 Python 仍是 `/usr/bin/python3` 3.8.10，没有系统级 PyQt5/OpenCV；
- `conda` 不在系统 `PATH` 里，按需 source。

重建环境：

```bash
"$HOME/miniforge3/bin/conda" env create -f scripts/rtmp_preview/environment.yml
```

### 5.2 启动

```bash
cd /root/rk3568/rk_project/mediaCam

# 默认地址 rtmp://127.0.0.1:1935/live/imx415
./scripts/rtmp_preview/run_preview.sh

# 指定地址
./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415

# 本地文件
./scripts/rtmp_preview/run_preview.sh tmpfile/imx415.h264
```

`run_preview.sh` 只在当前进程里：

```bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HOME/miniforge3/etc/profile.d/conda.sh"
conda activate mediaCam-preview
exec python "$SCRIPT_DIR/preview_rtmp.py" "$@"
```

不会修改 `~/.bashrc`。

### 5.3 功能

- PyQt6/PyQt5 自动检测，优先 PyQt6；
- RTMP / RTSP / HTTP 流，或本地 `.h264` / `.mp4`；
- 视频按比例居中绘制，默认压到 960 宽；
- 自动重连（2 秒）；
- 状态栏显示分辨率、连接、错误信息；
- WSLg 下可直接弹窗；如果窗口不出现，检查 `echo $DISPLAY` 和 `/mnt/wslg`。

### 5.4 RTMP 低延迟打开

OpenCV 默认打开 RTMP 时会等较多探测数据，可能 5~7 秒才出画面，甚至被 5 秒打开超时打断。

预览脚本对直播流设置：

```text
OPENCV_FFMPEG_CAPTURE_OPTIONS=rtmp_live;live|analyzeduration;0|probesize;32
```

并把：

- `OPEN_TIMEOUT_MS` 调到 10000；
- `READ_TIMEOUT_MS` 调到 5000。

实测 MediaMTX 下约 1~2 秒出画面。若探测参数太激进导致打不开，会自动回退到默认探测。

---

## 六、完整运行步骤

### 1. WSL 启动服务器

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh
```

看到：

```text
[RTMP] started with listener on :1935 (TCP/RTMP)
```

### 2. Windows 管理员建立端口转发

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\setup_windows_portproxy.ps1"
```

记下它打印的 **Windows 局域网 IP**，例如 `192.168.10.31`。

### 3. WSL 启动预览

```bash
./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415
```

### 4. 板端推流

```bash
./rkCam -o rtmp://192.168.10.31:1935/live/imx415
```

---

## 七、无板子本地联调

```bash
# 终端 1：服务器
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh

# 终端 2：模拟板端推流
./scripts/rtmp_server/test_publish.sh

# 终端 3：预览
./scripts/rtmp_preview/run_preview.sh
```

这条链路已实际验证：预览显示 `1920x1080`，MediaMTX 日志出现：

```text
[path live/imx415] stream is available and online, 1 track (H264)
[RTMP] ... is publishing to path 'live/imx415'
```

---

## 八、验证记录

### 8.1 编译

```text
make clean all
0 warning / 0 error
rkCam: ELF 64-bit LSB, ARM aarch64
NEEDED: libavformat.so.58, libavcodec.so.58, libavutil.so.56
```

### 8.2 NAL / AVCC 解析

用 `tmpfile/imx415.h264` 验证解析：

```text
总 NAL：778
SPS：1
PPS：1
SEI：26
IDR：13
P：737
```

### 8.3 FLV 封装

按模块相同的 avcC/AVCC 规则生成测试 FLV：

```text
ffprobe: codec_name=h264, width=1920, height=1080
ffmpeg -f null -: 解码返回 0
```

### 8.4 WSL 端到端

```text
MediaMTX v1.21.3
  + ffmpeg 模拟推流
  + PyQt5 预览
  -> 预览 image=True，状态“预览中 1920x1080”
```

### 8.5 尚未实测

- 板端真实摄像头推流（需要板子和实际网络）；
- Windows 端口转发的真实生效（需要管理员执行 setup 脚本后验证）；
- 长时间运行（建议跑 30 分钟以上，观察内存、重连、丢帧计数）。

---

## 九、常见问题

### 9.1 `wsl.exe : wsl: 检测到 localhost 代理配置...`

见 4.5 节。这是 WSL 的提示，不影响 RTMP；setup 脚本已经屏蔽了 `wsl.exe` 的 stderr。

### 9.2 服务器报 `bind: address already in use`

旧 MediaMTX 还在跑：

```bash
ss -ltn | grep 1935
pkill -9 -f 'scripts/rtmp_server/bin/mediamtx'
```

然后重新 `run_server.sh`。

### 9.3 板端连不上

按顺序检查：

1. 板端和 Windows 在同一局域网，能 `ping <Windows局域网IP>`；
2. Windows 上 `netsh interface portproxy show all` 有 1935 规则；
3. Windows 防火墙允许 1935/TCP（setup 脚本会加规则）；
4. WSL 重启后 IP 变了，重新运行 setup；
5. 板端 `-o` 填的是 Windows 局域网 IP，不是板子 IP，也不是 WSL 的 172.x IP。

### 9.4 预览一直“正在连接”

1. 确认服务器在跑：`ss -ltn | grep 1935`；
2. 确认有推流源：`test_publish.sh` 或板端；
3. 看 MediaMTX 日志有没有 `is publishing to path 'live/imx415'`；
4. 确认预览地址路径一致：`live/imx415`。

### 9.5 中文显示方块

WSL 系统没有中文字体。已经在 conda 环境里安装 `font-ttf-noto-cjk`；如果重建环境，确保 `environment.yml` 里有这一项。

### 9.6 预览窗口不出现

WSLg 检查：

```bash
echo $DISPLAY
ls -d /mnt/wslg
```

PyQt 的 xcb 平台应能创建窗口。如果仍不行，可以在 Windows 上装 PyQt6/OpenCV 运行预览，或在 WSL 配置 X server。

### 9.7 `bin/mediamtx` 不存在

```bash
./scripts/rtmp_server/download_mediamtx.sh
```

默认从 GitHub Release 下载；网络慢时可覆盖：

```bash
MEDIAMTX_URL="<镜像地址>" ./scripts/rtmp_server/download_mediamtx.sh --force
```

---

## 十、安全与后续

### 10.1 安全

- MediaMTX 当前**没有鉴权**，只适合局域网开发调试；
- 不要直接把 1935 暴露到公网；
- 防火墙规则只放行 `LocalSubnet`；
- `bin/mediamtx` 来自 GitHub Release，下载的 tar.gz SHA256 已与 release digest 核对。

### 10.2 后续可做

1. 把 MediaMTX 和 setup 脚本做成 Windows 计划任务/服务，开机自动启动；
2. 如果 Windows 11 支持，改用 WSL 镜像网络，去掉 portproxy；
3. 需要浏览器播放时，打开 MediaMTX 的 HLS/WebRTC；
4. 需要公网推流时，加 RTMP 鉴权、TLS 和防火墙白名单；
5. 预览端增加录像、截图、码率/延迟统计。

---

## 十一、相关文档

- [scripts/README.md](../scripts/README.md)
- [scripts/rtmp_server/README.md](../scripts/rtmp_server/README.md)
- [scripts/rtmp_preview/README.md](../scripts/rtmp_preview/README.md)
- [三线程采集/编码/消费流水线改造](三线程采集编码消费流水线改造.md)
