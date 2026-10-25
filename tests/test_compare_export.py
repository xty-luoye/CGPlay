"""Deterministic, background-only tests for N01 backend; no player or real media."""
from pathlib import Path
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'tests/artifacts/compare_export_20261025'
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(BASE))

from fractions import Fraction as F
import json
import os
import subprocess
import unittest

import av
import numpy as np

from cgplay.export.compare_job import (Box, Clock, CompareOptions, ExportCancelled, Layout,
                         RawFrame, SourceSpec, _float_rgb, audio_clip_start,
                         compare_b_time, compose_float, export_compare, native_layout)
from cgplay.export.utils.compare_audio import extract_audio_float
from cgplay.export.utils.compare_sources import ImageSequenceSource, TimestampVideoSource
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.exporters.ffmpeg_exporter import FFmpegExporter
from cgplay.export.utils.color_pipeline import ColorPipeline

OUTPUT = BASE / 'outputs'
FFMPEG = ROOT / 'build_win_full/bin/Release/ffmpeg.exe'


class IdentityColor:
    def apply_float(self, rgb, *, source_encoding='display'):
        return np.array(rgb, dtype=np.float32, copy=True)


class SyntheticSource:
    def __init__(self, clock, width=64, height=48, base=(.1, .2, .3), fail_at=None):
        self.spec = SourceSpec(str(OUTPUT / 'synthetic.input'), clock, width, height)
        self.protected_paths = ()
        self.base = base
        self.fail_at = fail_at
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.closed = True

    def frame_at(self, seconds):
        index = int(seconds * self.spec.clock.fps)
        if index == self.fail_at:
            raise RuntimeError('injected decoder failure')
        if not 0 <= seconds < self.spec.clock.duration:
            return None
        rgb = np.empty((self.spec.height, self.spec.width, 3), dtype=np.float32)
        rgb[:] = self.base
        rgb[..., 0] += (index % 10) * .02
        return RawFrame(F(index, 1) / self.spec.clock.fps, rgb)


class CompareExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        cls.fixture = OUTPUT / 'offset-audio-source.mov'
        if cls.fixture.exists():
            cls.fixture.unlink()
        # Actual file has video start at +2sec, audio starts 250ms later.
        command = [str(FFMPEG), '-hide_banner', '-v', 'error', '-nostdin', '-n',
            '-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=24:duration=1',
            '-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=48000:duration=0.75',
            '-filter_complex', '[0:v]setpts=PTS+2/TB[v];[1:a]asetpts=PTS+2.25/TB[a]',
            '-map', '[v]', '-map', '[a]', '-copyts', '-fps_mode', 'passthrough',
            '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '1', '-bf', '0',
            '-c:a', 'pcm_f32le', str(cls.fixture)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout.decode(errors='replace'))
        cls.details = {'runtime': {'python': sys.version.split()[0], 'pyav': av.__version__},
                       'fixture': str(cls.fixture)}

    def source_pair(self, fps=F(24), count=72):
        a = SyntheticSource(Clock(fps, F(101) / fps, count), base=(.2, .1, .05))
        b = SyntheticSource(Clock(F(30), F(1001, 30), 90), base=(.05, .1, .4))
        return a, b

    def test_relative_absolute_and_hour_boundary(self):
        a, b = self.source_pair()
        self.assertEqual(F(0), compare_b_time(a.spec.clock, b.spec.clock, 0, 'relative'))
        self.assertEqual(F(1, 30), compare_b_time(a.spec.clock, b.spec.clock, 1, 'relative'))
        self.assertEqual(F(1), compare_b_time(a.spec.clock, b.spec.clock, 24, 'relative'))
        self.assertEqual(F(108001, 30), compare_b_time(a.spec.clock, b.spec.clock, 86401, 'relative'))
        self.assertEqual(F(126 - 1001, 30), compare_b_time(a.spec.clock, b.spec.clock, 0, 'absolute'))
        self.assertEqual(F(1, 24), audio_clip_start(a.spec.clock, b.spec.clock, 1, 'b', 'relative'))
        fractional = Clock(F(30000, 1001), F(0), 200000)
        self.assertEqual(F(108000, 30), compare_b_time(fractional, b.spec.clock, 107893, 'relative'))

    def test_geometry_wipe_overlay_difference_float(self):
        spec = SourceSpec('a', Clock(F(24), F(0), 1), 8, 8)
        a = np.empty((8, 8, 4), dtype=np.float32); a[:] = (1, 0, 0, 1)
        b = np.empty_like(a); b[:] = (0, 0, 1, 1)
        lay = native_layout(spec, spec, 'wipe')
        result = compose_float(a, b, spec, spec, CompareOptions(mode='wipe'), lay)
        np.testing.assert_array_equal(result[:, :4, 0], np.ones((8, 4)))
        np.testing.assert_array_equal(result[:, 4:, 2], np.ones((8, 4)))
        top = compose_float(a, b, spec, spec,
                            CompareOptions(mode='wipe', wipe_y=.25, wipe_rotation=90), lay)
        np.testing.assert_array_equal(top[:2, :, 0], np.ones((2, 8)))
        np.testing.assert_array_equal(top[2:, :, 2], np.ones((6, 8)))
        mix = compose_float(a, b, spec, spec, CompareOptions(mode='overlay', overlay_a=.25), lay)
        np.testing.assert_allclose(mix[3, 3], (.25, 0, .75))
        a[..., :3] = 2.; b[..., :3] = 1.5
        diff = compose_float(a, b, spec, spec, CompareOptions(mode='difference'), lay)
        np.testing.assert_allclose(diff, .5)  # Earlier clip of both sources would erase this.
        b[:, :, :3] = a[:, :, :3]
        np.testing.assert_array_equal(compose_float(a, b, spec, spec, CompareOptions(mode='difference'), lay),
                                      np.zeros((8, 8, 3), dtype=np.float32))

    def test_native_tile_and_letterbox(self):
        a, _ = self.source_pair()
        b = SourceSpec('b', a.spec.clock, 80, 32)
        lay = native_layout(a.spec, b, 'horizontal')
        self.assertEqual((128, 48), (lay.width, lay.height))
        tile = native_layout(a.spec, b, 'tile')
        self.assertEqual((64, 96), (tile.width, tile.height))  # A has larger pixel area.
        ar = np.ones((48, 64, 4), dtype=np.float32)
        br = np.ones((32, 80, 4), dtype=np.float32)
        image = compose_float(ar, br, a.spec, b, CompareOptions(mode='horizontal'), lay)
        self.assertTrue(np.all(image[0, 64:] == 0))
        self.assertTrue(np.all(image[24, 64:] == 1))

    def test_current_public_float_pipeline(self):
        work = ColorPipeline({'mode': 'bypass'}).apply_float(np.array([[[2., -.25, .5]]], dtype=np.float32))
        np.testing.assert_allclose(work, [[[2., -.25, .5]]])

    def test_real_fractional_cfr_and_last_frame(self):
        a, b = self.source_pair(F(30000, 1001), 30)
        output = OUTPUT / 'fractional-compare.mp4'
        settings = ExportSettings(output_path=str(output), bitrate='1M')
        export_compare(a, b, IdentityColor(), IdentityColor(), CompareOptions(), settings)
        with av.open(str(output)) as container:
            stream = container.streams.video[0]
            frames = list(container.decode(stream))
            self.assertEqual(F(30000, 1001), stream.average_rate)
            self.assertEqual(30, len(frames))
            times = [frame.pts * frame.time_base for frame in frames]
            self.assertEqual([F(i * 1001, 30000) for i in range(30)], times)
            self.details['fractionalOutput'] = {'frames': len(frames), 'fps': str(stream.average_rate),
                'firstPtsSeconds': str(times[0]), 'lastPtsSeconds': str(times[-1])}
        with av.open(str(output)) as container:
            dts = [p.dts for p in container.demux(video=0) if p.dts is not None]
            self.assertEqual(sorted(dts), dts)

    def test_trim_does_not_restart_b_and_atomic_failure_cancel(self):
        a, b = self.source_pair()
        seen = []
        original = b.frame_at
        def observe(t):
            seen.append(t)
            return original(t)
        b.frame_at = observe
        output = OUTPUT / 'trim-compare.mp4'
        export_compare(a, b, IdentityColor(), IdentityColor(), CompareOptions(),
                       ExportSettings(output_path=str(output), bitrate='1M'), 24, 35)
        self.assertEqual(F(1), seen[0])
        with av.open(str(output)) as container:
            self.assertEqual(12, len(list(container.decode(video=0))))
        sentinel = OUTPUT / 'existing-target.mp4'
        sentinel.write_bytes(b'previous verified output')
        a, b = self.source_pair()
        a.fail_at = 2
        with self.assertRaisesRegex(RuntimeError, 'injected'):
            export_compare(a, b, IdentityColor(), IdentityColor(), CompareOptions(),
                           ExportSettings(output_path=str(sentinel)))
        self.assertEqual(b'previous verified output', sentinel.read_bytes())
        self.assertTrue(a.closed and b.closed)
        a, b = self.source_pair()
        cancelled = [False]
        with self.assertRaises(ExportCancelled):
            export_compare(a, b, IdentityColor(), IdentityColor(), CompareOptions(),
                ExportSettings(output_path=str(sentinel)), cancel=lambda: cancelled[0],
                progress=lambda *args: cancelled.__setitem__(0, True))
        self.assertEqual(b'previous verified output', sentinel.read_bytes())

    def test_real_audio_origin_padding_tail_and_missing_track(self):
        pcm = OUTPUT / 'offset-audio.f32le'
        pcm.unlink(missing_ok=True)
        info = extract_audio_float(str(self.fixture), pcm, F(0), F(1), media_duration=F(1))
        samples = np.fromfile(pcm, dtype='<f4').reshape(-1, 2)
        self.assertEqual(48000, len(samples))
        self.assertLess(float(np.abs(samples[:11500]).max()), 1e-6)
        self.assertGreater(float(np.abs(samples[-1000:]).max()), .03)
        self.assertEqual('2', info['videoOrigin'])
        self.details['audioClip'] = info
        video_only = OUTPUT / 'fractional-compare.mp4'
        if not video_only.exists():
            self.test_real_fractional_cfr_and_last_frame()
        (OUTPUT / 'missing-track.f32le').unlink(missing_ok=True)
        with self.assertRaisesRegex(ValueError, 'no requested audio track'):
            extract_audio_float(str(video_only), OUTPUT / 'missing-track.f32le', F(0), F(1))

    def test_audio_trim_and_positive_delay(self):
        trimmed = OUTPUT / 'trimmed-audio.f32le'
        trimmed.unlink(missing_ok=True)
        extract_audio_float(str(self.fixture), trimmed, F(1, 2), F(1, 2), media_duration=F(1))
        samples = np.fromfile(trimmed, dtype='<f4').reshape(-1, 2)
        self.assertEqual(24000, len(samples))
        self.assertGreater(float(np.abs(samples[:1000]).max()), .03)
        self.assertGreater(float(np.abs(samples[-1000:]).max()), .03)
        delayed = OUTPUT / 'delayed-audio.f32le'
        delayed.unlink(missing_ok=True)
        extract_audio_float(str(self.fixture), delayed, -F(1, 8), F(1), media_duration=F(1))
        samples = np.fromfile(delayed, dtype='<f4').reshape(-1, 2)
        self.assertLess(float(np.abs(samples[:17500]).max()), 1e-6)
        self.assertGreater(float(np.abs(samples[18500:19500]).max()), .03)

    def test_raw_exr_float_and_sequence_gap(self):
        import OpenImageIO as oiio
        paths = {}
        for number in (1001, 1003):
            path = OUTPUT / f'raw.{number}.exr'
            data = np.empty((4, 6, 4), dtype=np.float32)
            data[:] = (2.5, -.125, .25, 3.)
            writer = oiio.ImageOutput.create(str(path))
            if not writer.open(str(path), oiio.ImageSpec(6, 4, 4, oiio.FLOAT)) or not writer.write_image(data):
                raise RuntimeError(writer.geterror())
            writer.close()
            paths[number] = str(path)
        spec = SourceSpec(paths[1001], Clock(F(24), F(1001, 24), 3), 6, 4,
                          encoding='scene-linear', alpha_mode='opaque')
        with ImageSequenceSource(spec, paths) as source:
            first = source.frame_at(F(0))
            np.testing.assert_allclose(first.rgb[0, 0], (2.5, -.125, .25))
            np.testing.assert_allclose(_float_rgb(first, spec, IdentityColor())[0, 0], (2.5, -.125, .25, 1.))
            with self.assertRaisesRegex(ValueError, 'Missing sequence frame 1002'):
                source.frame_at(F(1, 24))
            self.assertEqual(F(2, 24), source.frame_at(F(2, 24)).stamp)

    def test_real_vfr_sampling_uses_pts(self):
        path = OUTPUT / 'vfr.mov'
        path.unlink(missing_ok=True)
        command = [str(FFMPEG), '-hide_banner', '-v', 'error', '-nostdin', '-n',
            '-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=10:duration=1',
            '-vf', 'select=eq(n\\,0)+eq(n\\,1)+eq(n\\,4)+eq(n\\,9)',
            '-fps_mode', 'passthrough', '-c:v', 'libx264', '-g', '1', '-bf', '0', str(path)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout.decode(errors='replace'))
        spec = SourceSpec(str(path), Clock(F(10), F(0), 10), 64, 48)
        with TimestampVideoSource(spec) as source:
            self.assertEqual(F(1, 10), source.frame_at(F(3, 10)).stamp)
            self.assertEqual(F(4, 10), source.frame_at(F(7, 10)).stamp)
            self.assertEqual(F(9, 10), source.frame_at(F(19, 20)).stamp)

    def test_real_video_adapter_and_audio_mux_flush(self):
        spec = SourceSpec(str(self.fixture), Clock(F(24), F(101, 24), 24), 64, 48)
        output = OUTPUT / 'with-audio.mp4'
        export_compare(TimestampVideoSource(spec), TimestampVideoSource(spec), IdentityColor(), IdentityColor(),
            CompareOptions(mode='wipe', wipe_rotation=45), ExportSettings(output_path=str(output), bitrate='2M'),
            audio_choice='a')
        with av.open(str(output)) as container:
            stream = container.streams.video[0]
            self.assertEqual(24, len(list(container.decode(stream))))
        with av.open(str(output)) as container:
            audio = container.streams.audio[0]
            self.assertLessEqual(abs(audio.duration * audio.time_base - 1), F(1, 48000))
            chunks = [f.to_ndarray() for f in container.decode(audio)]
            decoded = np.concatenate(chunks, axis=1)
            self.assertLess(float(np.abs(decoded[:, :9500]).max()), .002)
            self.assertGreater(float(np.abs(decoded[:, 47000:48000]).max()), .02)
            self.details['muxAudio'] = {'duration': str(audio.duration * audio.time_base),
                'decodedSamplesIncludingCodecPadding': decoded.shape[1], 'videoFrames': 24}

    def test_json_cli_uses_public_backend_and_protects_job(self):
        source = {'kind': 'video', 'path': str(self.fixture), 'width': 64, 'height': 48,
                  'clock': {'fps': {'num': 24, 'den': 1}, 'timelineStartFrames': '101', 'frameCount': 24},
                  'encoding': 'display', 'alphaMode': 'opaque',
                  'color': {'mode': 'bypass', 'outputEncoding': 'srgb'}}
        output = OUTPUT / 'cli-compare.mp4'
        job = {'schemaVersion': 1, 'sources': {'a': source, 'b': source},
               'range': {'start': 6, 'end': 17},
               'compare': {'mode': 'overlay', 'time': 'relative', 'overlay': .25,
                           'wipeCenter': [.25, .75], 'wipeRotation': 45},
               'audio': {'source': 'b', 'offsetSeconds': '0'},
               'output': {'path': str(output), 'codec': 'h264', 'encoding': 'srgb', 'bitrate': '1M'},
               'runtime': {'ffmpegPath': str(FFMPEG)}}
        path = OUTPUT / 'compare-job.json'
        path.write_text(json.dumps(job), encoding='utf-8')
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'tools'), PYTHONDONTWRITEBYTECODE='1')
        command = [sys.executable, '-B', '-m', 'cgplay.export.compare_engine', '--job', str(path),
                   '--staging-dir', str(OUTPUT)]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertEqual(0, result.returncode, result.stdout.decode(errors='replace'))
        self.assertIn(b'DONE', result.stdout)
        with av.open(str(output)) as container:
            self.assertEqual(12, len(list(container.decode(video=0))))
            self.assertEqual(1, len(container.streams.audio))
        job['output']['path'] = str(path)
        path.write_text(json.dumps(job), encoding='utf-8')
        original = path.read_bytes()
        failed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertNotEqual(0, failed.returncode)
        self.assertEqual(original, path.read_bytes())
        logical_source = OUTPUT / 'original-before-proxy.mp4'
        logical_source.write_bytes(b'original source must survive a cached-player export')
        job['sources']['a'] = dict(job['sources']['a'], originalPath=str(logical_source))
        job['output']['path'] = str(logical_source)
        path.write_text(json.dumps(job), encoding='utf-8')
        failed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertNotEqual(0, failed.returncode)
        self.assertEqual(b'original source must survive a cached-player export', logical_source.read_bytes())
        wrong_container = OUTPUT / 'prores-must-not-use-mp4.mp4'
        wrong_container.write_bytes(b'existing file before invalid container selection')
        job['output'].update(path=str(wrong_container), codec='prores_hq')
        path.write_text(json.dumps(job), encoding='utf-8')
        failed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, creationflags=subprocess.CREATE_NO_WINDOW, timeout=30)
        self.assertNotEqual(0, failed.returncode)
        self.assertIn(b'.mov container', failed.stdout)
        self.assertEqual(b'existing file before invalid container selection', wrong_container.read_bytes())

    def test_real_prores_hq_mov_audio_and_frame_count(self):
        spec = SourceSpec(str(self.fixture), Clock(F(24), F(101, 24), 24), 64, 48)
        output = OUTPUT / 'prores-hq-audio.mov'
        export_compare(TimestampVideoSource(spec), TimestampVideoSource(spec), IdentityColor(), IdentityColor(),
            CompareOptions(mode='horizontal'), ExportSettings(output_path=str(output), codec='prores_hq'),
            3, 14, audio_choice='b')
        with av.open(str(output)) as container:
            stream = container.streams.video[0]
            self.assertEqual('prores', stream.codec_context.name)
            self.assertIn('HQ', stream.codec_context.profile.upper())
            self.assertEqual('yuv422p10le', stream.codec_context.format.name)
            self.assertEqual(F(24), stream.average_rate)
            frames = list(container.decode(stream))
            self.assertEqual(12, len(frames))
            self.assertGreater(float(frames[-1].to_ndarray(format='rgb24').mean()), 5.)
            self.details['proresHq'] = {'frames': len(frames), 'profile': stream.codec_context.profile,
                                       'format': stream.codec_context.format.name}
        with av.open(str(output)) as container:
            self.assertEqual(1, len(container.streams.audio))
            audio = container.streams.audio[0]
            self.assertLessEqual(abs(audio.duration * audio.time_base - F(1, 2)), F(1, 48000))
            decoded = np.concatenate([frame.to_ndarray() for frame in container.decode(audio)], axis=1)
            self.assertLess(float(np.abs(decoded[:, :4000]).max()), .002)
            self.assertGreater(float(np.abs(decoded[:, 23000:24000]).max()), .02)

    def test_prores_keeps_more_than_eight_bit_composite_precision(self):
        # 1024 flat, DCT-aligned gray patches: an RGB8 input can only provide
        # 256 distinct patch means. Inspect native decoded 10-bit luma directly.
        gray = np.repeat(np.repeat(np.arange(1024, dtype=np.float32).reshape(8, 128) / 1023., 8, 0), 8, 1)
        rgb = np.repeat(gray[..., None], 3, axis=2)
        source = SyntheticSource(Clock(F(24), F(0), 1), 1024, 64)
        source.frame_at = lambda seconds: RawFrame(F(0), rgb)
        other = SyntheticSource(source.spec.clock, 1024, 64)
        precise = OUTPUT / 'prores-float-precision.mov'
        export_compare(source, other, IdentityColor(), IdentityColor(), CompareOptions(mode='a'),
                       ExportSettings(output_path=str(precise), codec='prores_hq'))
        control = OUTPUT / 'prores-eight-bit-control.mov'
        encoder = FFmpegExporter(ExportSettings(output_path=str(control), codec='prores_hq', width=1024, height=64))
        encoder.open()
        encoder.write_frame(np.rint(rgb[..., ::-1] * 255).astype(np.uint8))
        encoder.close()
        def levels(path):
            with av.open(str(path)) as container:
                frame = next(container.decode(video=0))
                self.assertEqual('yuv422p10le', frame.format.name)
                plane = frame.planes[0]
                samples = np.frombuffer(plane, dtype='<u2').reshape(plane.height, plane.line_size // 2)[:, :plane.width]
                patch_centers = samples[np.arange(8)[:, None] * 8 + 4, np.arange(128)[None, :] * 8 + 4]
                return len(np.unique(patch_centers))
        kept, eight = levels(precise), levels(control)
        self.assertGreater(kept, 700)
        self.assertLessEqual(eight, 300)
        self.details['proresPrecision'] = {'floatCompositeLumaLevels': kept, 'rgb8ControlLumaLevels': eight}

    def test_public_uint16_alpha_resize_preserves_precision(self):
        array = np.empty((32, 1024, 4), dtype=np.uint16)
        array[..., :3] = (12000, 24000, 48000)
        array[..., 3] = np.linspace(0, 65535, 1024, dtype=np.uint16)
        path = OUTPUT / 'prores-uint16-alpha-resized.mov'
        encoder = FFmpegExporter(ExportSettings(output_path=str(path), codec='prores_4444', width=512, height=32))
        encoder.open()
        encoder.write_frame(array)
        encoder.close()
        with av.open(str(path)) as container:
            image = next(container.decode(video=0)).to_ndarray(format='rgba64le')
        self.assertEqual((32, 512, 4), image.shape)
        self.assertEqual(np.uint16, image.dtype)
        alpha = image[16, :, 3]
        self.assertGreater(len(np.unique(alpha)), 400)
        self.assertLess(int(alpha.min()), 256)
        self.assertGreater(int(alpha.max()), 65000)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CompareExportTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {'testsRun': result.testsRun, 'failures': len(result.failures), 'errors': len(result.errors),
              'skipped': [{'test': str(t), 'reason': reason} for t, reason in result.skipped],
              'details': getattr(CompareExportTests, 'details', {}),
              'scope': 'Python backend and synthetic media only; native UI validation is separate'}
    (BASE / 'test_results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
