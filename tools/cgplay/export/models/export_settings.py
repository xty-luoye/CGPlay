"""ExportSettings — 导出参数统一模型"""

from dataclasses import dataclass, field
from typing import Optional

SUPPORTED_CODECS = {
    "h264",
    "h265",
    "prores_422",
    "prores_hq",
    "prores_4444",
    "dnxhr",
}

CODEC_ALIASES = {
    "libx264": "h264",
    "libx265": "h265",
}


@dataclass
class ExportSettings:
    """控制视频导出的所有参数"""
    fps: float = 24.0
    codec: str = "h264"          # h264, h265, prores_422, prores_hq, prores_4444, dnxhr
    bitrate: str = "20M"         # 10M, 20M, 50M, 100M 或 "20000000"
    output_path: str = ""
    output_format: str = "mp4"   # mp4, mov
    width: int = 1920
    height: int = 1080
    pixel_format: str = "yuv420p"

    # FFmpeg enum values. BT.709 limited is the safe default for ordinary video.
    color_range: int = 1
    color_space: int = 1
    color_primaries: int = 1
    color_transfer: int = 1

    # OCIO (future)
    ocio_config: str = ""
    ocio_input: str = ""
    ocio_display: str = ""
    ocio_view: str = ""

    # Log
    log_file: Optional[str] = None

    def normalized_codec(self) -> str:
        codec = CODEC_ALIASES.get(self.codec.strip().lower(), self.codec.strip().lower())
        if codec not in SUPPORTED_CODECS:
            raise ValueError(f"Unsupported export codec: {self.codec}")
        return codec

    def parse_bitrate(self) -> int:
        """将 '20M' 转换为 20000000"""
        s = self.bitrate.upper().strip()
        if s.endswith("M"):
            return int(float(s[:-1]) * 1_000_000)
        if s.endswith("K"):
            return int(float(s[:-1]) * 1_000)
        return int(s)
