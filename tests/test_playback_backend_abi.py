"""Real cross-DLL playback ABI and production plugin-loader compatibility checks."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'tests/artifacts/migration_1_0_8_20261025'


def run(command, folder, name, environment=None):
    with (folder / (name + '.log')).open('wb') as output:
        result = subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT,
                                env=environment, creationflags=subprocess.CREATE_NO_WINDOW, timeout=180)
    if result.returncode:
        raise AssertionError(str(folder / (name + '.log')))


class PlaybackBackendAbi(unittest.TestCase):
    def test_opposite_backend_flags_opaque_signal_and_legacy_plugin_rejection(self):
        self.assertEqual(os.name, 'nt')
        folder = BASE / 'backend_abi_validation' / str(time.time_ns())
        folder.mkdir(parents=True)
        qt = Path(json.loads((BASE / 'engine_assessment/qt_extraction/receipt.json').read_text(encoding='utf-8'))['kit'])
        # Generate only fixture overrides from the actual pure interface. The
        # assertions exercise virtual dispatch, not duplicated service behavior.
        interface = (ROOT / 'src/core/playback/api/IPlaybackService.h').read_text(encoding='utf-8')
        overrides = []
        for signature in re.findall(r'virtual\s+([^;{}]+?)\s*=\s*0\s*;', interface):
            match = re.fullmatch(r'(.+?)\s+(\w+)\(([^)]*)\)(\s+const)?', signature.strip())
            self.assertIsNotNone(match, signature)
            result_type, name, parameters, const = match.groups()
            body = {
                'fps': 'return 24.;', 'pause': '++pauseCalls;', 'setInPoint': 'lastIn=frame;',
                'seekToFrame': 'lastSeek=frame;', 'setMute': 'muteValue=muted;',
            }.get(name, '' if result_type.strip() == 'void' else 'return {};')
            overrides.append(f'{result_type} {name}({parameters}){const or ""} override {{ {body} }}')
        (folder / 'FakePlayback.h').write_text('#pragma once\n#include "core/playback/api/IPlaybackService.h"\n'
            'namespace cgplay { struct FakePlayback final : IPlaybackService { int pauseCalls=0,lastIn=0,lastSeek=0;bool muteValue=false;\n'
            + '\n'.join(overrides) + '\n}; }\n', encoding='utf-8')
        fixture = BASE / 'backend_abi_fixture'
        self.assertTrue((fixture / 'CMakeLists.txt').is_file())
        build = folder / 'build'
        run(['cmake', '-S', fixture, '-B', build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
             '-DQt6_DIR=' + (qt / 'lib/cmake/Qt6').as_posix(), '-DAPP_ROOT=' + ROOT.as_posix(),
             '-DFIXTURE_DATA=' + folder.as_posix()], folder, 'configure')
        run(['cmake', '--build', build, '--config', 'Release', '--parallel', '2'], folder, 'build')
        environment = os.environ.copy()
        environment['PATH'] = str(qt / 'bin') + os.pathsep + environment['PATH']
        records = []
        for host in (0, 1):
            for consumer in (0, 1):
                report = folder / f'host{host}_consumer{consumer}.json'
                run([build / f'Release/AbiHost{host}.exe', build / f'Release/AbiConsumer{consumer}.dll', report],
                    folder, f'host{host}_consumer{consumer}', environment)
                data = json.loads(report.read_text(encoding='utf-8'))
                self.assertTrue(data['passed'], data)
                self.assertTrue(data['queuedOwnerReleased'], data)
                records.append(data)
        marker = folder / 'legacy_ctor.txt'
        good = folder / 'good_events.txt'
        environment['CGPLAY_ABI_LEGACY_MARKER'] = str(marker)
        environment['CGPLAY_ABI_GOOD_EVENTS'] = str(good)
        report = folder / 'plugin_manager.json'
        run([build / 'Release/AbiPluginManager.exe', build / 'legacy/Release', build / 'good/Release', report,
             BASE / 'build_qt6113_1_0_8/bin/Release/plugins'], folder, 'plugin_manager', environment)
        manager = json.loads(report.read_text(encoding='utf-8'))
        self.assertTrue(manager['passed'], manager)
        self.assertEqual(manager['realLegacyPluginsAccepted'], 0)
        self.assertGreaterEqual(manager['realLegacyPluginsSeen'], 4)
        self.assertEqual(good.read_text(encoding='utf-8').splitlines(), ['ctor', 'init', 'shutdown'])
        # Negative control proves the legacy fixture's constructor really does
        # create its marker when intentionally loaded outside the manager.
        self.assertEqual(marker.read_text(encoding='utf-8').strip(), 'legacy-ctor')
        receipt = {'passed': True, 'crossModuleCases': records, 'manager': manager, 'folder': str(folder),
                   'nativeSdkIncludedByFixture': False}
        (folder / 'summary.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
        (BASE / 'backend_abi_validation/latest.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')


if __name__ == '__main__':
    unittest.main()
