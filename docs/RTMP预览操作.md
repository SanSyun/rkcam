# RTMP 预览操作

> 目标：板端 `rkCam` 推 RTMP，WSL 里跑 MediaMTX，WSL 里用 PyQt 预览
> 配套实现文档：[RTMP 推流实现](RTMP推流实现.md)

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
WSL2（例如 172.26.177.124）:1935 MediaMTX
  │
  ├── WSL 预览：rtmp://127.0.0.1:1935/live/imx415
  └── 可选：Windows 预览：rtmp://172.26.177.124:1935/live/imx415
```

关键点：

1. `rkCam -o` 填的是 **RTMP 服务器地址**，不是板子 IP；
2. WSL2 默认 NAT，板子不能直接访问 WSL 的 `172.x`，需要 Windows 做 1935 转发；
3. 预览脚本也是客户端，必须连同一个服务器；服务在 WSL 时用 `127.0.0.1`；
4. Windows 局域网 IP、WSL IP 都可能变，以 `setup_windows_portproxy.ps1` 打印结果为准。

---

## 二、快速开始

### 1. WSL 启动服务器

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh
```

看到这行即成功：

```text
[RTMP] started with listener on :1935 (TCP/RTMP)
```

### 2. Windows 管理员建立端口转发

**第一次使用、以及每次 WSL 重启/`wsl --shutdown` 之后**都要执行：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\setup_windows_portproxy.ps1"
```

记下脚本打印的 **Windows 局域网 IP**，例如 `192.168.10.31`。

### 3. WSL 启动预览

另开一个 WSL 终端：

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_preview/run_preview.sh
```

默认地址就是 `rtmp://127.0.0.1:1935/live/imx415`。

### 4. 板端推流

```bash
./rkCam -o rtmp://192.168.10.31:1935/live/imx415
```

这里的 IP 是 **Windows 的局域网 IP**，以第 2 步脚本打印的为准。

---

## 三、目录与脚本

```text
scripts/
├── README.md
├── rtmp_preview/
│   ├── preview_rtmp.py       # PyQt + OpenCV 预览
│   ├── run_preview.sh        # 进程级激活 conda，不写 ~/.bashrc
│   ├── environment.yml
│   ├── requirements.txt
│   └── README.md
└── rtmp_server/
    ├── bin/mediamtx          # 本地二进制，已 gitignore
    ├── mediamtx.yml          # 只开 RTMP 1935
    ├── run_server.sh
    ├── test_publish.sh       # 没有板子时模拟推流
    ├── download_mediamtx.sh
    ├── setup_windows_portproxy.ps1
    ├── remove_windows_portproxy.ps1
    └── README.md
```

| 脚本 | 作用 |
|---|---|
| `run_server.sh` | 前台启动 MediaMTX，Ctrl-C 停止 |
| `test_publish.sh` | 用 `tmpfile/imx415.h264` 模拟板端推流 |
| `download_mediamtx.sh` | 下载/更新 MediaMTX 二进制 |
| `setup_windows_portproxy.ps1` | Windows 管理员运行，建立 1935 转发 |
| `remove_windows_portproxy.ps1` | Windows 管理员运行，删除转发和防火墙规则 |
| `run_preview.sh` | 激活 conda 环境并启动 PyQt 预览 |

---

## 四、一次性准备

### 4.1 conda 环境

本机已经装好：

- Miniforge：`/root/miniforge3`
- 环境名：`mediaCam-preview`
- Python 3.11.18、PyQt5 5.15.11、OpenCV 5.0.0、NumPy 2.4.6、Noto CJK 字体

隔离说明：

- 没有执行 `conda init`，`.bashrc` 未修改；
- 系统 Python 仍是 `/usr/bin/python3` 3.8.10，没有系统级 PyQt5/OpenCV；
- `run_preview.sh` 只在自己进程里 source conda。

重建环境：

```bash
"$HOME/miniforge3/bin/conda" env create -f scripts/rtmp_preview/environment.yml
```

### 4.2 MediaMTX 二进制

正常情况下已经在：

```text
scripts/rtmp_server/bin/mediamtx
```

如果不存在：

```bash
./scripts/rtmp_server/download_mediamtx.sh
```

当前版本 `v1.21.3`。二进制约 56 MB，不提交到 git。

### 4.3 Windows 端口转发

需要管理员权限，见下面 5.2。脚本会读取当前 WSL IP，建立：

```text
Windows 0.0.0.0:1935 -> WSL_IP:1935
```

并添加只放行局域网来源的 1935/TCP 防火墙规则。

### 4.4 WSLg

预览是 WSL 里的 Qt 窗口，需要 WSLg：

```bash
echo $DISPLAY
ls -d /mnt/wslg
```

本机 `DISPLAY=:0`，`/mnt/wslg` 存在，PyQt 的 xcb 平台可正常创建窗口。

---

## 五、详细操作

### 5.1 WSL 启动 MediaMTX

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh
```

脚本会打印：

```text
MediaMTX RTMP 监听 :1935
WSL 预览: rtmp://127.0.0.1:1935/live/imx415
板端推流: rtmp://<Windows局域网IP>:1935/live/imx415
按 Ctrl-C 停止

2026/... INF MediaMTX v1.21.3, linux, amd64
2026/... INF [RTMP] started with listener on :1935 (TCP/RTMP)
```

检查监听：

```bash
ss -ltn | grep 1935
```

### 5.2 Windows 建立端口转发

在 **管理员 PowerShell** 中执行：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\setup_windows_portproxy.ps1"
```

脚本会：

1. 读取当前 WSL IP；
2. 读取 Windows 默认路由网卡的局域网 IP；
3. 建立 `Windows 0.0.0.0:1935 -> WSL_IP:1935`；
4. 添加防火墙规则 `RK3568 RTMP 1935`；
5. 打印板端推流地址和 WSL 预览地址。

预期输出：

```text
端口转发已建立：
  Windows 192.168.10.31:1935  ->  WSL 172.26.177.124:1935

板端推流：
  ./rkCam -o rtmp://192.168.10.31:1935/live/imx415

WSL 预览：
  ./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415
```

检查：

```powershell
netsh interface portproxy show all
netstat -ano | findstr :1935
```

删除：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\remove_windows_portproxy.ps1"
```

注意：

- WSL 重启后 IP 会变，必须重新运行 setup；
- Windows 有 VPN/虚拟网卡时，默认路由可能不是 WLAN，打印的 IP 不对就按 `ipconfig` 手工改板端命令；
- 防火墙规则只放行 `LocalSubnet`，板子要和 Windows 在同一局域网。

### 5.3 WSL 启动预览

```bash
cd /root/rk3568/rk_project/mediaCam

# 默认地址 rtmp://127.0.0.1:1935/live/imx415
./scripts/rtmp_preview/run_preview.sh

# 显式指定
./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415

# 本地文件
./scripts/rtmp_preview/run_preview.sh tmpfile/imx415.h264
```

界面支持：

- 地址栏直接编辑；
- “打开文件…”选择本地 `.h264/.mp4`；
- “开始预览”/“停止”；
- 状态栏显示连接状态、分辨率；
- 状态栏实时显示“解码 fps / 显示 fps”，终端每秒打印一行 `[preview] decode=... display=...`；
- 直播流断开后每 2 秒自动重连。

WSLg 下会直接弹窗。打开 RTMP 时脚本会设置低延迟探测参数，通常 1~2 秒出画面。

### 5.4 板端推流

```bash
./rkCam -o rtmp://192.168.10.31:1935/live/imx415
```

- `-o`：推流模式，不落盘；
- `-r`：落盘模式，例如 `./rkCam -r /root/imx415.h264`；
- `-or`：同时推流和落盘，例如
  `./rkCam -or rtmp://192.168.10.31:1935/live/imx415 /root/imx415.h264`；
- `-o` 与 `-r` 也可以同时给，等价于 `-or`；至少要给一个 sink；
- `-or` 下落盘失败（磁盘满/路径不可写）只关闭本地文件、继续推流，不影响预览；
- 程序运行到 `SIGINT`（Ctrl-C）或 `SIGTERM`，然后优雅停机。

运行中每秒打印一行统计：

```text
[stats] 12s cap=29.8 enc=29.7 sent=29.6 file=29.6 fps drop=0 rb_f=0% rb_p=9% br=5.62Mbps fw=5.62MB/s lat_mux p50=20.8ms p95=27.4ms max=44.9ms
```

退出时打印累计量和 `cap2enc` / `cap2file` / `cap2mux` 的 p50/p95/p99/max。
这些延迟是**板端采集时间戳到编码/落盘/发送完成**的时间，不含网络传输和预览端缓冲；
预览端的“解码/显示 fps”只反映播放侧是否流畅。详见 [帧率与延迟统计](帧率与延迟统计.md)。

如果板端没有 `rkCam`，先确认程序已编译并拷贝到板端。

---

## 六、没有板子时本地联调

准备三个 WSL 终端：

```bash
# 终端 1：服务器
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh

# 终端 2：模拟板端推流
./scripts/rtmp_server/test_publish.sh

# 终端 3：预览
./scripts/rtmp_preview/run_preview.sh
```

`test_publish.sh` 会：

1. 把 `tmpfile/imx415.h264` 封装成临时 MP4 补时间戳；
2. 循环推到 `rtmp://127.0.0.1:1935/live/imx415`；
3. 退出时删除临时文件。

MediaMTX 日志应出现：

```text
[path live/imx415] stream is available and online, 1 track (H264)
[RTMP] ... is publishing to path 'live/imx415'
```

预览状态栏应显示：

```text
预览中  1920x1080
```

---

## 七、地址怎么填

### 7.1 服务在 WSL（当前方案）

| 位置 | 地址 |
|---|---|
| 板端 `rkCam -o` | `rtmp://<Windows局域网IP>:1935/live/imx415` |
| WSL 预览 | `rtmp://127.0.0.1:1935/live/imx415` |
| Windows 预览（可选） | `rtmp://<WSL_IP>:1935/live/imx415` |

当前示例：

```text
Windows WLAN: 192.168.10.31
WSL:          172.26.177.124

板端：
./rkCam -o rtmp://192.168.10.31:1935/live/imx415
./rkCam -or rtmp://192.168.10.31:1935/live/imx415 /root/imx415.h264

WSL 预览：
./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415
```

### 7.2 常见填错

- 板端填板子自己的 IP：除非 SRS/MediaMTX 就在板子上，否则连不上；
- WSL 预览填板子 IP：预览要从服务器拉流，不是从板子拉；
- WSL 预览填 Windows 局域网 IP：服务在 WSL 时用 `127.0.0.1` 更直接；
- 板端填 WSL 的 `172.x`：板子不能直接访问 WSL NAT 地址；
- 路径不一致：服务器和预览都用 `live/imx415`。

### 7.3 查 IP

WSL：

```bash
hostname -I
```

Windows：

```powershell
ipconfig
```

WSL 看 Windows 主机地址：

```bash
ip route show default | awk '{print $3}'
```

注意：WSL 里的 `127.0.0.1` 指 WSL 自己；WSL 访问 Windows 上的服务要用 Windows 主机地址，不是 `127.0.0.1`。

---

## 八、停止与清理

### 8.1 停止

- 服务器：在其终端 `Ctrl-C`；
- 模拟推流：在其终端 `Ctrl-C`；
- 预览：点“停止”或关闭窗口。

### 8.2 清理端口转发

管理员 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\remove_windows_portproxy.ps1"
```

### 8.3 清理旧服务器

如果 1935 被旧进程占用：

```bash
ss -ltn | grep 1935
pkill -9 -f 'scripts/rtmp_server/bin/mediamtx'
```

---

## 九、常见问题

### 9.1 setup 脚本报 `wsl.exe : wsl: 检测到 localhost 代理配置...`

这是 `wsl.exe` 写到 **stderr** 的警告，不是真正的错误。Windows PowerShell 5.1 在 `$ErrorActionPreference = "Stop"` 下会把它当成 `NativeCommandError`，脚本会在第 26 行退出。

之前脚本确实踩过这个坑，已经修复：

```powershell
$wslOut = cmd.exe /c "wsl.exe -d $Distro -- hostname -I 2>nul"
cmd.exe /c "netsh interface portproxy delete ... 2>nul" | Out-Null
$addOut = cmd.exe /c "netsh interface portproxy add ... 2>&1"
```

失败的那次没有执行任何 `netsh`，不会留下 portproxy/防火墙规则。重新运行 setup 即可。

### 9.2 服务器报 `bind: address already in use`

```bash
ss -ltn | grep 1935
pkill -9 -f 'scripts/rtmp_server/bin/mediamtx'
```

然后重新 `run_server.sh`。

### 9.3 板端连不上

按顺序检查：

1. 板端和 Windows 同局域网，`ping <Windows局域网IP>`；
2. `netsh interface portproxy show all` 有 1935 规则；
3. Windows 防火墙允许 1935/TCP（setup 脚本会加 `RK3568 RTMP 1935`）；
4. WSL 重启后 IP 变了，重新运行 setup；
5. 板端 `-o` 填的是 Windows 局域网 IP，不是板子 IP/WSL IP；
6. Windows 有 VPN/多网卡时，setup 打印的 IP 可能不对，按 `ipconfig` 换 WLAN IP。

### 9.4 预览一直“正在连接”

1. 服务器在跑：`ss -ltn | grep 1935`；
2. 有推流源：`test_publish.sh` 或板端；
3. MediaMTX 日志有 `is publishing to path 'live/imx415'`；
4. 预览地址路径也是 `live/imx415`；
5. 预览脚本对 RTMP 设置了低延迟探测，首次连接约 1~2 秒，稍等。

### 9.5 中文显示方块

WSL 系统没有中文字体。conda 环境里已经安装 `font-ttf-noto-cjk`；重建环境时确保 `environment.yml` 包含这一项。

### 9.6 预览窗口不出现

```bash
echo $DISPLAY
ls -d /mnt/wslg
```

WSLg 不可用时，可以在 Windows 上装 PyQt6/OpenCV 运行预览，或在 WSL 配 X server。

### 9.7 `bin/mediamtx` 不存在

```bash
./scripts/rtmp_server/download_mediamtx.sh
```

网络慢时可用镜像：

```bash
MEDIAMTX_URL="<镜像地址>" ./scripts/rtmp_server/download_mediamtx.sh --force
```

### 9.8 板端推流成功但预览没有画面

- 确认板端和预览连的是同一个服务器；
- 板端推 `rtmp://<WindowsIP>:1935/live/imx415`；
- 预览拉 `rtmp://127.0.0.1:1935/live/imx415`；
- 如果板端推流路径/流名不同，预览也要一致；
- 看 MediaMTX 日志是否有 `is publishing`。

---

## 十、验证记录

### 10.1 已验证

- WSL MediaMTX v1.21.3 启动，监听 `:1935`；
- `test_publish.sh` 用本地 `tmpfile/imx415.h264` 推流成功；
- WSL PyQt5 预览成功显示 `1920x1080`；
- WSLg xcb 平台可创建窗口；
- conda 环境隔离，系统 Python 未修改；
- 中文字体正常。

### 10.2 待验证

- 板端真实 IMX415 推流；
- Windows 端口转发的真实生效（需要管理员执行 setup 后验证）；
- 长时间运行（建议 30 分钟以上，观察内存、重连、丢帧）；
- 弱网/公网环境。

---

## 十一、安全说明

- MediaMTX 当前没有鉴权，只适合局域网开发调试；
- 不要把 1935 直接暴露到公网；
- 防火墙规则只放行 `LocalSubnet`；
- 需要公网使用时加 RTMP 鉴权、TLS、防火墙白名单。

---

## 十二、相关文档

- [RTMP 推流实现](RTMP推流实现.md) — u_ffmpeg 设计、H.264/FLV/RTMP 背景
- [scripts/README.md](../scripts/README.md)
- [scripts/rtmp_server/README.md](../scripts/rtmp_server/README.md)
- [scripts/rtmp_preview/README.md](../scripts/rtmp_preview/README.md)