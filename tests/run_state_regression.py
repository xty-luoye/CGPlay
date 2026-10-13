"""Background integration tests with real tlRender/Qt, generated fixtures and window monitoring.

Requires ffmpeg and psutil. All outputs live beneath tests/artifacts; no user settings or media.
Run: python tests/run_state_regression.py --output tests/artifacts/<task>/regression
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import psutil

ROOT = Path(__file__).resolve().parents[1]


def visible_pids():
    if os.name != 'nt':
        raise RuntimeError('This runner verifies Windows native window visibility.')
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    found = []
    @callback
    def visit(hwnd, unused):
        if user32.IsWindowVisible(hwnd):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found.append(pid.value)
        return True
    user32.EnumWindows(visit, 0)
    return found


def run(command, output, env=None, timeout=150):
    started = time.monotonic()
    evidence = {'backgroundMode': True, 'visibleAutomationWindowsMax': 0, 'command': list(map(str, command))}
    with output.with_suffix('.log').open('w', encoding='utf-8') as log:
        proc = subprocess.Popen(evidence['command'], env=env, cwd=ROOT, stdout=log,
                                stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        known = {proc.pid}
        timed_out = False
        while proc.poll() is None:
            try:
                known.update(p.pid for p in psutil.Process(proc.pid).children(recursive=True))
            except psutil.NoSuchProcess:
                pass
            evidence['visibleAutomationWindowsMax'] = max(evidence['visibleAutomationWindowsMax'],
                sum(pid in known for pid in visible_pids()))
            if time.monotonic() - started > timeout:
                timed_out = True
                for pid in known:
                    try: psutil.Process(pid).kill()
                    except psutil.NoSuchProcess: pass
            time.sleep(.1)
        evidence.update(exitCode=proc.returncode, timedOut=timed_out, elapsedSeconds=round(time.monotonic()-started, 2))
    evidence['passed'] = proc.returncode == 0 and not timed_out and evidence['visibleAutomationWindowsMax'] == 0
    output.write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT/'build_win_full/bin/Release/CGPlay.exe')
    parser.add_argument('--ffmpeg', type=Path, default=ROOT/'build_win_full/bin/Release/ffmpeg.exe')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--groups', default='state-switch,sequence-time,fullscreen')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT/'tests/artifacts').resolve()):
        parser.error('--output must be inside tests/artifacts')
    output.mkdir(parents=True, exist_ok=True)
    fixtures = {}
    for first in (0, 101, 1001):
        folder = output/f'fixtures/start{first}'
        folder.mkdir(parents=True, exist_ok=True)
        command = [args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
            'testsrc=size=64x48:rate=24', '-frames:v', '32', '-pix_fmt', 'gbrpf32le', '-start_number', str(first),
            folder/'sequence.%04d.exr']
        if not run(command, output/f'fixture_{first}.json')['passed']:
            raise RuntimeError('EXR fixture failed')
        fixtures[first] = folder/f'sequence.{first:04d}.exr'
    video = output/'fixtures/clip.mp4'
    if not run([args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                'testsrc2=size=320x180:rate=24', '-t', '6', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', video],
               output/'fixture_video.json')['passed']:
        raise RuntimeError('Video fixture failed')
    cases = []
    groups = args.groups.split(',')
    if 'state-switch' in groups:
        cases.append(('state-switch', 101, 24))
    if 'sequence-time' in groups:
        cases.extend(('sequence-time', start, fps) for start, fps in ((0, 24), (101, 24), (1001, 30), (101, 23.976), (101, 1)))
    if 'fullscreen' in groups:
        cases.append(('fullscreen', 0, 24))
    summaries = []
    for group, first, fps in cases:
        name = f'{group}_{first}_{fps}'
        folder = output/name
        folder.mkdir(exist_ok=True)
        report = folder/'report.json'
        env = os.environ.copy()
        env.update(CGPLAY_SMOKE_GROUPS=group, CGPLAY_SMOKE_SEQUENCE_FPS=str(fps),
            CGPLAY_SMOKE_OTHER_SEQUENCE=str(fixtures[1001]), CGPLAY_SMOKE_VIDEO=str(video),
            CGPLAY_SMOKE_EVIDENCE_DIR=str(folder))
        namespace = 'state-contract-' + hashlib.sha256(str(folder).encode()).hexdigest()[:12]
        wrapper = run([args.exe.resolve(), '--automation-background', '--automation-settings-namespace', namespace,
            '--smoke-player', video if group == 'fullscreen' else fixtures[first], '--smoke-output', report],
            folder/'process.json', env)
        data = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
        summary, automation = data.get('summary', {}), data.get('automation', {})
        passed = (wrapper['passed'] and summary.get('fail') == 0 and summary.get('skip', 0) == 0
            and summary.get('pass', 0) > 0 and automation.get('background') is True
            and automation.get('visiblePlatformWindows') == 0 and data.get('coverage', {}).get('complete') is True)
        item = dict(name=name, passed=passed, summary=summary, report=str(report))
        summaries.append(item)
        print(json.dumps(item), flush=True)
    result = dict(passed=bool(summaries) and all(c['passed'] for c in summaries), cases=summaries,
        backgroundMode=True, exe=str(args.exe.resolve()), exeSha256=hashlib.sha256(args.exe.read_bytes()).hexdigest())
    (output/'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
