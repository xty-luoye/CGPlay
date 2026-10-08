"""VideoReader — 使用 PyAV 解码视频文件，返回 RGB uint8 帧"""

import os
import av
import numpy as np
from typing import Iterator, Optional


class VideoReader:
    """使用 PyAV 解码视频文件（MP4/MOV/MXF 等）"""

    def __init__(self):
        self._container = None
        self._stream = None

    def open(self, path: str):
        """打开视频文件"""
        self._container = av.open(path)
        self._stream = self._container.streams.video[0]
        return self

    def close(self):
        """关闭容器"""
        if self._container:
            self._container.close()
            self._container = None
            self._stream = None

    @property
    def fps(self) -> float:
        """获取视频帧率"""
        if self._stream and self._stream.average_rate:
            return float(self._stream.average_rate)
        return 24.0

    @property
    def frame_count(self) -> int:
        """获取总帧数"""
        declared_frames = int(getattr(self._stream, "frames", 0) or 0)
        if declared_frames > 0:
            return declared_frames
        duration_seconds = 0.0
        stream_duration = getattr(self._stream, "duration", None)
        stream_time_base = getattr(self._stream, "time_base", None)
        if stream_duration is not None and stream_time_base is not None:
            duration_seconds = float(stream_duration * stream_time_base)
        elif self._container and self._container.duration is not None:
            duration_seconds = float(self._container.duration) / float(av.time_base)
        if duration_seconds > 0 and self.fps > 0:
            return max(1, int(round(duration_seconds * self.fps)))
        return 0

    @property
    def width(self) -> int:
        if self._stream:
            return self._stream.width
        return 0

    @property
    def height(self) -> int:
        if self._stream:
            return self._stream.height
        return 0

    @property
    def color_metadata(self) -> dict[str, int]:
        """Return the source's FFmpeg color enum values.

        FFmpeg uses ``0`` for ``UNSPECIFIED``.  Keep that value when a source
        does not carry color metadata instead of silently relabelling it as
        BT.709.  The exporter can then leave the conversion/metadata choice to
        the encoder for genuinely unspecified inputs.
        """
        context = getattr(self._stream, "codec_context", None)
        fields = {
            "color_range": "color_range",
            "color_space": "colorspace",
            "color_primaries": "color_primaries",
            "color_transfer": "color_trc",
        }
        metadata = {}
        for setting_name, av_name in fields.items():
            value = getattr(self._stream, av_name, 0) if self._stream else 0
            if not value and context is not None:
                value = getattr(context, av_name, 0)
            metadata[setting_name] = int(value or 0)
        return metadata

    def read_frames(
        self,
        start_frame: int = 0,
        end_frame: Optional[int] = None,
    ) -> Iterator[np.ndarray]:
        """
        读取指定范围的帧，返回 RGB uint8 (H, W, 3) 列表。
        """
        if not self._container or not self._stream:
            raise RuntimeError("VideoReader is not open")
        self._container.seek(0)

        frame_idx = 0
        for packet in self._container.demux(self._stream):
            for frame in packet.decode():
                if end_frame is not None and frame_idx > end_frame:
                    return
                if frame_idx < start_frame:
                    frame_idx += 1
                    continue

                # YUV → RGB 转换
                source_format = frame.format.name.lower() if frame.format else ""
                if "yuva" in source_format or "rgba" in source_format or "bgra" in source_format:
                    yield frame.to_ndarray(format="bgra")
                else:
                    yield frame.to_ndarray(format="bgr24")

                # PyAV RGB → BGR (与 image_reader 保持一致的 BGR 格式)
                frame_idx += 1


    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


# 已知视频扩展名
VIDEO_EXTENSIONS = {'.mp4', '.mov', '.avi', '.mkv', '.mxf', '.webm', '.flv', '.wmv', '.m4v'}


def is_video_file(path: str) -> bool:
    """判断是否为视频文件"""
    ext = os.path.splitext(path)[1].lower()
    return ext in VIDEO_EXTENSIONS
