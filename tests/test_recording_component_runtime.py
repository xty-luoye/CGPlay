"""Exercise production recorder component validation and optional real stage."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get('CGPLAY_RECORDING_COMPONENT_ARTIFACTS',
    ROOT/'tests/artifacts/recording_component_1_0_8'))
QT = Path(os.environ.get('CGPLAY_TEST_QT_ROOT', 'E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64'))


class RecordingComponentRuntimeTests(unittest.TestCase):
    def test_runtime_requires_valid_identity_and_capture_modules(self):
        OUT.mkdir(parents=True, exist_ok=True)
        build = OUT/'build'
        environment = os.environ.copy()
        environment['PATH'] = str(QT/'bin')+os.pathsep+environment.get('PATH','')
        commands = [
            ['cmake','-S',ROOT/'tests/component_install_contracts','-B',build,
             '-G','Visual Studio 17 2022','-A','x64',f'-DCMAKE_PREFIX_PATH={QT}',
             f'-DCGPLAY_SOURCE_ROOT={ROOT}',f'-DCGPLAY_TEST_EXTRACTED_DIR={OUT}'],
            ['cmake','--build',build,'--config','Release','--target','recording_runtime_probe','--parallel','2'],
        ]
        native = [build/'Release/recording_runtime_probe.exe',OUT]
        if environment.get('CGPLAY_TEST_RECORDER_RUNTIME'):
            native.append(environment['CGPLAY_TEST_RECORDER_RUNTIME'])
        commands.append(native)
        with (OUT/'commands.log').open('wb') as log:
            for command in commands:
                subprocess.run([str(v) for v in command],cwd=ROOT,env=environment,check=True,
                    creationflags=subprocess.CREATE_NO_WINDOW,stdout=log,stderr=log,timeout=180)
        report = json.loads((OUT/'report.json').read_text(encoding='utf-8'))
        self.assertTrue(report['passed'],report)
        self.assertGreaterEqual(len(report['checks']),10)
        self.assertEqual(0,report['nativeVisibleWindows'])
        if environment.get('CGPLAY_TEST_RECORDER_RUNTIME'):
            self.assertTrue(report['actualRuntimeChecked'])


if __name__=='__main__':
    unittest.main()
