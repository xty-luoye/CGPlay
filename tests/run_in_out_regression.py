"""Real I/O key and loop checks; generated media, isolated settings, no visible windows."""
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
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT/'tests/artifacts').resolve()):
        parser.error('--output must be inside tests/artifacts')
    output.mkdir(parents=True, exist_ok=True)
    ffmpeg = ROOT/'build_win_full/bin/Release/ffmpeg.exe'
    short = output/'short.mp4'
    long = output/'one_hour.mp4'
    exr = output/'exr'
    exr.mkdir(exist_ok=True)
    fixtures = [
        ('short', ['-f','lavfi','-i','testsrc2=size=160x90:rate=24','-f','lavfi','-i','sine=frequency=440:sample_rate=48000',
            '-t','4','-c:v','libx264','-preset','fast','-g','24','-c:a','aac','-b:a','64k',short]),
        ('long', ['-stream_loop','899','-i',short,'-c','copy',long]),
        ('exr', ['-f','lavfi','-i','testsrc=size=64x48:rate=24','-frames:v','32','-pix_fmt','gbrpf32le','-start_number','1001',exr/'frame.%04d.exr'])]
    for name, command in fixtures:
        if not run([ffmpeg,'-hide_banner','-loglevel','error','-y',*command], output/f'fixture_{name}.json')['passed']:
            raise RuntimeError('Fixture failed: ' + name)
    cases = []
    for name, media in [('long',long),('short',short),('exr',exr/'frame.1001.exr')]:
        folder = output/name
        folder.mkdir(exist_ok=True)
        report = folder/'report.json'
        env = os.environ.copy()
        env.update(CGPLAY_SMOKE_GROUPS='in-out', CGPLAY_SMOKE_EVIDENCE_DIR=str(folder))
        namespace = 'in-out-' + hashlib.sha256(str(folder).encode()).hexdigest()[:12]
        process = run([args.exe.resolve(),'--automation-background','--automation-settings-namespace',namespace,
            '--smoke-player',media,'--smoke-output',report],folder/'process.json',env)
        data = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
        summary, automation = data.get('summary',{}), data.get('automation',{})
        passed = (process['passed'] and summary.get('pass',0)>0 and summary.get('fail')==0 and summary.get('skip',0)==0
            and automation.get('background') is True and automation.get('visiblePlatformWindows')==0
            and data.get('coverage',{}).get('complete') is True)
        case = dict(name=name,passed=passed,summary=summary,report=str(report))
        cases.append(case)
        print(json.dumps(case),flush=True)
    result = dict(passed=all(case['passed'] for case in cases),cases=cases,backgroundMode=True,
        exeSha256=hashlib.sha256(args.exe.read_bytes()).hexdigest())
    (output/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
