"""Run after applying F03/F04, via the repository background/window wrapper."""
from pathlib import Path
import hashlib, json, os, re, subprocess, unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get('CGPLAY_TEST_ARTIFACTS', str(ROOT/'tests/artifacts/component_install_20261025')))
QT = Path(os.environ.get('CGPLAY_TEST_QT_ROOT', 'C:/QtClean/6.5.3/msvc2019_64'))

class ComponentInstallContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        extract = OUT/'extracted'
        extract.mkdir(exist_ok=True)
        review = (ROOT/'src/features/annotation/ReviewExport.cpp').read_text(encoding='utf-8-sig')
        declaration = re.search(r'^static thread_local QString s_lastExportError;[^\n]*', review, re.M).group(0)
        method = re.search(r'QString ReviewExport::lastError\(\)\s*\{[^}]*\}', review).group(0)
        (extract/'extracted_error_storage.h').write_text('#include <QString>\nnamespace cgplay {\n'
            'class ReviewExport { public: static QString lastError(); };\n'+declaration+'\n'+method+'\n}\n',encoding='utf-8')
        (OUT/'error_storage_source.json').write_text(json.dumps({'source':str(ROOT/'src/features/annotation/ReviewExport.cpp'),
            'sha256':hashlib.sha256((ROOT/'src/features/annotation/ReviewExport.cpp').read_bytes()).hexdigest(),
            'storageExtractedVerbatim':True,'methodExtractedVerbatim':True},indent=2),encoding='utf-8')
        cls.build = OUT/'build'
        cls.environment = os.environ.copy()
        cls.environment['PATH'] = str(QT/'bin')+os.pathsep+cls.environment.get('PATH','')
        cls.call('cmake','-S',ROOT/'tests/component_install_contracts','-B',cls.build,
            '-G','Visual Studio 17 2022','-A','x64','-DCMAKE_PREFIX_PATH='+str(QT),
            '-DCGPLAY_SOURCE_ROOT='+str(ROOT),'-DCGPLAY_TEST_EXTRACTED_DIR='+str(extract))
        cls.call('cmake','--build',cls.build,'--config','Release','--parallel','2')

    @classmethod
    def call(cls,*arguments):
        with (OUT/'commands.log').open('ab') as log:
            subprocess.run([str(a) for a in arguments],cwd=ROOT,env=cls.environment,stdout=log,stderr=log,
                check=True,creationflags=subprocess.CREATE_NO_WINDOW)

    def test_immutable_payloads_required_files_cancel_and_locked_old_dll(self):
        self.call(self.build/'Release/component_payload_probe.exe',OUT/'payload')
        report=json.loads((OUT/'payload/payload-store-report.json').read_text(encoding='utf-8'))
        self.assertTrue(report['passed'],report)
        self.assertGreaterEqual(len(report['assertions']),16)

    def test_simultaneous_export_error_storage_is_task_local(self):
        self.call(self.build/'Release/export_error_storage_probe.exe',OUT/'error_storage.json')
        report=json.loads((OUT/'error_storage.json').read_text(encoding='utf-8'))
        self.assertTrue(report['passed'],report)
        self.assertEqual('task-A-error',report['a'])
        self.assertEqual('task-B-error',report['b'])

    def test_gui_preflight_worker_and_error_handoff_contract(self):
        application=(ROOT/'src/ui/app/Application.cpp').read_text(encoding='utf-8-sig')
        review=(ROOT/'src/features/annotation/ReviewExport.cpp').read_text(encoding='utf-8-sig')
        worker=review.split('static bool exportWithPython(',1)[1].split('// Public API',1)[0]
        self.assertNotIn('ensureComponent(',worker)
        self.assertNotIn('activeWindow()',worker)
        self.assertNotIn('QMessageBox',worker)
        self.assertGreaterEqual(application.count('prepareVideoExportRuntime(this, &exportRuntime'),2)
        self.assertGreaterEqual(application.count('return qMakePair(succeeded, succeeded ? QString{} : ReviewExport::lastError())'),2)
        self.assertGreaterEqual(application.count('QString detail = exportResult.second'),2)
        self.assertNotIn('QString detail = ReviewExport::lastError()',application)

    def test_all_changed_runtime_consumers_keep_job_snapshot(self):
        expected={
            'src/ui/app/ApplicationEnhancement.cpp':['schedulerRequest.runtime = captureComponentRuntimePaths()',
                'request.runtime = captureComponentRuntimePaths()'],
            'src/ui/app/ApplicationSubtitles.cpp':['request.runtime = captureComponentRuntimePaths()'],
            'src/services/ai/SubtitleGenerationService.cpp':['request.runtime.ffmpegExecutable',
                'subtitleFallbackRequest.runtime = request.runtime'],
            'src/services/ai/TranslationEnhancementScheduler.cpp':['defaultOcrHelperPath(request.runtime)',
                'defaultLongAsrHelperPath(request.runtime)','&translateError, request.runtime'],
        }
        for path,needles in expected.items():
            source=(ROOT/path).read_text(encoding='utf-8-sig')
            for needle in needles:self.assertIn(needle,source,path)

if __name__ == '__main__':unittest.main()
