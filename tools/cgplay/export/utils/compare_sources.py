"""Timestamp media adapters for N01; bounded decode state, no GUI."""
from fractions import Fraction
import itertools

import av
import numpy as np

from ..compare_job import RawFrame
from cgplay.export.utils.color_pipeline import ColorPipeline
from cgplay.export.utils.image_reader import ImageReader


class TimestampVideoSource:
    def __init__(self, spec):
        self.spec = spec
        self.protected_paths = (spec.path,)
        self.stream_index = spec.video_stream_index
        self.container = None

    def __enter__(self):
        try:
            self.container = av.open(self.spec.path)
            self.stream = self.container.streams.video[self.stream_index]
            self.iterator = iter(self.container.decode(self.stream))
            first = next(self.iterator)
            if first.pts is None or first.time_base is None:
                raise ValueError('Video has no presentation timestamp; cannot synchronize A/B')
            self.origin = first.pts * first.time_base
            self.pending = self._convert(first)
            self.current = None
            self.last_request = None
            self.last_decoded_time = self.pending.stamp
            self.last_duration = Fraction(first.duration or 0) * first.time_base
            self.eof = False
            return self
        except BaseException:
            self.__exit__()
            raise

    def _convert(self, frame):
        if frame.pts is None or frame.time_base is None:
            raise ValueError('Missing video PTS')
        fmt = frame.format.name.lower()
        with_alpha = any(name in fmt for name in ('rgba', 'bgra', 'argb', 'abgr', 'yuva', 'gbrap'))
        # PyAV 17.1.0 returns HxWx3/4 RGB(A) float32 for these planar AV formats.
        array = frame.to_ndarray(format='gbrapf32le' if with_alpha else 'gbrpf32le')
        if array.ndim != 3 or array.shape[2] not in (3, 4):
            raise ValueError('PyAV float RGB conversion contract differs in this runtime')
        self.last_duration = Fraction(frame.duration or 0) * frame.time_base
        return RawFrame(Fraction(frame.pts * frame.time_base - self.origin),
                        np.ascontiguousarray(array[..., :3]),
                        np.ascontiguousarray(array[..., 3]) if array.shape[2] == 4 else None)

    def frame_at(self, local_seconds):
        target = Fraction(local_seconds)
        if not 0 <= target < self.spec.clock.duration:
            return None
        if self.last_request is not None and target < self.last_request:
            raise ValueError('Source adapter expects monotonic output time')
        if self.last_request is None and target > 0:
            seek_tick = (self.origin + target) / self.stream.time_base
            self.container.seek(seek_tick.numerator // seek_tick.denominator,
                                backward=True, any_frame=False, stream=self.stream)
            self.iterator = iter(self.container.decode(self.stream))
            self.pending = self.current = None
            self.last_decoded_time = None
        self.last_request = target
        while True:
            if self.pending is None and not self.eof:
                try:
                    raw = self._convert(next(self.iterator))
                except StopIteration:
                    self.eof = True
                else:
                    if self.last_decoded_time is not None and raw.stamp < self.last_decoded_time:
                        raise ValueError('Non-monotonic decoded video PTS')
                    self.last_decoded_time = raw.stamp
                    self.pending = raw
            if self.pending is None or self.pending.stamp > target:
                break
            self.current, self.pending = self.pending, None
        if self.current is None:
            raise ValueError(f'No decoded frame covers requested time {target}')
        if self.eof:
            # No unbounded hold of a stale last frame when metadata overstates duration.
            duration = self.last_duration or 1 / self.spec.clock.fps
            if target >= self.current.stamp + duration:
                raise ValueError('Video ended before the declared comparison time range')
        return self.current

    def __exit__(self, *args):
        if self.container is not None:
            self.container.close()
            self.container = None
        self.current = self.pending = None


class ImageSequenceSource:
    def __init__(self, spec, frame_paths):
        self.spec = spec
        self.frame_paths = dict(frame_paths)  # Full immutable source-frame-number snapshot.
        self.first_number = min(self.frame_paths)
        if self.frame_paths and max(self.frame_paths) - self.first_number + 1 != spec.clock.frame_count:
            raise ValueError('Sequence time range differs from its source-number extent')
        self.protected_paths = tuple(self.frame_paths.values())
        self.last = None
        self.reader = ImageReader()
        # Public bypass keeps raw float data without a hidden scene-linear transform.
        self.bypass = ColorPipeline({'mode': 'bypass'})

    def __enter__(self):
        return self

    def frame_at(self, local_seconds):
        target = Fraction(local_seconds)
        if not 0 <= target < self.spec.clock.duration:
            return None
        position = target * self.spec.clock.fps
        relative = position.numerator // position.denominator
        number = self.first_number + relative
        if number not in self.frame_paths:
            raise ValueError(f'Missing sequence frame {number}; never pack timeline gaps')
        if self.last is None or self.last[0] != number:
            array = self.reader.read_frame(self.frame_paths[number], color_pipeline=self.bypass, output_float=True)
            if array.dtype != np.float32:
                raise ValueError('Raw image reader must return float32 without prior quantization')
            rgb = np.ascontiguousarray(array[..., [2, 1, 0]])
            alpha = array[..., 3] if array.shape[2] == 4 else None
            self.last = number, RawFrame(Fraction(relative, 1) / self.spec.clock.fps, rgb, alpha)
        return self.last[1]

    def __exit__(self, *args):
        self.last = None
