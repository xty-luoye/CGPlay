"""Exercise both legacy CLIs against real named-channel EXR and PyAV."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import uuid

import av
import numpy as np
import OpenImageIO as oiio

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
OUT = ROOT/'tests/artifacts/fix_review_20261101/legacy_regression'


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools'/f'{name}.py')
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def exr(path, names, values):
    spec = oiio.ImageSpec(64, 32, len(names), oiio.FLOAT)
    spec.channelnames = names
    pixels = np.empty((32, 64, len(names)), np.float32)
    pixels[:] = values
    output = oiio.ImageOutput.create(str(path))
    assert output.open(str(path), spec), output.geterror()
    try:
        assert output.write_image(pixels)
    finally:
        output.close()


class LegacyRepairs(unittest.TestCase):
    def setUp(self):
        self.folder = OUT/(self._testMethodName+'_'+uuid.uuid4().hex[:8])
        self.folder.mkdir(parents=True)

    def test_named_beauty_is_identical_with_normal_and_depth_channels(self):
        control = self.folder/'control.exr'
        exr(control, ['R','G','B'], [.1,.2,.3])
        cases = [(['N.x','N.y','N.z','beauty.R','beauty.G','beauty.B'], [.8,.7,.6,.1,.2,.3]),
                 (['Z','beauty.R','beauty.G','beauty.B'], [.9,.1,.2,.3])]
        for name in ['export_video','export_video_simple']:
            entry = module(name)
            read = getattr(entry, 'read_exr_with_oiio', None) or entry.read_exr
            expected = read(control)
            for index, (names, values) in enumerate(cases):
                path = self.folder/f'case{index}.exr'
                exr(path, names, values)
                np.testing.assert_array_equal(expected, read(path))

    def test_all_sequence_separators_work_through_real_cli(self):
        for name in ['export_video','export_video_simple']:
            for separator in '._-':
                with self.subTest(name=name, separator=separator):
                    folder = self.folder/(name+'_'+str(ord(separator)))
                    folder.mkdir()
                    for frame in range(1,4):
                        exr(folder/f'shot{separator}{frame:04d}.exr', ['R','G','B'], [.1,.2,.3])
                    source = folder/f'shot{separator}0001.exr'
                    output = folder/'out.mp4'
                    result = subprocess.run([sys.executable, '-B', str(ROOT/'tools'/f'{name}.py'), str(source), str(output)],
                        capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
                    (folder/'cli.log').write_bytes(result.stdout+result.stderr)
                    self.assertEqual(0, result.returncode, result.stderr)
                    with av.open(str(output)) as container:
                        frames = list(container.decode(video=0))
                    self.assertEqual(3, len(frames))
                    # Actual encoded image is beauty RGB, not saturated normal/Z.
                    rgb = frames[0].to_ndarray(format='rgb24').mean(axis=(0,1))
                    self.assertTrue(70 < rgb[0] < 105 and 110 < rgb[1] < 140 and 135 < rgb[2] < 165, rgb)

    def test_data_only_and_missing_frames_fail_and_preserve_old_output(self):
        for name in ['export_video','export_video_simple']:
            for kind in ['data','gap']:
                folder = self.folder/(name+'_'+kind)
                folder.mkdir()
                source, output = folder/'shot.0001.exr', folder/'out.mp4'
                exr(source, ['Z'] if kind=='data' else ['R','G','B'], [.9] if kind=='data' else [.1,.2,.3])
                if kind=='gap':
                    exr(folder/'shot.0003.exr', ['R','G','B'], [.1,.2,.3])
                output.write_bytes(b'existing output')
                result = subprocess.run([sys.executable, '-B', str(ROOT/'tools'/f'{name}.py'), str(source), str(output)],
                    capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
                (folder/'cli.log').write_bytes(result.stdout+result.stderr)
                self.assertNotEqual(0, result.returncode)
                self.assertEqual(b'existing output', output.read_bytes())

    def test_simple_encoder_failure_is_nonzero_and_preserves_target(self):
        entry = module('export_video_simple')
        source, target = self.folder/'shot.0001.exr', self.folder/'out.mp4'
        exr(source, ['R','G','B'], [.1,.2,.3])
        target.write_bytes(b'existing output')
        with patch.object(sys, 'argv', ['export_video_simple.py', str(source), str(target)]), \
                patch('cgplay.export.export_job._open_exporter', side_effect=RuntimeError('injected encoder failure')):
            self.assertEqual(1, entry.main())
        self.assertEqual(b'existing output', target.read_bytes())


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(LegacyRepairs))
    (OUT/'result.json').write_text(json.dumps({'tests':result.testsRun,'errors':len(result.errors),
        'failures':len(result.failures),'passed':result.wasSuccessful(),'backgroundMode':True,
        'realOIIO':oiio.VERSION_STRING,'realPyAV':av.__version__},indent=2)+'\n',encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
