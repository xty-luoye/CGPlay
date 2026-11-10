"""Real OIIO/PyAV regression cases for the export audit repairs."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from fractions import Fraction
import unittest
from unittest.mock import patch
import uuid

import av
import numpy as np
import OpenImageIO as oiio
import PyOpenColorIO as ocio

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from cgplay.export.export_job import export_frames, _export_image_sequence, _VIDEO_EXTS
from cgplay.export.models.export_settings import ExportSettings
from cgplay.export.utils.image_reader import ImageReader, _process_pixels
from cgplay.export.utils.video_reader import VideoReader
from cgplay.export.recording_finalize import protected_inputs, assert_target

OUT = ROOT / 'tests/artifacts/fix_review_20261101/export_regression'


def write_exr(path, data, **attributes):
    spec = oiio.ImageSpec(data.shape[1], data.shape[0], data.shape[2], oiio.FLOAT)
    spec.channelnames = ['R', 'G', 'B', 'A'][:data.shape[2]]
    for key, value in attributes.items():
        setattr(spec, key, value)
    writer = oiio.ImageOutput.create(str(path))
    assert writer.open(str(path), spec), writer.geterror()
    try:
        assert writer.write_image(data), writer.geterror()
    finally:
        writer.close()


class ExportRepairTests(unittest.TestCase):
    def setUp(self):
        self.folder = OUT / (self._testMethodName + '_' + uuid.uuid4().hex[:8])
        self.folder.mkdir(parents=True)

    def test_sparse_relative_export_rejects_without_replacing_target(self):
        source = self.folder / 'shot.1001.exr'
        data = np.full((32, 64, 3), .2, np.float32)
        write_exr(source, data)
        write_exr(self.folder / 'shot.1003.exr', data)
        target = self.folder / 'out.mov'
        target.write_bytes(b'existing output')
        with self.assertRaisesRegex(ValueError, 'missing frames'):
            export_frames(str(source), ExportSettings(output_path=str(target), codec='prores_4444'),
                          start_frame=0, end_frame=2, frame_numbering='relative')
        self.assertEqual(b'existing output', target.read_bytes())

    def test_video_extensions_match_native_probe(self):
        source = (ROOT / 'src/services/media/MediaProbe.cpp').read_text(encoding='utf-8')
        block = source.split('const QStringList& MediaProbe::videoExtensions()', 1)[1].split('return extensions;', 1)[0]
        self.assertEqual({'.' + ext for ext in re.findall(r'QStringLiteral\("([a-z0-9]+)"\)', block)}, _VIDEO_EXTS)

    def test_real_ts_exports_as_video(self):
        source, target = self.folder / 'source.ts', self.folder / 'out.mov'
        with av.open(str(source), 'w', format='mpegts') as container:
            stream = container.add_stream('libx264', rate=24)
            stream.width, stream.height, stream.pix_fmt = 64, 32, 'yuv420p'
            for index in range(6):
                frame = av.VideoFrame.from_ndarray(np.full((32, 64, 3), index * 20, np.uint8), format='rgb24')
                for packet in stream.encode(frame):
                    container.mux(packet)
            for packet in stream.encode():
                container.mux(packet)
        export_frames(str(source), ExportSettings(output_path=str(target), codec='prores_4444'), start_frame=0, end_frame=5)
        with av.open(str(target)) as container:
            self.assertEqual(6, len(list(container.decode(video=0))))

    def test_exr_restores_display_window_and_crops_overscan(self):
        source = self.folder / 'window.exr'
        write_exr(source, np.full((4, 6, 3), .25, np.float32), x=2, y=3, full_x=0, full_y=0, full_width=12, full_height=10)
        image = ImageReader.read_frame(str(source), output_float=True)
        self.assertEqual((10, 12, 3), image.shape)
        self.assertTrue(np.all(image[:3] == 0))
        self.assertTrue(np.all(image[3:7, 2:8] > 0))
        self.assertTrue(np.all(image[7:] == 0))
        overscan = self.folder / 'overscan.exr'
        write_exr(overscan, np.full((8, 8, 3), .25, np.float32), x=-2, y=-3, full_x=0, full_y=0, full_width=4, full_height=4)
        cropped = ImageReader.read_frame(str(overscan), output_float=True)
        self.assertEqual((4, 4, 3), cropped.shape)
        self.assertTrue(np.all(cropped > 0))

    def test_gray_alpha_keeps_luma_and_alpha(self):
        data = np.empty((4, 6, 2), np.uint8)
        data[:, :, 0], data[:, :, 1] = 100, 64
        result = _process_pixels(data, source_encoding='display')
        self.assertEqual((4, 6, 4), result.shape)
        self.assertTrue(np.all(result[:, :, :3] == 100))
        self.assertTrue(np.all(result[:, :, 3] == 64))
        from PIL import Image
        source, target = self.folder/'gray.png', self.folder/'gray.mov'
        Image.fromarray(np.tile(data, (8, 8, 1)), mode='LA').save(source)
        export_frames(str(source), ExportSettings(output_path=str(target), codec='prores_4444'))
        with av.open(str(target)) as container:
            frame = next(container.decode(video=0)).to_ndarray(format='rgba')
        self.assertLessEqual(abs(float(frame[:, :, 3].mean()) - 64), 2)

    def test_dnxhr_sequence_input_preserves_high_precision(self):
        import logging
        source = self.folder/'gradient.0001.exr'
        data = np.repeat(np.linspace(0, 1, 512, dtype=np.float32)[None, :, None], 3, axis=2)
        write_exr(source, np.repeat(data, 32, axis=0))
        captured = []
        class CaptureEncoder:
            def write_frame(self, image): captured.append(image.copy())
            def close(self): pass
        settings = ExportSettings(output_path=str(self.folder/'not_encoded.mov'), codec='dnxhr')
        with patch('cgplay.export.export_job._open_exporter', return_value=CaptureEncoder()):
            _export_image_sequence(str(source), settings, logging.getLogger('probe'), None, None, None)
        self.assertEqual(np.uint16, captured[0].dtype)
        self.assertEqual(512, np.unique(captured[0][0, :, 0]).size)

    def test_planar_video_alpha_survives_real_export(self):
        source, target = self.folder / 'alpha.mkv', self.folder / 'out.mov'
        command = [str(ROOT / 'build_win_full/bin/Release/ffmpeg.exe'), '-hide_banner', '-v', 'error', '-nostdin',
                   '-f', 'lavfi', '-i', 'color=red@0.25:size=64x32:rate=24:duration=0.125,format=rgba',
                   '-c:v', 'ffv1', '-pix_fmt', 'gbrap16le', str(source)]
        subprocess.run(command, check=True, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        with VideoReader().open(str(source)) as reader:
            frames = list(reader.read_frames())
            again = list(reader.read_frames())
        self.assertEqual(4, frames[0].shape[2])
        self.assertEqual(len(frames), len(again))
        np.testing.assert_array_equal(frames[0], again[0])
        export_frames(str(source), ExportSettings(output_path=str(target), codec='prores_4444'), start_frame=0, end_frame=2)
        with av.open(str(target)) as container:
            output = list(container.decode(video=0))
        self.assertEqual(3, len(output))
        for frame in output:
            alpha = frame.to_ndarray(format='rgba')[:, :, 3]
            self.assertLessEqual(abs(float(alpha.mean()) - 63), 2)

    def test_negative_pts_first_and_repeated_reads_export(self):
        source, target = self.folder / 'negative.mkv', self.folder / 'out.mov'
        with av.open(str(source), 'w', options={'avoid_negative_ts': 'disabled'}) as container:
            stream = container.add_stream('ffv1', rate=10)
            stream.width = stream.height = 32
            stream.pix_fmt, stream.time_base = 'bgr0', Fraction(1, 10)
            for index in range(20):
                frame = av.VideoFrame.from_ndarray(np.full((32, 32, 3), index * 10, np.uint8), format='bgr24')
                frame.pts, frame.time_base = index - 10, Fraction(1, 10)
                for packet in stream.encode(frame):
                    container.mux(packet)
            for packet in stream.encode():
                container.mux(packet)
        with VideoReader().open(str(source)) as reader:
            first = list(reader.read_frames())
            second = list(reader.read_frames())
        self.assertEqual((20, 20), (len(first), len(second)))
        np.testing.assert_array_equal(first[0], second[0])
        export_frames(str(source), ExportSettings(output_path=str(target), codec='prores_4444'), start_frame=0, end_frame=19)
        with av.open(str(target)) as container:
            self.assertEqual(20, len(list(container.decode(video=0))))

    def test_config_output_and_log_hardlink_are_protected(self):
        config = ocio.Config.CreateRaw()
        config.addDisplayView(display='audit', view='raw', colorSpaceName='raw')
        config.setActiveDisplays('audit')
        config.setActiveViews('raw')
        path = self.folder / 'config.mov'
        path.write_text(config.serialize(), encoding='utf-8')
        original = path.read_bytes()
        source = self.folder / 'shot.0001.exr'
        write_exr(source, np.full((32, 64, 3), .2, np.float32))
        color = dict(mode='ocio', configPath=str(path), input='raw', display='audit', view='raw')
        with self.assertRaises(ValueError):
            export_frames(str(source), ExportSettings(output_path=str(path), codec='prores_4444'), color_settings=color)
        self.assertEqual(original, path.read_bytes())
        alias = self.folder / 'log.txt'
        os.link(path, alias)
        with self.assertRaises(ValueError):
            export_frames(str(source), ExportSettings(output_path=str(self.folder/'out.mov'), codec='prores_4444', log_file=str(alias)), color_settings=color)
        self.assertEqual(original, path.read_bytes())

    def test_recording_protection_retains_missing_sequence_paths(self):
        source = self.folder / 'moved/shot.0001.exr'
        protected = protected_inputs([str(source)])
        self.assertEqual(1, len(protected))
        self.assertIsNone(protected[0]['inode'])
        with self.assertRaises(ValueError):
            assert_target(source, protected)
        target = self.folder / 'recording.mp4'
        assert_target(target, protected)


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ExportRepairTests))
    (OUT / 'result.json').write_text(json.dumps({'tests': result.testsRun, 'failures': len(result.failures),
        'errors': len(result.errors), 'passed': result.wasSuccessful(), 'backgroundMode': True,
        'realPyAV': av.__version__, 'realOIIO': oiio.VERSION_STRING}, indent=2) + '\n', encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
