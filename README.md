# mediaCam

RK3568 平台上的摄像头采集与硬件编码练手工程：从**自写 IMX415 驱动**出发，串起 **V4L2 采集 → DMA/零拷贝 → RGA 缩放 → MPP 硬编** 的完整链路。

目标板：ATK DLRK3568（RK3568，Linux 5.10），交叉编译，程序在板端运行。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `driver/imx415/` | 自写 IMX415 V4L2 子设备驱动（I2C 寄存器读写、三路电源管理、V4L2 subdev 注册），产出 `my_imx415.ko` |
| `tests/imx415_capture/` | 最小 V4L2 采集：MMAP + MPLANE，输出 3840x2160 NV12 |
| `tests/mpp_encode/` | 采集 + `librockchip_mpp` 硬件 H.264 编码，落盘 `imx415.h264` |
| `tests/rga_capture/` | 采集 + RGA 4K→1080p 缩放 + MPP 编码（`rga_mpp`），含 DMA heap 零拷贝尝试 |
| `baseline/` | SDK 原始固件与分区/版本报告；`factory/*.img` 体积过大，**不入库** |
| `docs/` | 技术文档：编码性能复盘、RGA 练习题 |
| `tmpfile/` | 本地测试输出目录（原始帧、yuv、mp4 均不入库）；`play_nv12.sh` 把 NV12 序列合成视频 |
| `.vscode/` | 交叉编译的 IntelliSense 配置（`c_cpp_properties.json`） |

## 环境依赖

- 内核源码：`/root/rk3568/rk3568_linux_sdk/kernel`（Makefile 中 `KERNELDIR`，可按需覆盖）
- 工具链：`aarch64-buildroot-linux-gnu-gcc`（默认在 `/opt/atk-dlrk3568-5_10_sdk-toolchain/bin`）
- 板端库：`librockchip_mpp`、`librga`，以及 `/dev/dma_heap/system`
- 调试依赖：ASan（`tests/*` 的 Makefile 默认带 `-fsanitize=address`，发布时请去掉）

## 编译

内核模块：

```bash
cd driver/imx415
make                                  # 等价于 make -C /root/rk3568/rk3568_linux_sdk/kernel M=$PWD modules
make clean
```

测试程序：

```bash
cd tests/mpp_encode                   # 或 tests/imx415_capture、tests/rga_capture
make
make run                              # 板端执行
```

> 各 `Makefile` 里的 SDK 路径是作者本机绝对路径，换机器请先改 `KERNELDIR` / `SYSROOT` / `TOOLCHAIN_DIR`。

## 技术文档

- [mpp_encode 编码性能问题复盘](docs/mpp_encode_编码性能问题复盘.md) — 三阶段分段计时定位瓶颈，结论是编码链路达标、14 秒开销来自 `librockchip_mpp` 退出清理
- [RGA 练习题：4K NV12 → 1080p 零拷贝缩放](docs/RGA练习_4K转1080p.md) — 任务分解 M0~M3、验收清单与常见坑

## 关于仓库内容

工程原始体积约 3.8 GB，其中固件镜像（`update.img` 1.4 GB、`rootfs.img` 1.3 GB）与原始帧序列（75 帧 x 12 MB NV12）占绝大部分，超出 GitHub 单文件 100 MB 上限，故在 `.gitignore` 中全部排除。仓库只保留可编译的源码、Makefile 与文档。