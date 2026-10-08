"""FFmpegExporter - encode video with PyAV/libavcodec."""

import logging
from fractions import Fraction

import av
from av.video.reformatter import ColorRange, Colorspace, VideoReformatter

try:
    from ..models.export_settings import ExportSettings
except ImportError:
    from cgplay.export.models.export_settings import ExportSettings  # type: ignore

logger = logging.getLogger(__name__)

CODEC_MAP = {
    "h264": {"name": "libx264", "fmt": "mp4", "pixel_format": "yuv420p"},
    "h265": {"name": "libx265", "fmt": "mp4", "pixel_format": "yuv420p"},
    "prores_422": {"name": "prores_ks", "fmt": "mov", "profile": "standard", "pixel_format": "yuv422p10le"},
    "prores_hq": {"name": "prores_ks", "fmt": "mov", "profile": "hq", "pixel_format": "yuv422p10le"},
    "prores_4444": {"name": "prores_ks", "fmt": "mov", "profile": "4444", "pixel_format": "yuva444p10le"},
    # DNxHR HQX is the 10-bit 4:2:2 profile.  ``dnxhr_hq`` only accepts
    # ``yuv422p`` (8-bit); pairing it with ``yuv422p10le`` makes avcodec_open2
    # fail with EINVAL before the first frame is encoded.
    "dnxhr": {
        "name": "dnxhd",
        "fmt": "mov",
        "profile": "dnxhr_hqx",
        "pixel_format": "yuv422p10le",
    },
}

COLORSPACE_MAP = {
    1: Colorspace.ITU709,
    4: Colorspace.FCC,
    5: Colorspace.ITU601,
    6: Colorspace.ITU601,
    7: Colorspace.SMPTE240M,
}


class FFmpegExporter:
    """Encode frames to a movie file using PyAV."""

    def __init__(self, settings: ExportSettings):
        self.settings = settings
        self._container = None
        self._stream = None
        self._frame_count = 0
        self._reformatter = VideoReformatter()

    def _get_codec_info(self):
        codec_key = self.settings.normalized_codec()
        return codec_key, CODEC_MAP[codec_key]

    def open(self):
        codec_key, info = self._get_codec_info()
        logger.info("Opening encoder: %s -> %s", info["name"], self.settings.output_path)

        self._container = av.open(self.settings.output_path, mode="w")
        rate = Fraction(self.settings.fps).limit_denominator()
        stream = self._container.add_stream(info["name"], rate=rate)

        stream.width = self.settings.width
        stream.height = self.settings.height

        if codec_key.startswith("prores"):
            stream.pix_fmt = info["pixel_format"]
            stream.options = {"profile": info.get("profile", "standard")}
        elif codec_key == "dnxhr":
            stream.pix_fmt = info["pixel_format"]
            stream.options = {"profile": info["profile"]}
        else:
            stream.pix_fmt = info["pixel_format"]
            stream.bit_rate = self.settings.parse_bitrate()

        codec = stream.codec_context
        codec.color_range = int(self.settings.color_range)
        codec.colorspace = int(self.settings.color_space)
        codec.color_primaries = int(self.settings.color_primaries)
        codec.color_trc = int(self.settings.color_transfer)

        self._stream = stream
        self._frame_count = 0
        logger.info(
            "Stream: %sx%s @ %sfps %s codec=%s",
            stream.width,
            stream.height,
            self.settings.fps,
            stream.pix_fmt,
            info["name"],
        )

    def write_frame(self, frame_bgr: "np.ndarray"):
        import numpy as np

        if self._stream is None:
            raise RuntimeError("Exporter not opened. Call open() first.")

        h, w = frame_bgr.shape[:2]
        channels = frame_bgr.shape[2] if frame_bgr.ndim == 3 else 1
        if channels not in (3, 4):
            raise ValueError(f"Expected BGR or BGRA frame, got shape {frame_bgr.shape}")
        if h != self.settings.height or w != self.settings.width:
            from PIL import Image

            if channels == 4:
                rgba = frame_bgr[:, :, [2, 1, 0, 3]]
                resized = Image.fromarray(rgba, "RGBA").resize(
                    (self.settings.width, self.settings.height),
                    Image.Resampling.LANCZOS,
                )
                frame_bgr = np.array(resized)[:, :, [2, 1, 0, 3]]
            else:
                rgb = frame_bgr[:, :, ::-1]
                resized = Image.fromarray(rgb, "RGB").resize(
                    (self.settings.width, self.settings.height),
                    Image.Resampling.LANCZOS,
                )
                frame_bgr = np.array(resized)[:, :, ::-1]

        av_frame = av.VideoFrame.from_ndarray(
            frame_bgr,
            format="bgra" if frame_bgr.shape[2] == 4 else "bgr24",
        )
        colorspace = COLORSPACE_MAP.get(int(self.settings.color_space))
        if colorspace is not None:
            # Keep FFmpeg's UNSPECIFIED range as UNSPECIFIED.  Treating every
            # value other than JPEG as MPEG silently changes the sample range
            # for sources that did not declare one.
            configured_range = int(self.settings.color_range)
            if configured_range == int(ColorRange.UNSPECIFIED):
                destination_range = ColorRange.UNSPECIFIED
            elif configured_range == int(ColorRange.JPEG):
                destination_range = ColorRange.JPEG
            else:
                destination_range = ColorRange.MPEG
            av_frame = self._reformatter.reformat(
                av_frame,
                format=self._stream.pix_fmt,
                src_colorspace=colorspace,
                dst_colorspace=colorspace,
                src_color_range=ColorRange.JPEG,
                dst_color_range=destination_range,
                dst_color_trc=int(self.settings.color_transfer),
                dst_color_primaries=int(self.settings.color_primaries),
            )
        for packet in self._stream.encode(av_frame):
            self._container.mux(packet)

        self._frame_count += 1

    def close(self):
        if self._stream is None and self._container is None:
            return
        stream, container = self._stream, self._container
        self._stream = None
        self._container = None
        failure = None
        if stream is not None:
            try:
                for packet in stream.encode():
                    container.mux(packet)
            except Exception as error:
                failure = error
        if container is not None:
            try:
                container.close()
            except Exception as error:
                if failure is None:
                    failure = error
        if failure is not None:
            raise failure
        logger.info("Encoded %s frames -> %s", self._frame_count, self.settings.output_path)
