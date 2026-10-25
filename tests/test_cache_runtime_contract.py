"""Two real native players, generated media, and background cache controls."""
from pathlib import Path
import json
import os
import subprocess
import unittest

from tests.run_state_regression import run

ROOT=Path(__file__).resolve().parents[1]
ARTIFACT=Path(os.environ.get('CGPLAY_CACHE_TEST_ARTIFACTS',ROOT/'tests/artifacts/cache_runtime_1_0_8'))


class CacheRuntimeTests(unittest.TestCase):
    def test_live_native_options_and_clear_lifecycle(self):
        ARTIFACT.mkdir(parents=True,exist_ok=True)
        release=ROOT/'build_win_full/bin/Release'
        for rate in (30,24):
            result=run([release/'ffmpeg.exe','-v','error','-y','-f','lavfi','-i',
                f'testsrc2=size=160x90:rate={rate}','-t','2','-c:v','libx264','-pix_fmt','yuv420p',
                ARTIFACT/f'cache_{rate}.mp4'],ARTIFACT/f'fixture_{rate}.json')
            self.assertTrue(result['passed'],result)
        environment=os.environ.copy()
        environment.update(CGPLAY_SMOKE_GROUPS='cache',CGPLAY_SMOKE_EVIDENCE_DIR=str(ARTIFACT),
            CGPLAY_CACHE_SECOND_MEDIA=str(ARTIFACT/'cache_24.mp4'))
        report_path=ARTIFACT/'player.json'
        evidence=run([release/'CGPlay.exe','--smoke-player',str(ARTIFACT/'cache_30.mp4'),
            '--smoke-output',str(report_path),'--automation-background',
            '--automation-settings-namespace','cache_runtime_108'],ARTIFACT/'process.json',env=environment)
        self.assertTrue(evidence['passed'],evidence)
        report=json.loads(report_path.read_text(encoding='utf-8'))
        failures=[r for r in report['results'] if r.get('status')=='FAIL']
        self.assertEqual([],failures)
        self.assertGreaterEqual(len(report['results']),11)
        self.assertTrue(report['coverage']['complete'])
        for result in report['results']:
            if result['name'].startswith(('Native cache','Native request','Source/stat','Explicit native','Closed controller')):
                self.assertEqual(0,result['details']['nativeVisibleWindows'])


if __name__=='__main__':
    unittest.main()
