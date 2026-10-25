"""N01: float A/B compositor and exact-time CFR job, for offline compare exports.

Only public OutputTransaction / ColorPipeline.apply_float / FFmpegExporter APIs.
The raw media adapter contract is explicit; no GUI or tlRender objects enter it.
"""
from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass, replace
from fractions import Fraction
import logging
from pathlib import Path
import shutil
from typing import Callable, Protocol

import numpy as np
from PIL import Image

from cgplay.export.utils.output_transaction import OutputTransaction
from cgplay.export.exporters.ffmpeg_exporter import FFmpegExporter


class ExportCancelled(RuntimeError):
    pass


@dataclass(frozen=True)
class Clock:
    fps: Fraction
    timeline_start: Fraction
    frame_count: int

    def __post_init__(self):
        if not isinstance(self.fps, Fraction) or self.fps <= 0:
            raise ValueError('fps must be an exact positive Fraction')
        if not isinstance(self.timeline_start, Fraction) or self.frame_count <= 0:
            raise ValueError('invalid source time range')

    @property
    def duration(self):
        return Fraction(self.frame_count, 1) / self.fps


@dataclass(frozen=True)
class SourceSpec:
    path: str
    clock: Clock
    width: int
    height: int
    pixel_aspect: Fraction = Fraction(1)
    encoding: str = 'display'
    alpha_mode: str = 'opaque'
    rotation_cw: int = 0
    video_stream_index: int = 0
    audio_stream_index: int = 0

    def __post_init__(self):
        if self.width <= 0 or self.height <= 0 or self.pixel_aspect <= 0:
            raise ValueError('invalid source geometry')
        if self.encoding not in ('display', 'scene-linear'):
            raise ValueError('source encoding must be captured explicitly')
        if self.alpha_mode not in ('opaque', 'straight', 'premultiplied'):
            raise ValueError('invalid alpha mode')
        if self.rotation_cw not in (0, 90, 180, 270):
            raise ValueError('unsupported orientation')
        if min(self.video_stream_index, self.audio_stream_index) < 0:
            raise ValueError('stream indices must be non-negative')

    @property
    def aspect(self):
        value = Fraction(self.width, self.height) * self.pixel_aspect
        return 1 / value if self.rotation_cw in (90, 270) else value


@dataclass(frozen=True)
class RawFrame:
    stamp: Fraction
    rgb: np.ndarray
    alpha: np.ndarray | None = None


class FloatSource(Protocol):
    spec: SourceSpec
    protected_paths: tuple[str, ...]
    def __enter__(self): ...
    def __exit__(self, *args): ...
    def frame_at(self, local_seconds: Fraction) -> RawFrame | None: ...


class FloatColorPipeline(Protocol):
    def apply_float(self, rgb, *, source_encoding='display'): ...


@dataclass(frozen=True)
class CompareOptions:
    mode: str = 'horizontal'
    wipe_x: float = .5
    wipe_y: float = .5
    wipe_rotation: float = 0.
    overlay_a: float = .5
    compare_time: str = 'relative'

    def __post_init__(self):
        if self.mode not in ('a', 'b', 'horizontal', 'vertical', 'tile', 'wipe', 'overlay', 'difference'):
            raise ValueError('unknown compare mode')
        if self.compare_time not in ('relative', 'absolute'):
            raise ValueError('unknown compare time mode')
        values = (self.wipe_x, self.wipe_y, self.wipe_rotation, self.overlay_a)
        if not all(np.isfinite(x) for x in values):
            raise ValueError('non-finite compare parameter')
        if not 0 <= self.overlay_a <= 1 or not 0 <= self.wipe_x <= 1 or not 0 <= self.wipe_y <= 1:
            raise ValueError('compare parameter outside slider range')


@dataclass(frozen=True)
class Box:
    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class Layout:
    width: int
    height: int
    a: Box
    b: Box


def native_layout(a: SourceSpec, b: SourceSpec, mode: str) -> Layout:
    """Opaque-source geometry corresponding to local tl::getBoxes (two inputs).

    Integration should serialize boxes returned by tl::getBoxes directly,
    especially when the viewer uses a DisplayOptions aspect override.
    """
    if a.rotation_cw or b.rotation_cw:
        raise ValueError('oriented sources require a captured tl::getBoxes layout')
    h = a.height
    w = max(1, int(h * a.aspect))
    if mode == 'tile':
        # Local tlRender: two inputs -> one column, two rows; choose max-area size.
        chosen = a if a.width * a.height >= b.width * b.height else b
        h = chosen.height if chosen.rotation_cw in (0, 180) else chosen.width
        # The source-native reference uses max PAR; this adapter assumes rotation=0.
        if a.rotation_cw or b.rotation_cw:
            raise ValueError('rotated Tile requires a captured tl::getBoxes layout')
        w = max(1, int(chosen.width * max(Fraction(1), a.pixel_aspect, b.pixel_aspect)))
    if mode == 'horizontal':
        return Layout(w * 2, h, Box(0, 0, w, h), Box(w, 0, w, h))
    if mode in ('vertical', 'tile'):
        return Layout(w, h * 2, Box(0, 0, w, h), Box(0, h, w, h))
    return Layout(w, h, Box(0, 0, w, h), Box(0, 0, w, h))


def compare_b_time(a: Clock, b: Clock, a_relative_frame: int, mode: str) -> Fraction:
    """Exact seconds relative to B start; follows tl::getCompareTime's floor.

    The A trim is not a new source origin: A frame 24 at 24fps maps to B 1sec.
    """
    elapsed = Fraction(a_relative_frame, 1) / a.fps
    value = elapsed if mode == 'relative' else a.timeline_start + elapsed
    if mode not in ('relative', 'absolute'):
        raise ValueError('unknown comparison clock')
    frames = value * b.fps
    quantized = Fraction(frames.numerator // frames.denominator, 1) / b.fps
    return quantized if mode == 'relative' else quantized - b.timeline_start


def audio_clip_start(a: Clock, b: Clock, start_frame: int, choice: str, mode: str) -> Fraction:
    """Continuous source time; do not staircase audio with B video frame floor."""
    elapsed = Fraction(start_frame, 1) / a.fps
    if choice == 'a':
        return elapsed
    if choice == 'b':
        return elapsed if mode == 'relative' else a.timeline_start + elapsed - b.timeline_start
    raise ValueError('audio choice must be a or b')


def _float_rgb(frame: RawFrame, spec: SourceSpec, pipeline: FloatColorPipeline):
    rgb = np.asarray(frame.rgb, dtype=np.float32)
    if rgb.shape != (spec.height, spec.width, 3) or not np.all(np.isfinite(rgb)):
        raise ValueError('raw RGB shape changed or non-finite values found')
    alpha = np.ones(rgb.shape[:2], dtype=np.float32)
    if spec.alpha_mode != 'opaque':
        if frame.alpha is None or frame.alpha.shape != rgb.shape[:2]:
            raise ValueError('declared alpha channel is missing')
        alpha = np.asarray(frame.alpha, dtype=np.float32)
        if not np.all(np.isfinite(alpha)) or np.any((alpha < 0) | (alpha > 1)):
            raise ValueError('opacity must be finite in [0,1]')
        if spec.alpha_mode == 'premultiplied':
            rgb = np.divide(rgb, alpha[..., None], out=np.zeros_like(rgb), where=alpha[..., None] > 0)
    work = pipeline.apply_float(rgb, source_encoding=spec.encoding)
    if work.shape != rgb.shape or work.dtype != np.float32 or not np.all(np.isfinite(work)):
        raise ValueError('float color pipeline violated its contract')
    rgba = np.dstack((work * alpha[..., None], alpha))
    return np.rot90(rgba, -(spec.rotation_cw // 90)) if spec.rotation_cw else rgba


def _fit(rgba: np.ndarray, aspect: Fraction, box: Box):
    if min(box.width, box.height) <= 0:
        raise ValueError('empty layout box')
    if Fraction(box.width, box.height) > aspect:
        height = box.height
        width = max(1, min(box.width, int(height * aspect)))
    else:
        width = box.width
        height = max(1, min(box.height, int(width / aspect)))
    # Float32 per-channel resize; no Pillow RGB uint8 intermediate.
    result = np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(rgba[..., c])).resize(
        (width, height), Image.Resampling.BILINEAR), dtype=np.float32) for c in range(4)], axis=-1)
    out = np.zeros((box.height, box.width, 4), dtype=np.float32)
    x, y = (box.width - width) // 2, (box.height - height) // 2
    out[y:y + height, x:x + width] = result
    return out


def compose_float(a_rgba, b_rgba, a_spec, b_spec, options, layout):
    """Premultiplied float layers; final opaque RGB uses a solid black background."""
    a = np.zeros((layout.height, layout.width, 4), dtype=np.float32)
    b = np.zeros_like(a)
    for target, source, spec, box in ((a, a_rgba, a_spec, layout.a), (b, b_rgba, b_spec, layout.b)):
        if min(box.x, box.y) < 0 or box.x + box.width > layout.width or box.y + box.height > layout.height:
            raise ValueError('layout box outside canvas')
        target[box.y:box.y + box.height, box.x:box.x + box.width] = _fit(source, spec.aspect, box)
    if options.mode == 'a':
        return a[..., :3]
    if options.mode == 'b':
        return b[..., :3]
    if options.mode in ('horizontal', 'vertical', 'tile'):
        return (a + b)[..., :3]
    if options.mode == 'overlay':
        amount = np.float32(options.overlay_a)
        return a[..., :3] * amount + b[..., :3] * (1 - a[..., 3:] * amount)
    if options.mode == 'difference':
        return np.abs(a[..., :3] - b[..., :3])
    # In the local renderer rotation=0 puts A left; positive 90deg puts A top.
    y, x = np.ogrid[:layout.height, :layout.width]
    theta = np.deg2rad(options.wipe_rotation)
    center_x = layout.a.x + layout.a.width * options.wipe_x
    center_y = layout.a.y + layout.a.height * options.wipe_y
    side = (x + .5 - center_x) * np.cos(theta) + (y + .5 - center_y) * np.sin(theta)
    return np.where((side <= 0)[..., None], a[..., :3], b[..., :3])


def quantize_bgr8(rgb):
    """The sole RGB quantization point after all color/resize/compare operations."""
    if not np.all(np.isfinite(rgb)):
        raise ValueError('compositor produced non-finite RGB')
    return np.ascontiguousarray(np.rint(np.clip(rgb[..., ::-1], 0., 1.) * 255.).astype(np.uint8))


def quantize_bgr16(rgb):
    """Final full-range RGB precision for 10/12-bit mezzanine encoders."""
    if not np.all(np.isfinite(rgb)):
        raise ValueError('compositor produced non-finite RGB')
    return np.ascontiguousarray(np.rint(np.clip(rgb[..., ::-1], 0., 1.) * 65535.).astype(np.uint16))


def _check_cancel(cancel):
    if cancel and cancel():
        raise ExportCancelled('A/B export cancelled')


def export_compare(a_source: FloatSource, b_source: FloatSource, a_color: FloatColorPipeline,
                   b_color: FloatColorPipeline, options: CompareOptions, settings,
                   start_frame=0, end_frame=None, *, layout=None, audio_choice='none',
                   audio_offset=Fraction(0), staging_dir=None, cancel=None, progress=None,
                   protected_paths=()):
    """Reusable job draft; writes final target only through the existing transaction.

    Audio helpers are imported lazily; no mutation of export_job/engine is needed.
    """
    a_clock, b_clock = a_source.spec.clock, b_source.spec.clock
    end_frame = a_clock.frame_count - 1 if end_frame is None else end_frame
    if not 0 <= start_frame <= end_frame < a_clock.frame_count:
        raise ValueError('A export range must be inclusive relative frame positions')
    if audio_choice not in ('none', 'a', 'b'):
        raise ValueError('audio choice must be a, b, or none')
    layout = layout or native_layout(a_source.spec, b_source.spec, options.mode)
    if layout.width <= 0 or layout.height <= 0:
        raise ValueError('empty export canvas')
    # Pad at right/bottom for subsampled encoders without stretching either image.
    width, height = layout.width + layout.width % 2, layout.height + layout.height % 2
    count = end_frame - start_frame + 1
    high_depth = settings.normalized_codec() in ('prores_422', 'prores_hq', 'prores_4444', 'dnxhr')
    duration = Fraction(count, 1) / a_clock.fps
    protected = tuple(a_source.protected_paths) + tuple(b_source.protected_paths) + tuple(protected_paths)
    _check_cancel(cancel)
    with OutputTransaction(settings.output_path, protected, staging_dir) as transaction:
        if audio_choice != 'none':
            required = -(-(duration * 48000).numerator // (duration * 48000).denominator) * 8
            if shutil.disk_usage(transaction.directory).free < required + 16 * 1024 * 1024:
                raise ValueError('Output volume lacks space for the timestamp-aligned temporary audio')
        silent_path = transaction.staged_path if audio_choice == 'none' else transaction.directory / ('silent' + transaction.staged_path.suffix)
        writer = FFmpegExporter(replace(settings, output_path=str(silent_path), width=width,
                                        height=height, fps=a_clock.fps))
        with ExitStack() as resources:
            resources.enter_context(a_source)
            resources.enter_context(b_source)
            try:
                writer.open()
                cached_a = cached_b = None
                for index, source_frame in enumerate(range(start_frame, end_frame + 1)):
                    _check_cancel(cancel)
                    a = a_source.frame_at(Fraction(source_frame, 1) / a_clock.fps)
                    if a is None:
                        raise ValueError(f'A source has no frame at relative position {source_frame}')
                    bt = compare_b_time(a_clock, b_clock, source_frame, options.compare_time)
                    b_in_range = 0 <= bt < b_clock.duration
                    b = b_source.frame_at(bt) if b_in_range else None
                    if b_in_range and b is None:
                        raise ValueError(f'B failed to decode at {bt}; this is not an outside-range black frame')
                    if cached_a is None or cached_a[0] != a.stamp:
                        cached_a = a.stamp, _float_rgb(a, a_source.spec, a_color)
                    ar = cached_a[1]
                    if b is not None and (cached_b is None or cached_b[0] != b.stamp):
                        cached_b = b.stamp, _float_rgb(b, b_source.spec, b_color)
                    br = cached_b[1] if b is not None else np.zeros(
                        (b_source.spec.height, b_source.spec.width, 4), dtype=np.float32)
                    if b is None and b_source.spec.rotation_cw:
                        br = np.rot90(br, -(b_source.spec.rotation_cw // 90))
                    rgb = compose_float(ar, br, a_source.spec, b_source.spec, options, layout)
                    frame = quantize_bgr16(rgb) if high_depth else quantize_bgr8(rgb)
                    if (width, height) != (layout.width, layout.height):
                        padded = np.zeros((height, width, 3), dtype=frame.dtype)
                        padded[:layout.height, :layout.width] = frame
                        frame = padded
                    writer.write_frame(frame)
                    if progress:
                        progress(min(90, (index + 1) * 90 // count), f'Compositing {index + 1}/{count}')
            except BaseException:
                try:
                    writer.close()
                except Exception:
                    logging.exception('Secondary encoder-close error after A/B export failure')
                raise
            writer.close()  # Flush/container-close errors propagate before commit.
        if audio_choice != 'none':
            from .utils.compare_audio import extract_audio_float, mux_audio
            chosen = a_source if audio_choice == 'a' else b_source
            # Local tlRender PlayerAudio uses source_time = playhead - audioOffset.
            # Positive offset delays the selected sound relative to the picture.
            audio_start = audio_clip_start(a_clock, b_clock, start_frame, audio_choice, options.compare_time) - Fraction(audio_offset)
            pcm = transaction.directory / 'selected-audio.f32le'
            extract_audio_float(chosen.spec.path, pcm, audio_start, duration,
                                media_duration=chosen.spec.clock.duration,
                                video_stream_index=chosen.spec.video_stream_index,
                                audio_stream_index=chosen.spec.audio_stream_index, cancel=cancel)
            mux_audio(silent_path, pcm, transaction.staged_path, cancel=cancel)
        _check_cancel(cancel)
        transaction.commit()
    if progress:
        try:
            progress(100, 'Done')
        except Exception:
            logging.exception('A/B output committed; completion observer failed')
    return settings.output_path
