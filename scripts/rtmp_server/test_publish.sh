#!/usr/bin/env bash
# 用本地 .h264/.mp4 模拟板端推流，便于在没有板子时测试服务器和预览。
# 用法: ./test_publish.sh [视频文件] [RTMP地址]
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="${1:-$ROOT/tmpfile/imx415.h264}"
URL="${2:-rtmp://127.0.0.1:1935/live/imx415}"

if [ ! -f "$SRC" ]; then
    echo "找不到视频文件: $SRC" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# 裸 .h264 没有时间戳，先封装成 MP4，否则部分 RTMP 客户端拉流会卡住
case "${SRC##*.}" in
    h264|264)
        echo "封装 H.264 为 MP4（补时间戳）..."
        ffmpeg -y -hide_banner -loglevel error -f h264 -i "$SRC" -c copy "$TMP_DIR/test.mp4"
        SRC="$TMP_DIR/test.mp4"
        ;;
esac

echo "模拟板端推流: $SRC"
echo "目标: $URL"
ffmpeg -hide_banner -loglevel warning -re -stream_loop -1 -i "$SRC" -c copy -f flv "$URL"