#!/usr/bin/env bash
# 启动 PC 端预览脚本；只在本进程内激活 conda 环境，不修改 ~/.bashrc。
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONDA_ROOT="${CONDA_ROOT:-$HOME/miniforge3}"
ENV_NAME="${ENV_NAME:-mediaCam-preview}"

if [ ! -f "$CONDA_ROOT/etc/profile.d/conda.sh" ]; then
    echo "未找到 conda: $CONDA_ROOT" >&2
    echo "请先安装 Miniforge，或设置 CONDA_ROOT 指向安装目录。" >&2
    exit 1
fi

# shellcheck disable=SC1090
source "$CONDA_ROOT/etc/profile.d/conda.sh"
conda activate "$ENV_NAME"

exec python "$SCRIPT_DIR/preview_rtmp.py" "$@"