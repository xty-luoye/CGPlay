"""R78/R82/R97/R98/R100/R105: failures must propagate through diagnostic wrappers."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT, function

sys.path.insert(0, str(ROOT / 'tools'))
import player_function_smoke as smoke
import stability_suite as stability

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage22'
NATIVE = r'''
#include <QSaveFile>
#include <QDebug>
WRITER
int commitReport(const QString&outputPath,bool pass){QJsonObject report{{"pass",pass}};int exitCode=0;
REPORT_BRANCH
return exitCode;}
bool childPassed(QJsonObject step){
CHILD_BRANCH
return childPassed;}
QString switchStatus(int requested,int completed,int fail,bool hasWindow){struct{int requested,completed,fail;}stats{requested,completed,fail};struct Secondary{bool*viewerWidget(){return &ready;}bool ready=true;}window;auto*secondary=hasWindow?&window:nullptr;QJsonArray assertions;QJsonObject windowSwitchSection;
auto addAssertion=[&](QString,bool passed,QString,QJsonObject){assertions.append(QJsonObject{{"status",passed?"PASS":"FAIL"}});};
SWITCH_BRANCH
return assertions.first().toObject()["status"].toString();}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject result;const QDir dir(QString::fromUtf8(argv[1]));const QString good=dir.filePath("valid.json");
result["successfulServiceAndReportReturnsZero"]=commitReport(good,true)==0;
result["serviceFailureReturnsNonzero"]=commitReport(good,false)==2;
result["reportDirectoryReturnsWriteFailure"]=commitReport(dir.absolutePath(),true)==4;
HANDLE reader=CreateFileW(reinterpret_cast<LPCWSTR>(good.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
result["reportCommitFailureReturnsNonzero"]=reader!=INVALID_HANDLE_VALUE&&commitReport(good,true)==4;if(reader!=INVALID_HANDLE_VALUE)CloseHandle(reader);
QJsonObject goodChild{{"summary",QJsonObject{{"fail",0}}},{"frames",QJsonArray{QJsonObject{}}}};
result["childPassPropagates"]=childPassed(goodChild);goodChild["summary"]=QJsonObject{{"fail",1}};result["childFailurePropagates"]=!childPassed(goodChild);
goodChild["summary"]=QJsonObject{{"fail",0}};goodChild["frames"]=QJsonArray{};result["missingChildEvidenceFails"]=!childPassed(goodChild)&&!childPassed({});
result["missingSecondaryFails"]=switchStatus(10,0,0,false)=="FAIL";
result["incompleteSwitchLoopFails"]=switchStatus(10,3,0,true)=="FAIL";
result["completedSwitchLoopPasses"]=switchStatus(10,10,0,true)=="PASS";
result["switchFailureFails"]=switchStatus(10,10,1,true)=="FAIL";
result["zeroRequestExplicitlySkipped"]=switchStatus(0,0,0,false)=="SKIP";return finish(result);}
'''


class DiagnosticResultRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/ui/app/ApplicationRuntimeSupport.cpp').read_text(encoding='utf-8-sig')
        writer = function(source, 'bool writeJsonObjectFile(')
        source = (ROOT / 'src/ui/app/ApplicationRuntimeAutomation.cpp').read_text(encoding='utf-8-sig')
        end = source.index('        QCoreApplication::quit();', source.index('[QwenSmoke] Failed to commit report:'))
        start = source.rindex('        if (!writeJsonObjectFile(outputPath, report))', 0, end)
        branch = source[start:end]
        source = (ROOT / 'src/ui/app/MainWindowAutomationSubtitles.cpp').read_text(encoding='utf-8-sig')
        start = source.index('        const QJsonObject childSummary =')
        child = source[start:source.index('        addAssertion(', start)]
        source = (ROOT / 'src/ui/app/MainWindowAutomation.cpp').read_text(encoding='utf-8-sig')
        switch = function(source, 'if (stats.requested == 0)')
        end = source.index('        }\n    }\n\n    QJsonObject pluginReloadSection', source.index(switch)) + len('        }')
        start = source.index(switch)
        switch = source[start:end]
        (OUT / 'noop.cpp').write_text('int main(){return 0;}\n', encoding='utf-8')
        build_and_run(OUT, NATIVE.replace('WRITER', writer).replace('REPORT_BRANCH', branch).replace('CHILD_BRANCH', child).replace('SWITCH_BRANCH', switch),
                      extra_cmake='add_executable(noop "' + (OUT / 'noop.cpp').as_posix() + '")\n')

    def test_python_smoke_requires_fresh_report(self):
        directory = OUT / 'python'; directory.mkdir(exist_ok=True)
        output = directory / 'smoke.json'
        good = {'summary': {'fail': 0, 'manual': 0}, 'coverage': {'complete': True}, 'automation': {'background': True, 'visiblePlatformWindows': 0}}
        output.write_text(json.dumps(good), encoding='utf-8')
        with patch.object(sys, 'argv', ['smoke', '--output', str(output)]), patch.object(smoke, 'run_cmd', return_value=(0, '', '')), contextlib.redirect_stdout(io.StringIO()):
            self.assertNotEqual(0, smoke.main())
        self.assertFalse(output.exists())
        self.assertTrue(list(directory.glob('smoke.json.previous.*')))
        def fresh(*args):
            output.write_text(json.dumps(good), encoding='utf-8')
            return 0, '', ''
        with patch.object(sys, 'argv', ['smoke', '--output', str(output)]), patch.object(smoke, 'run_cmd', side_effect=fresh), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, smoke.main())

    def test_benchmark_freshness_and_rate_gate(self):
        path = OUT / 'benchmark.json'; path.write_text('{}', encoding='utf-8')
        with patch.object(stability, 'run_cmd', return_value=(0, '', '', .01)):
            benchmark = stability.run_benchmark(Path('fixture.exe'), Path('fixture.mp4'), path, True)
        self.assertFalse(benchmark['ok'])
        self.assertTrue(list(OUT.glob('benchmark.json.previous.*')))
        for rate, expected in [(True, True), (False, False), (None, False)]:
            report = {'playback_stats': {'avg_fps': 5}, 'render_fps': 5}
            if rate is not None:
                report['playbackRatePass'] = rate
            self.assertEqual(expected, stability.playback_rate_passed({'ok': True, 'report': report}))
        self.assertFalse(stability.playback_rate_passed({'ok': False, 'report': {'playbackRatePass': True}}))

    def test_powershell_old_report_and_writer_failure(self):
        directory = OUT / 'powershell'; directory.mkdir(exist_ok=True)
        output = directory / 'stale.json'; output.write_text('{"summary":{"fail":0}}', encoding='utf-8')
        media = directory / 'media.fixture'; media.write_bytes(b'fixture')
        noop = OUT / 'build/Release/noop.exe'
        no_window = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        command = ['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(ROOT / 'tools/run_player_smoke_one_click.ps1'),
                   '-ExePath', str(noop), '-MediaPath', str(media), '-ReportPath', str(output)]
        result = subprocess.run(command, capture_output=True, creationflags=no_window, timeout=15)
        (directory / 'stale.log').write_bytes(result.stdout + result.stderr)
        self.assertNotEqual(0, result.returncode)
        self.assertFalse(output.exists())
        source = (ROOT / 'tools/cgplay/ai/run_translation_acceptance.ps1').read_text(encoding='utf-8-sig')
        tail = source[source.index('$reportExit = $LASTEXITCODE'):]
        reports = []
        for name, writer_code, headless_code, files, expected in [('writer_failure', 7, 0, False, 7), ('missing_report', 0, 0, False, 4), ('headless_failure', 0, 9, True, 9), ('success', 0, 0, True, 0)]:
            fixture = directory / name; fixture.mkdir(exist_ok=True)
            if files:
                (fixture / 'ACCEPTANCE_REPORT.md').write_text('fixture')
                (fixture / 'acceptance_summary.json').write_text('{}')
            script = directory / (name + '.ps1')
            script.write_text('param([string]$ArtifactsDir)\n$LASTEXITCODE=' + str(writer_code) + '\n$headlessExit=' + str(headless_code) + '\n' + tail, encoding='utf-8-sig')
            result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(script), '-ArtifactsDir', str(fixture)],
                                    capture_output=True, creationflags=no_window, timeout=15)
            self.assertEqual(expected, result.returncode, result.stdout + result.stderr)
            reports.append({'case': name, 'exitCode': result.returncode})
        (OUT / 'wrappers.json').write_text(json.dumps({'cases': reports, 'backgroundMode': True, 'visiblePlatformWindows': 0,
            'boundaries': ['zero-exit native stub for absent smoke report', 'actual PowerShell completion branch with injected child exit status'], 'passed': True}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    unittest.main()
