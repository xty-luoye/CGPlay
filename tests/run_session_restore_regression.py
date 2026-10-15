"""Real session/Qt/tlRender recovery regression with artifact-local snapshots."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from run_state_regression import ROOT, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exe', type=Path, default=ROOT/'build_win_full/bin/Release/CGPlay.exe')
    parser.add_argument('--ffmpeg', type=Path, default=ROOT/'build_win_full/bin/Release/ffmpeg.exe')
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT/'tests/artifacts').resolve()):
        parser.error('--output must be inside tests/artifacts')
    output.mkdir(parents=True, exist_ok=True)
    media, other = output/'clip.mp4', output/'other.mp4'
    exr = output/'exr'
    exr.mkdir(exist_ok=True)
    fixtures = [
        ('video', ['-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=24', '-t', '6', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', media]),
        ('other', ['-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=30', '-t', '6', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', other]),
        ('exr', ['-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=24', '-frames:v', '32', '-pix_fmt', 'gbrpf32le',
                 '-start_number', '1001', exr/'frame.%04d.exr'])]
    for name, command in fixtures:
        if not run([args.ffmpeg.resolve(), '-hide_banner', '-loglevel', 'error', '-y', *command], output/f'fixture_{name}.json')['passed']:
            raise RuntimeError('Fixture failed: ' + name)
    report = output/'report.json'
    env = os.environ.copy()
    env.update(CGPLAY_SMOKE_GROUPS='session-restore', CGPLAY_SMOKE_EVIDENCE_DIR=str(output),
               CGPLAY_SMOKE_OTHER_VIDEO=str(other), CGPLAY_SMOKE_EXR=str(exr/'frame.1001.exr'))
    namespace = 'session-restore-' + hashlib.sha256(str(output).encode()).hexdigest()[:12]
    process = run([args.exe.resolve(), '--automation-background', '--automation-settings-namespace', namespace,
                   '--smoke-player', media, '--smoke-output', report], output/'process.json', env, timeout=180)
    data = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
    summary, automation = data.get('summary', {}), data.get('automation', {})
    passed = (process['passed'] and summary.get('pass', 0) > 0 and summary.get('fail') == 0 and summary.get('skip') == 0
              and automation.get('background') is True and automation.get('visiblePlatformWindows') == 0
              and data.get('coverage', {}).get('complete') is True)
    result = dict(passed=passed, summary=summary, backgroundMode=True, report=str(report),
                  exeSha256=hashlib.sha256(args.exe.read_bytes()).hexdigest())
    (output/'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
