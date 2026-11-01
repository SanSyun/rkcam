# 视频预览脚本

PyQt 视频预览小工具：在 PC 上查看 RK3568 推上来的 RTMP 流，或直接播放本地 `.h264` / `.mp4` 文件。

RTMP 服务端在 WSL 里的启动方式见 [../rtmp_server/README.md](../rtmp_server/README.md)。

## 已装好的 WSL 环境

本机 WSL（Ubuntu 20.04）已经用 Miniforge 建好独立环境，**没有修改系统 Python，也没有改 `~/.bashrc`**：

- conda 安装目录：`/root/miniforge3`
- 环境名：`mediaCam-preview`
- 内容：Python 3.11、PyQt5、OpenCV、Noto CJK 字体

直接运行：

```bash
cd /root/rk3568/rk_project/mediaCam
./scripts/rtmp_preview/run_preview.sh
./scripts/rtmp_preview/run_preview.sh rtmp://192.168.1.100:1935/live/imx415
./scripts/rtmp_preview/run_preview.sh tmpfile/imx415.h264
```

`run_preview.sh` 只在当前进程里 `source conda.sh` 并激活环境，不会写 `~/.bashrc`。

需要手动进入环境时：

```bash
source /root/miniforge3/etc/profile.d/conda.sh
conda activate mediaCam-preview
python scripts/rtmp_preview/preview_rtmp.py
```

> WSLg 已可用（`DISPLAY=:0`），可以直接弹窗。如果窗口不出现，先在 WSL 里确认 `echo $DISPLAY` 和 `/mnt/wslg`。

> 在 WSL 里预览时注意：`127.0.0.1` 指的是 WSL 自己。如果 RTMP 服务跑在 Windows 上，请填 Windows 主机在 WSL 里的地址（`ip route show default | awk '{print $3}'`）；如果服务跑在板端或另一台机器，填对应 IP。

## 重建环境（可选）

在另一台机器上安装 Miniforge（不要执行 `conda init`）：

```bash
curl -L -o /tmp/miniforge.sh \
  https://mirrors.tuna.tsinghua.edu.cn/github-release/conda-forge/miniforge/LatestRelease/Miniforge3-Linux-x86_64.sh
bash /tmp/miniforge.sh -b -p "$HOME/miniforge3"
```

然后在仓库根目录创建环境：

```bash
"$HOME/miniforge3/bin/conda" env create -f scripts/rtmp_preview/environment.yml
```

## Windows / 已有 Python 的环境

```bash
python -m pip install -r scripts/rtmp_preview/requirements.txt
python scripts/rtmp_preview/preview_rtmp.py
```

如果环境里已经有 PyQt5，也可以只装 `PyQt5 opencv-python`，脚本会自动回退到 PyQt5。

## 说明

- 直播流断开后每 2 秒自动重连；点“停止”结束预览。
- 解码依赖 OpenCV 的 FFmpeg 后端，`opencv-python` 预编译包通常已带 RTMP。
- 如果 OpenCV 打不开 RTMP，可以先用 `ffplay rtmp://...` 确认流本身正常。
- 预览只解码显示，不落盘。