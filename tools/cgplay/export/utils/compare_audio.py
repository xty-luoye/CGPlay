"""N01: timestamp-aligned float audio clip and lossless-video remux."""
from fractions import Fraction
import itertools
from pathlib import Path

import av
import numpy as np

from ..compare_job import _check_cancel


RATE = 48000
CHANNELS = 2


def _nearest(value):
    value = Fraction(value)
    if value < 0:
        return -_nearest(-value)
    return (value.numerator * 2 + value.denominator) // (2 * value.denominator)


def extract_audio_float(path, pcm_path, start, duration, *, media_duration=None,
                        audio_stream_index=0, video_stream_index=0, cancel=None):
    """Write exactly ceil(duration*48000) stereo f32 samples, with explicit silence.

    Source audio PTS remains relative to the first visible video PTS, not the
    first audio packet. Missing tracks fail. Resample and decoder tails flush.
    """
    start, duration = Fraction(start), Fraction(duration)
    if duration <= 0:
        raise ValueError('Audio clip duration must be positive')
    _check_cancel(cancel)
    with av.open(path) as probe:
        if not 0 <= video_stream_index < len(probe.streams.video):
            raise ValueError('Selected audio source has no video reference clock')
        first_video = next(probe.decode(probe.streams.video[video_stream_index]), None)
        if first_video is None or first_video.pts is None or first_video.time_base is None:
            raise ValueError('Audio source video origin has no timestamp')
        video_origin = first_video.pts * first_video.time_base
    sample_count = -(-(duration * RATE).numerator // (duration * RATE).denominator)
    cursor = 0
    silence = np.zeros((8192, CHANNELS), dtype='<f4')
    end = start + duration
    resampler = av.AudioResampler(format='fltp', layout='stereo', rate=RATE)
    with av.open(path) as source, Path(pcm_path).open('xb') as output:
        if not 0 <= audio_stream_index < len(source.streams.audio):
            raise ValueError('Selected A/B source has no requested audio track; choose none explicitly')
        stream = source.streams.audio[audio_stream_index]
        iterator = iter(source.decode(stream))
        first_audio = next(iterator, None)
        if first_audio is None or first_audio.pts is None or first_audio.time_base is None:
            raise ValueError('Selected audio stream has no timestamped decoded samples')
        absolute_start = video_origin + max(start, Fraction(0))
        if absolute_start > first_audio.pts * first_audio.time_base:
            seek_tick = absolute_start / stream.time_base
            source.seek(seek_tick.numerator // seek_tick.denominator,
                        stream=stream, backward=True, any_frame=False)
            frames = iter(source.decode(stream))
        else:
            frames = itertools.chain([first_audio], iterator)

        def pad(until):
            nonlocal cursor
            until = min(until, sample_count)
            while cursor < until:
                _check_cancel(cancel)
                amount = min(8192, until - cursor)
                output.write(silence[:amount].tobytes())
                cursor += amount

        def write(resampled):
            nonlocal cursor
            if resampled.pts is None or resampled.time_base is None:
                raise ValueError('Resampled audio has no PTS')
            seconds = resampled.pts * resampled.time_base - video_origin
            begin = _nearest((seconds - start) * RATE)
            array = np.asarray(resampled.to_ndarray(), dtype=np.float32).T
            if array.ndim != 2 or array.shape[1] != CHANNELS or not np.all(np.isfinite(array)):
                raise ValueError('Resampler did not return finite stereo float audio')
            low, high = max(0, begin, cursor), min(sample_count, begin + len(array))
            low = max(low, _nearest(-start * RATE))
            if media_duration is not None:
                high = min(high, _nearest((Fraction(media_duration) - start) * RATE))
            if high <= low:
                return
            pad(low)
            output.write(np.ascontiguousarray(array[low - begin:high - begin], dtype='<f4').tobytes())
            cursor = high

        previous_pts = None
        for frame in frames:
            _check_cancel(cancel)
            if frame.pts is None or frame.time_base is None:
                raise ValueError('Decoded audio has no PTS')
            pts = frame.pts * frame.time_base
            if previous_pts is not None and pts < previous_pts:
                raise ValueError('Non-monotonic audio PTS')
            previous_pts = pts
            if pts - video_origin >= end:
                break
            for result in resampler.resample(frame):
                write(result)
        for result in resampler.resample(None):
            write(result)
        pad(sample_count)
    return {'samples': sample_count, 'rate': RATE, 'channels': CHANNELS,
            'alignmentToleranceSamples': 1, 'videoOrigin': str(video_origin)}


def mux_audio(video_path, pcm_path, output_path, *, cancel=None):
    """Copy video and encode AAC in one process with two-packet lookahead.

    Each generator flushes independently. DTS is compared in exact seconds,
    so the muxer never receives a complete long video ahead of all its audio.
    """
    if Path(pcm_path).stat().st_size % (CHANNELS * 4):
        raise ValueError('Temporary float audio has an incomplete stereo sample')
    with av.open(str(video_path)) as video, Path(pcm_path).open('rb') as pcm:
        input_video = video.streams.video[0]
        output = av.open(str(output_path), mode='w', options={'movflags': '+faststart'})
        try:
            output_video = output.add_stream_from_template(input_video)
            output_video.metadata.update(input_video.metadata)
            output_audio = output.add_stream('aac', rate=RATE)
            output_audio.layout = 'stereo'
            output_audio.bit_rate = 192000
            output_audio.time_base = Fraction(1, RATE)
            output_audio.codec_context.time_base = Fraction(1, RATE)

            def video_packets():
                for packet in video.demux(input_video):
                    _check_cancel(cancel)
                    if packet.dts is not None:
                        yield packet

            def audio_packets():
                cursor = 0
                while True:
                    _check_cancel(cancel)
                    data = pcm.read(1024 * CHANNELS * 4)
                    if not data:
                        break
                    array = np.frombuffer(data, dtype='<f4').reshape(-1, CHANNELS)
                    frame = av.AudioFrame.from_ndarray(np.ascontiguousarray(array.T),
                                                       format='fltp', layout='stereo')
                    frame.sample_rate = RATE
                    frame.time_base = Fraction(1, RATE)
                    frame.pts = cursor
                    cursor += len(array)
                    yield from output_audio.encode(frame)
                _check_cancel(cancel)
                yield from output_audio.encode(None)

            def seconds(packet):
                if packet.dts is None or packet.time_base is None:
                    raise ValueError('Encoded packet has no DTS/time_base')
                return packet.dts * packet.time_base

            video_iterator, audio_iterator = iter(video_packets()), iter(audio_packets())
            vp, ap = next(video_iterator, None), next(audio_iterator, None)
            while vp is not None or ap is not None:
                _check_cancel(cancel)
                if ap is None or (vp is not None and seconds(vp) <= seconds(ap)):
                    vp.stream = output_video
                    output.mux(vp)
                    vp = next(video_iterator, None)
                else:
                    ap.stream = output_audio
                    output.mux(ap)
                    ap = next(audio_iterator, None)
        except BaseException:
            try:
                output.close()
            except Exception:
                pass
            raise
        output.close()  # Propagate trailer/faststart errors before transaction commit.
