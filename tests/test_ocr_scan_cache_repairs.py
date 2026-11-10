"""R73/R74: actual OCR cache paths; decode/model boundaries use deterministic fixtures."""
import contextlib
import ctypes
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import uuid

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage16'
sys.path.insert(0, str(ROOT / 'tools/cgplay/ai'))
import subtitle_ocr_extract as worker
import subtitle_region_detector as detector


class ScanCacheRepairs(unittest.TestCase):
    @classmethod
    def tearDownClass(cls):
        visible = []
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def inspect_window(window, _):
            pid = ctypes.c_ulong()
            ctypes.windll.user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == os.getpid() and ctypes.windll.user32.IsWindowVisible(window):
                visible.append(int(window))
            return True
        callback = callback_type(inspect_window)
        ctypes.windll.user32.EnumWindows(callback, 0)
        (OUT / 'python_runtime.json').write_text(json.dumps({
            'backgroundMode': True, 'visiblePlatformWindows': len(visible),
            'boundaries': ['injected image decoding', 'injected OCR model output'],
            'actualLogic': ['worker main', 'cache validation', 'cache merge/reuse', 'VTT output']}), encoding='utf-8')
        if visible:
            raise AssertionError('Visible automation window')

    def setUp(self):
        self.directory = OUT / ('fixture_' + uuid.uuid4().hex)
        self.directory.mkdir(parents=True)
        self.media = self.directory / 'media.fixture'
        self.media.write_bytes(b'identity fixture; decoding injected')
        self.helper = self.directory / 'helper.fixture'
        self.helper.write_text('unused')
        self.cache = self.directory / 'candidate_scan.json'
        self.extractions = 0

    def seed(self, cues, intervals=None):
        payload = detector.build_scan_result(self.media, self.directory, cues, 20, 1)
        payload.update(scanStartSeconds=0, scanEndSeconds=20)
        if intervals is not None:
            payload['scannedIntervals'] = intervals
        detector.write_scan_result(self.cache, payload)
        return payload

    def run_scan(self, name, start, duration, maximum=100, fail_index=None, decode_fail_index=None):
        owner = self

        def batch(*args):
            owner.extractions += 1
            return True, [str(i) for i in range(maximum)], ''

        class Ocr:
            def __init__(self, *args):
                pass
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def run(self, image):
                if int(image) == fail_index:
                    return {'success': False, 'error': 'MODEL_FAILED'}
                return {'success': True, 'text': 'Recognized dialogue', 'backend': 'fixture'}

        def partial_batch(*args):
            owner.extractions += 1
            return False, [], 'fixture batch fallback'

        def single_frame(ffmpeg, media, at, image, *args):
            return round(at - start) != decode_fail_index

        output = self.directory / (name + '.json')
        argv = ['probe', '--media', str(self.media), '--helper', str(self.helper),
                '--output-json', str(output), '--output-vtt', str(self.directory / (name + '.vtt')),
                '--candidate-scan-json', str(self.cache), '--start', str(start),
                '--duration', str(duration), '--sample-interval', '1', '--max-samples', str(maximum)]
        with patch.object(sys, 'argv', argv), patch.object(worker, '_OcrServer', Ocr), \
             patch.object(worker, '_extract_frames_batch', batch if decode_fail_index is None else partial_batch), \
             patch.object(worker, '_extract_frame', single_frame), contextlib.redirect_stdout(io.StringIO()):
            if decode_fail_index is not None:
                Ocr.run = lambda self, image: {'success': True, 'text': 'Recognized dialogue', 'backend': 'fixture'}
            worker.main()
        return json.loads(output.read_text(encoding='utf-8'))

    def test_reverse_disjoint_overlap_and_legacy(self):
        self.seed([{'start': 12, 'end': 14, 'text': 'Later subtitle'}], [[10, 20]])
        first = self.run_scan('early', 0, 5)
        self.assertTrue(any(c['text'] == 'Later subtitle' for c in first['cues']))
        self.assertEqual([[0, 5], [10, 20]], first['candidateScan']['scannedIntervals'])
        before = self.extractions
        later = self.run_scan('later', 10, 10)
        self.assertTrue(later['reusedCandidateScanCache'])
        self.assertEqual(before, self.extractions)
        gap = self.run_scan('gap', 5, 5)
        self.assertEqual(before + 1, self.extractions)
        self.assertEqual([[0, 20]], gap['candidateScan']['scannedIntervals'])
        self.seed([{'start': 0, 'end': 20, 'text': 'Existing subtitle'}], [])
        overlap = self.run_scan('overlap', 5, 5)
        retained = [(c['start'], c['end']) for c in overlap['cues'] if c['text'] == 'Existing subtitle']
        self.assertEqual([(0, 5), (10, 20)], retained)
        self.seed([{'start': 12, 'end': 14, 'text': 'Legacy subtitle'}])
        before = self.extractions
        self.run_scan('legacy', 10, 10)
        self.assertEqual(before + 1, self.extractions)

    def test_failures_and_sample_limit_do_not_claim_full_coverage(self):
        self.seed([], [])
        failed = self.run_scan('failed_ocr', 0, 5, fail_index=2)
        self.assertEqual([[0, 2], [3, 5]], failed['candidateScan']['scannedIntervals'])
        self.seed([], [])
        failed = self.run_scan('failed_decode', 0, 5, decode_fail_index=2)
        self.assertEqual([[0, 2], [3, 5]], failed['candidateScan']['scannedIntervals'])
        self.seed([], [])
        limited = self.run_scan('limited', 0, 20, maximum=3)
        self.assertEqual([[0, 3]], limited['candidateScan']['scannedIntervals'])
        self.assertFalse(detector.scan_intervals_cover(limited['candidateScan']['scannedIntervals'], 0, 20))

    def test_malformed_cache_is_rejected_and_rebuilt(self):
        valid = self.seed([{'start': 1, 'end': 2, 'text': 'Valid dialogue'}], [[0, 5]])
        malformed = [[], None, 'wrong', {}, dict(valid, schema='unknown'),
                     dict(valid, candidateCues=[None]), dict(valid, candidateCues=[{'text': 'x', 'start': float('nan'), 'end': 2}]),
                     dict(valid, candidateCues=[{'text': [], 'start': 0, 'end': 2}]),
                     dict(valid, scannedIntervals=[[0, '5']]), dict(valid, scannedIntervals=[[True, 2]]),
                     dict(valid, sampleIntervalSeconds=float('inf'))]
        for index, value in enumerate(malformed):
            detector.write_scan_result(self.cache, value)
            self.assertIsNone(detector.load_scan_result(self.cache, self.media))
            result = self.run_scan('malformed_' + str(index), 0, 2)
            self.assertTrue(result['success'])
        detector.write_scan_result(self.cache, valid)
        self.assertIsNotNone(detector.load_scan_result(self.cache, self.media))
        self.media.write_bytes(b'changed identity')
        self.assertIsNone(detector.load_scan_result(self.cache, self.media))

    def test_native_consumer_does_not_suppress_gap(self):
        source = (ROOT / 'src/services/ai/TranslationEnhancementScheduler.cpp').read_text(encoding='utf-8-sig')
        code = source[source.index('    const bool candidateScanFingerprintMatches ='):source.index('    const bool visualTrackAuthoritative =')]
        native = COMMON + '\n#include <cmath>\n#include <algorithm>\n' + r'''
bool covered(QJsonObject candidateScan,double at){QString mediaFingerprint="fixture";
struct{double currentSeconds;}request{at};bool visualTrackGenerated=true;
double visualCoverageStartSeconds=0,visualCoverageEndSeconds=20;
BLOCK
return currentTimeWithinVisualCoverage;}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;
QJsonObject cache{{"mediaFingerprint","fixture"},{"scanStartSeconds",0},{"scanEndSeconds",20},
{"scannedIntervals",QJsonArray{QJsonArray{0,5},QJsonArray{10,20}}}};
report["coveredIntervalsAccepted"]=covered(cache,2)&&covered(cache,12);
report["gapDoesNotSuppressQuickSubtitle"]=!covered(cache,7);
report["emptyIntervalsDoNotFallbackToBounds"]=!covered(QJsonObject{{"mediaFingerprint","fixture"},{"scanStartSeconds",0},{"scanEndSeconds",20},{"scannedIntervals",QJsonArray{}}},7);
cache.remove("scannedIntervals");report["legacyBoundsDoNotSuppress"]=!covered(cache,7);
cache["scannedIntervals"]=QJsonArray{QJsonArray{0,20}};cache["mediaFingerprint"]="stale";
report["staleIdentityDoesNotSuppress"]=!covered(cache,7);
cache["mediaFingerprint"]="fixture";cache["scannedIntervals"]=QJsonArray{QJsonArray{0,"20"},QJsonArray{-1,20},QJsonArray{10,5}};
report["invalidIntervalsRejected"]=!covered(cache,7);return finish(report);}
'''
        directory = OUT / 'native'; directory.mkdir(exist_ok=True)
        (directory / 'main.cpp').write_text(native.replace('BLOCK', code), encoding='utf-8')
        (directory / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.21)\nproject(ScanCoverage LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nfind_package(Qt6 REQUIRED COMPONENTS Core)\nadd_executable(scan main.cpp)\ntarget_link_libraries(scan PRIVATE Qt6::Core user32)\n', encoding='utf-8')
        no_window = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        build = directory / 'build'
        for name, command in [('configure', ['cmake', '-S', str(directory), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT}']), ('build', ['cmake', '--build', str(build), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=no_window, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        exe = build / 'Release/scan.exe'; deploy_runtime(exe, QT, ())
        result = subprocess.run([str(exe)], capture_output=True, creationflags=no_window, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode().splitlines()[-1])
        (OUT / 'native.json').write_text(json.dumps(report, indent=2))
        self.assertTrue(report['passed'])


if __name__ == '__main__':
    unittest.main()
