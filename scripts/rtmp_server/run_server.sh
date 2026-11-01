#!/usr/bin/env bash
# 前台启动 MediaMTX，按 Ctrl-C 停止。
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/bin/mediamtx"
CONF="$SCRIPT_DIR/mediamtx.yml"

if [ ! -x "$BIN" ]; then
    "$SCRIPT_DIR/download_mediamtx.sh"
fi

echo "MediaMTX RTMP 监听 :1935"
echo "WSL 预览: rtmp://127.0.0.1:1935/live/imx415"
echo "板端推流: rtmp://<Windows局域网IP>:1935/live/imx415"
echo "按 Ctrl-C 停止"
echo

exec "$BIN" "$CONF"