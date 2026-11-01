# scripts

| 目录 | 说明 |
|---|---|
| [rtmp_server](rtmp_server/README.md) | WSL 端 MediaMTX RTMP 服务、Windows 1935 端口转发、本地模拟推流 |
| [rtmp_preview](rtmp_preview/README.md) | PyQt + OpenCV 预览（WSL / Windows），支持 RTMP 和本地文件 |

推荐顺序：先启动 `rtmp_server`，再在板端运行 `rkCam -o ...` 推流，最后启动 `rtmp_preview` 拉流。

完整记录见 [../docs/RTMP推流与WSL预览环境.md](../docs/RTMP推流与WSL预览环境.md)。