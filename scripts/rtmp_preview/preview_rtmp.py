#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RK3568 摄像头视频预览工具（PyQt + OpenCV）

支持：
  - RTMP / RTSP / HTTP 流：在地址栏输入服务器 URL
  - 本地视频文件：点击“打开文件…”或把路径作为第一个参数

用法：
  python preview_rtmp.py
  python preview_rtmp.py rtmp://192.168.1.100:1935/live/imx415
  python preview_rtmp.py /path/to/imx415.h264

依赖：
  python -m pip install PyQt6 opencv-python
  也兼容 PyQt5，脚本会自动检测。
"""

import argparse
import os
import sys
import time

# ---------- Qt 兼容层：优先 PyQt6，其次 PyQt5 ----------
try:
    from PyQt6.QtCore import QRect, Qt, QThread, pyqtSignal
    from PyQt6.QtGui import QImage, QPainter
    from PyQt6.QtWidgets import (
        QApplication,
        QFileDialog,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QMessageBox,
        QPushButton,
        QSizePolicy,
        QStatusBar,
        QVBoxLayout,
        QWidget,
    )
    _QT6 = True
except ImportError:
    try:
        from PyQt5.QtCore import QRect, Qt, QThread, pyqtSignal
        from PyQt5.QtGui import QImage, QPainter
        from PyQt5.QtWidgets import (
            QApplication,
            QFileDialog,
            QHBoxLayout,
            QLabel,
            QLineEdit,
            QMessageBox,
            QPushButton,
            QSizePolicy,
            QStatusBar,
            QVBoxLayout,
            QWidget,
        )
        _QT6 = False
    except ImportError:
        print(
            "未找到 PyQt。请先安装：\n"
            "    python -m pip install PyQt6\n"
            "或：\n"
            "    python -m pip install PyQt5",
            file=sys.stderr,
        )
        sys.exit(1)

try:
    import cv2
except ImportError:
    cv2 = None

if _QT6:
    _ALIGN_CENTER = Qt.AlignmentFlag.AlignCenter
    _KEEP_ASPECT = Qt.AspectRatioMode.KeepAspectRatio
    _RGB888 = QImage.Format.Format_RGB888
    _EXPANDING = QSizePolicy.Policy.Expanding
    _BLACK = Qt.GlobalColor.black
    _GRAY = Qt.GlobalColor.gray
    _SMOOTH_PIXMAP = QPainter.RenderHint.SmoothPixmapTransform
else:
    _ALIGN_CENTER = Qt.AlignCenter
    _KEEP_ASPECT = Qt.KeepAspectRatio
    _RGB888 = QImage.Format_RGB888
    _EXPANDING = QSizePolicy.Expanding
    _BLACK = Qt.black
    _GRAY = Qt.gray
    _SMOOTH_PIXMAP = QPainter.SmoothPixmapTransform

DEFAULT_SOURCE = "rtmp://127.0.0.1:1935/live/imx415"
PREVIEW_WIDTH = 960
OPEN_TIMEOUT_MS = 10000
READ_TIMEOUT_MS = 5000
STREAM_PREFIXES = (
    "rtmp://", "rtmps://", "rtsp://", "rtsps://",
    "http://", "https://", "udp://", "tcp://",
)


class PreviewThread(QThread):
    """后台解码线程：ffmpeg/OpenCV 的阻塞读不能放在 GUI 线程。"""

    frame_ready = pyqtSignal(QImage)
    status_changed = pyqtSignal(str)
    failed = pyqtSignal(str)

    def __init__(self, source, parent=None):
        super().__init__(parent)
        self.source = source
        self._stop = False

    def stop(self):
        self._stop = True

    def _is_stream(self):
        return self.source.lower().startswith(STREAM_PREFIXES)

    def _open_capture(self):
        stream = self._is_stream()
        cap = None

        if stream:
            # RTMP 首次打开时 FFmpeg 默认会等较多探测数据；
            # 这几个选项让直播流尽快出画面（MediaMTX/SRS 都适用）。
            os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = (
                "rtmp_live;live|analyzeduration;0|probesize;32"
            )
            try:
                params = [
                    cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, OPEN_TIMEOUT_MS,
                    cv2.CAP_PROP_READ_TIMEOUT_MSEC, READ_TIMEOUT_MS,
                ]
                cap = cv2.VideoCapture(self.source, cv2.CAP_FFMPEG, params)
            except (AttributeError, TypeError, cv2.error):
                cap = None
        else:
            os.environ.pop("OPENCV_FFMPEG_CAPTURE_OPTIONS", None)

        if cap is None:
            cap = cv2.VideoCapture(self.source, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            cap.release()
            # 探测参数太激进时回退到默认探测，慢一点但兼容性更好
            os.environ.pop("OPENCV_FFMPEG_CAPTURE_OPTIONS", None)
            cap = cv2.VideoCapture(self.source)
        if cap.isOpened():
            try:
                cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            except Exception:
                pass
        return cap

    def _sleep(self, seconds):
        end = time.monotonic() + seconds
        while not self._stop and time.monotonic() < end:
            time.sleep(0.1)

    def _emit_frame(self, frame):
        h, w = frame.shape[:2]
        if w > PREVIEW_WIDTH:
            new_h = max(1, int(round(h * (PREVIEW_WIDTH / float(w)))))
            frame = cv2.resize(frame, (PREVIEW_WIDTH, new_h),
                               interpolation=cv2.INTER_AREA)
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        h, w = rgb.shape[:2]
        image = QImage(rgb.tobytes(), w, h, w * 3, _RGB888).copy()
        self.frame_ready.emit(image)

    def _preview_loop(self, cap, stream):
        fps = 0.0
        if not stream:
            fps = cap.get(cv2.CAP_PROP_FPS)
            if not (0.0 < fps < 240.0):
                fps = 30.0

        width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        if width > 0 and height > 0:
            self.status_changed.emit("预览中  %dx%d" % (width, height))
        else:
            self.status_changed.emit("预览中")

        while not self._stop:
            started = time.monotonic()
            ok, frame = cap.read()
            if not ok:
                return
            self._emit_frame(frame)
            if fps > 0:
                delay = (1.0 / fps) - (time.monotonic() - started)
                if delay > 0:
                    time.sleep(delay)

    def run(self):
        if cv2 is None:
            self.failed.emit("未安装 opencv-python，无法解码视频")
            return

        stream = self._is_stream()
        while not self._stop:
            cap = self._open_capture()
            if not cap.isOpened():
                cap.release()
                if self._stop:
                    break
                if not stream:
                    self.failed.emit("无法打开视频文件：\n%s" % self.source)
                    break
                self.status_changed.emit("连接失败，2 秒后重试…")
                self._sleep(2.0)
                continue

            self._preview_loop(cap, stream)
            cap.release()

            if self._stop:
                break
            if not stream:
                self.status_changed.emit("播放结束")
                break

            # 直播流断开后自动重连，和 rkCam 的推流重连配合
            self.status_changed.emit("连接断开，2 秒后重试…")
            self._sleep(2.0)

        self.status_changed.emit("已停止")


class VideoLabel(QWidget):
    """按比例居中绘制 QImage 的显示控件。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._image = None
        self._message = "未开始预览"
        self.setMinimumSize(640, 360)
        self.setSizePolicy(_EXPANDING, _EXPANDING)

    def set_image(self, image):
        self._image = image
        self._message = ""
        self.update()

    def set_message(self, message):
        self._image = None
        self._message = message
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), _BLACK)

        if self._image is not None and not self._image.isNull():
            target = self._image.size().scaled(self.size(), _KEEP_ASPECT)
            x = (self.width() - target.width()) // 2
            y = (self.height() - target.height()) // 2
            painter.setRenderHint(_SMOOTH_PIXMAP, True)
            painter.drawImage(QRect(x, y, target.width(), target.height()),
                              self._image)
            return

        if self._message:
            painter.setPen(_GRAY)
            painter.drawText(self.rect(), _ALIGN_CENTER, self._message)


class PreviewWindow(QWidget):
    def __init__(self, source):
        super().__init__()
        self.thread = None
        self._build_ui(source)
        self._apply_style()

    def _build_ui(self, source):
        self.setWindowTitle("RK3568 摄像头预览")
        self.resize(1000, 680)
        self.setMinimumSize(720, 520)

        self.source_edit = QLineEdit(source)
        self.source_edit.setPlaceholderText(
            "rtmp://服务器IP:1935/live/imx415 或本地 .h264/.mp4"
        )
        self.source_edit.setMinimumWidth(320)
        self.source_edit.returnPressed.connect(self.start_preview)

        self.open_btn = QPushButton("打开文件…")
        self.open_btn.clicked.connect(self.choose_file)

        self.start_btn = QPushButton("开始预览")
        self.start_btn.clicked.connect(self.start_preview)

        self.stop_btn = QPushButton("停止")
        self.stop_btn.clicked.connect(self.stop_preview)
        self.stop_btn.setEnabled(False)

        top = QHBoxLayout()
        top.addWidget(QLabel("地址："))
        top.addWidget(self.source_edit, 1)
        top.addWidget(self.open_btn)
        top.addWidget(self.start_btn)
        top.addWidget(self.stop_btn)

        self.video = VideoLabel()

        hint = QLabel(
            "提示：-o 里填的是 RTMP 服务器地址，不是板端 IP；本地文件可直接输入路径。"
        )
        hint.setStyleSheet("color: #888888;")
        hint.setWordWrap(True)

        self.status = QStatusBar()
        self.status.showMessage("就绪")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 6)
        layout.addLayout(top)
        layout.addWidget(self.video, 1)
        layout.addWidget(hint)
        layout.addWidget(self.status)

    def _apply_style(self):
        self.setStyleSheet(
            """
            QWidget {
                background-color: #202020;
                color: #e6e6e6;
            }
            QLineEdit {
                background-color: #2b2b2b;
                border: 1px solid #454545;
                border-radius: 4px;
                padding: 6px;
            }
            QPushButton {
                background-color: #3a3a3a;
                border: 1px solid #555555;
                border-radius: 4px;
                padding: 6px 14px;
            }
            QPushButton:hover {
                background-color: #4a4a4a;
            }
            QPushButton:disabled {
                color: #777777;
            }
            QStatusBar {
                background-color: #262626;
            }
            """
        )

    def choose_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self,
            "选择视频文件",
            "",
            "视频文件 (*.h264 *.264 *.mp4 *.flv *.mkv *.ts);;所有文件 (*)",
        )
        if path:
            self.source_edit.setText(path)

    def start_preview(self):
        if self.thread is not None:
            return

        source = self.source_edit.text().strip()
        if not source:
            self.status.showMessage("请输入 RTMP 地址或选择本地文件")
            return
        if cv2 is None:
            QMessageBox.critical(
                self,
                "缺少依赖",
                "未安装 opencv-python，无法解码视频。\n\n"
                "python -m pip install opencv-python",
            )
            return

        self.video.set_message("连接中…")
        self.start_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.source_edit.setEnabled(False)
        self.open_btn.setEnabled(False)
        self.status.showMessage("正在连接 %s …" % source)

        self.thread = PreviewThread(source, self)
        self.thread.frame_ready.connect(self.on_frame)
        self.thread.status_changed.connect(self.on_status)
        self.thread.failed.connect(self.on_error)
        self.thread.finished.connect(self.on_thread_finished)
        self.thread.start()

    def stop_preview(self):
        if self.thread is None:
            return
        self.status.showMessage("正在停止…")
        self.stop_btn.setEnabled(False)
        self.thread.stop()

    def on_frame(self, image):
        self.video.set_image(image)

    def on_status(self, message):
        self.status.showMessage(message)

    def on_error(self, message):
        self.status.showMessage(message.replace("\n", " "))
        self.video.set_message("预览失败\n" + message)

    def on_thread_finished(self):
        self.start_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.source_edit.setEnabled(True)
        self.open_btn.setEnabled(True)
        if self.thread is not None:
            self.thread.deleteLater()
            self.thread = None

    def closeEvent(self, event):
        if self.thread is not None:
            self.thread.stop()
            if not self.thread.wait(3000):
                self.thread.terminate()
                self.thread.wait(1000)
            self.thread = None
        super().closeEvent(event)


def parse_args():
    parser = argparse.ArgumentParser(description="RK3568 摄像头视频预览")
    parser.add_argument(
        "source",
        nargs="?",
        default=DEFAULT_SOURCE,
        help="RTMP 地址或本地视频文件路径（默认 %s）" % DEFAULT_SOURCE,
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if not _QT6:
        QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
        QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QApplication(sys.argv)
    window = PreviewWindow(args.source)
    window.show()
    return app.exec() if _QT6 else app.exec_()


if __name__ == "__main__":
    sys.exit(main())