#!/usr/bin/env bash
# 下载 MediaMTX Linux amd64 二进制到 bin/mediamtx。
# 默认从 GitHub 下载；网络慢时可覆盖 MEDIAMTX_URL。
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$SCRIPT_DIR/bin"
BIN="$BIN_DIR/mediamtx"
VERSION="${MEDIAMTX_VERSION:-v1.21.3}"
URL="${MEDIAMTX_URL:-https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/mediamtx_${VERSION}_linux_amd64.tar.gz}"

if [ -x "$BIN" ] && [ "${1:-}" != "--force" ]; then
    echo "已存在: $BIN"
    "$BIN" --version
    exit 0
fi

mkdir -p "$BIN_DIR"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "下载: $URL"
curl -L --fail --retry 2 --connect-timeout 20 -o "$TMP_DIR/mediamtx.tar.gz" "$URL"
tar -xzf "$TMP_DIR/mediamtx.tar.gz" -C "$TMP_DIR"
install -m 755 "$TMP_DIR/mediamtx" "$BIN"

echo "安装完成: $BIN"
"$BIN" --version