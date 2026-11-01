# WSL RTMP 服务（MediaMTX）

在 WSL 里运行 MediaMTX，接收板端 `rkCam` 推流，并给 WSL 预览脚本拉流。

## 数据流

```text
板端 rkCam
  -> rtmp://<Windows局域网IP>:1935/live/imx415
  -> Windows netsh 端口转发（0.0.0.0:1935 -> WSL_IP:1935）
  -> WSL MediaMTX 监听 :1935
  -> WSL 预览 rtmp://127.0.0.1:1935/live/imx415
```

WSL2 默认是 NAT，板子不能直接访问 WSL 的 `172.x` 地址，所以需要 Windows 侧做 1935 端口转发。
当前机器的示例：Windows WLAN `192.168.10.31`，WSL `172.26.177.124`（IP 会变，以脚本输出为准）。

## 启动

### 1. WSL 启动 MediaMTX

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_server/run_server.sh
```

看到下面这行说明 RTMP 已监听：

```text
[RTMP] started with listener on :1935 (TCP/RTMP)
```

### 2. Windows 管理员建立端口转发

**第一次使用、以及每次 `wsl --shutdown` 之后**都要执行一次（WSL IP 会变）。
以管理员身份打开 PowerShell，执行：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\setup_windows_portproxy.ps1"
```

脚本会：

- 读取当前 WSL IP；
- 读取 Windows 局域网 IP；
- 建立 `Windows 0.0.0.0:1935 -> WSL_IP:1935` 转发；
- 添加只允许局域网来源的 1935/TCP 防火墙规则；
- 打印板端推流地址和 WSL 预览地址。

脚本会打印类似：

```text
板端推流：
  ./rkCam -o rtmp://192.168.10.31:1935/live/imx415
```

如果 Windows 有多个网卡（VPN、虚拟机网卡），脚本默认使用默认路由所在网卡；打印出来的 IP 不对时，用 `ipconfig` 找到 WLAN/以太网 IP，手工改板端命令即可。

### 3. WSL 预览

另开一个 WSL 终端：

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_preview/run_preview.sh rtmp://127.0.0.1:1935/live/imx415
```

也可以不写地址，直接 `./scripts/rtmp_preview/run_preview.sh`，默认就是这个 URL。

### 4. 板端推流

把上一步脚本打印的 Windows 局域网 IP 填进 `-o`：

```bash
./rkCam -o rtmp://192.168.10.31:1935/live/imx415
```

> 注意：这里不是板子 IP，也不是 WSL 的 172.x IP，而是 **Windows 的局域网 IP**。

## 没有板子时本地测试

准备三个 WSL 终端：

```bash
# 终端 1：服务器
./scripts/rtmp_server/run_server.sh

# 终端 2：用 tmpfile/imx415.h264 模拟板端推流
./scripts/rtmp_server/test_publish.sh

# 终端 3：预览
./scripts/rtmp_preview/run_preview.sh
```

`test_publish.sh` 会先把裸 `.h264` 封装成临时 MP4 补时间戳，再循环推流，避免部分客户端拉流卡住。

## 停止与清理

- 服务器、推流、预览都在各自终端按 `Ctrl-C`。
- 删除 Windows 端口转发和防火墙规则（管理员 PowerShell）：

```powershell
powershell -ExecutionPolicy Bypass -File "\\wsl.localhost\Ubuntu-20.04\root\rk3568\rk_project\mediaCam\scripts\rtmp_server\remove_windows_portproxy.ps1"
```

## 文件说明

| 文件 | 说明 |
|---|---|
| `bin/mediamtx` | MediaMTX 二进制（体积大，不提交到 git；可用下载脚本重装） |
| `mediamtx.yml` | 只开 RTMP 1935 的最小配置 |
| `run_server.sh` | 前台启动服务器，Ctrl-C 停止 |
| `test_publish.sh` | 用本地视频模拟板端推流 |
| `download_mediamtx.sh` | 下载/更新 MediaMTX 二进制 |
| `setup_windows_portproxy.ps1` | Windows 管理员运行，建立 1935 转发 |
| `remove_windows_portproxy.ps1` | Windows 管理员运行，删除转发和防火墙规则 |

## 排查

```bash
# WSL 里看 1935 是否在监听
ss -ltn | grep 1935

# WSL 里看服务器日志
# run_server.sh 前台会直接打印

# Windows 里看端口转发
netsh interface portproxy show all

# Windows 里看 1935 监听
netstat -ano | findstr :1935

# 板端到 Windows 的连通性
ping <Windows局域网IP>
```

常见问题：

- **板端连不上**：先确认 Windows 端口转发和防火墙；再确认板端和 Windows 在同一局域网。
- **WSL 重启后板端连不上**：WSL IP 变了，重新运行 `setup_windows_portproxy.ps1`。
- **运行 setup 脚本时看到 `wsl: 检测到 localhost 代理配置...`**：这是 WSL 对 Windows 代理设置的提示，不影响 RTMP；脚本已经屏蔽了 `wsl.exe` 的 stderr，不会因此中断。
- **服务器启动报 `bind: address already in use`**：旧 MediaMTX 还在跑，先 `pkill -f mediamtx` 或重启 WSL。
- **预览一直“正在连接”**：确认 `test_publish.sh` 或板端已经推流成功，服务器日志里出现 `is publishing to path 'live/imx415'`。

## 安全说明

MediaMTX 配置没有开鉴权，只适合局域网开发调试。不要直接暴露到公网；需要公网使用时加认证/防火墙。

## 可选：WSL 镜像网络（不用端口转发）

如果 Windows 11 版本支持 WSL 镜像网络，可以在 `%UserProfile%\.wslconfig` 中加：

```ini
[wsl2]
networkingMode=mirrored
```

然后 `wsl --shutdown` 重启。这样 WSL 直接使用 Windows 的网络身份，板端可以直接推 `rtmp://<Windows局域网IP>:1935/...`，不再需要 `netsh` 转发。
镜像网络可能影响 Docker/VPN，按需使用。