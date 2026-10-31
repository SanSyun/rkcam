#!/usr/bin/env bash

set -euo pipefail

WIDTH=3864
HEIGHT=2192
FPS=25
FRAME_COUNT=75
PREFIX="outpics/imx415_"

# 可以通过第一个参数指定输出文件名
OUTPUT="${1:-imx415_25fps.mp4}"

# NV12 每帧大小：宽 × 高 × 1.5
EXPECTED_SIZE=$((WIDTH * HEIGHT * 3 / 2))

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "错误：没有找到 ffmpeg" >&2
    exit 1
fi

# 检查所有帧是否存在、大小是否正确
for ((i = 0; i < FRAME_COUNT; i++)); do
    file="${PREFIX}${i}.nv12"

    if [[ ! -f "$file" ]]; then
        echo "错误：找不到文件 $file" >&2
        exit 1
    fi

    actual_size=$(stat -c '%s' "$file")

    if [[ "$actual_size" -ne "$EXPECTED_SIZE" ]]; then
        echo "错误：$file 大小异常" >&2
        echo "期望：${EXPECTED_SIZE} 字节" >&2
        echo "实际：${actual_size} 字节" >&2
        exit 1
    fi
done

echo "开始生成视频："
echo "  帧数：${FRAME_COUNT}"
echo "  分辨率：${WIDTH}x${HEIGHT}"
echo "  帧率：${FPS} fps"
echo "  输出：${OUTPUT}"

# NV12 文件可以按帧顺序直接进行字节拼接
{
    for ((i = 0; i < FRAME_COUNT; i++)); do
        cat -- "${PREFIX}${i}.nv12"
    done
} | ffmpeg \
    -hide_banner \
    -y \
    -f rawvideo \
    -pixel_format nv12 \
    -video_size "${WIDTH}x${HEIGHT}" \
    -framerate "$FPS" \
    -i pipe:0 \
    -frames:v "$FRAME_COUNT" \
    -an \
    -c:v libx264 \
    -preset medium \
    -crf 18 \
    -pix_fmt yuv420p \
    -movflags +faststart \
    "$OUTPUT"

echo "视频生成完成：${OUTPUT}"