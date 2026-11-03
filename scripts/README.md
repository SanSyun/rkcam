# scripts

| 目录 | 说明 |
|---|---|
| [rtmp_server](rtmp_server/README.md) | WSL 端 MediaMTX RTMP 服务、Windows 1935 端口转发、本地模拟推流 |
| [rtmp_preview](rtmp_preview/README.md) | PyQt + OpenCV 预览（WSL / Windows），支持 RTMP 和本地文件 |

推荐顺序：先启动 `rtmp_server`，再在板端运行 `rkCam -o ...`（或 `-or` 边推边录）推流，最后启动 `rtmp_preview` 拉流。

环境与操作见 [../docs/RTMP预览操作.md](../docs/RTMP预览操作.md)，推流实现见 [../docs/RTMP推流实现.md](../docs/RTMP推流实现.md)。