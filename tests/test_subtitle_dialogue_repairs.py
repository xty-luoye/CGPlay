"""R62/R63/R99: production adapters, loopback routing and native display rules."""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage12'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/cgplay/ai' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def record(name, report):
    visible = []
    callback = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def visit(window, _):
        pid = ctypes.c_ulong()
        ctypes.windll.user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == os.getpid() and ctypes.windll.user32.IsWindowVisible(window):
            visible.append(window)
        return True
    ctypes.windll.user32.EnumWindows(callback(visit), 0)
    report.update(backgroundMode=True, visiblePlatformWindows=len(visible), passed=not visible)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / (name + '.json')).write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    if visible:
        raise AssertionError('Visible test window')


class SubtitleDialogueRepairs(unittest.TestCase):
    def test_preserve_repeated_dialogue_by_interval_and_line(self):
        asr = module('local_transcribe')
        translator = module('local_text_translate')
        rows = []
        inputs = [SimpleNamespace(text='Hello', start=0, end=1),
                  SimpleNamespace(text='Hello', start=0, end=1),  # identical record only
                  SimpleNamespace(text='Hello', start=3, end=4),
                  SimpleNamespace(text='Hello', start=3.5, end=4.5),
                  SimpleNamespace(text='Goodbye', start=6, end=7)]
        class Model:
            def transcribe(self, *args, **kwargs):
                return iter(inputs), SimpleNamespace(language='en')
        for loader, value, adapter in [('_load_faster_whisper_model', Model(), asr._transcribe_with_faster_whisper),
                                       ('_load_whisperlive_source_model', (Model(), 'fixture'), asr._transcribe_with_whisperlive_source)]:
            with patch.object(asr, loader, return_value=value):
                result = adapter('not-read.wav', 'en', 'fixture')
            self.assertEqual([0, 3, 3.5, 6], [item['start'] for item in result['segments']])
            self.assertEqual('Hello\nHello\nHello\nGoodbye', result['text'])
            rows.append(result)
        with patch.object(translator, '_run_model', side_effect=lambda _, text: {'Hello': '你好', 'Goodbye': '再见'}[text]):
            result = translator._translate_text('Hello\nHello\nGoodbye', 'en', 'zh')
        self.assertEqual('你好\n你好\n再见', result['translatedText'])
        record('dialogue', dict(modelOutputsInjected=True, realModelsLoaded=False, realApiCalls=0, transcription=rows, translation=result))

    def test_translation_root_version_proxy_and_full_routes(self):
        worker = module('long_window_asr_worker')
        paths = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_POST(self):
                paths.append(self.path)
                self.rfile.read(int(self.headers['Content-Length']))
                valid = self.path in ('/v1/chat/completions', '/compatible-mode/v1/chat/completions')
                body = json.dumps({'choices': [{'message': {'content': '夹具翻译'}}]}).encode()
                self.send_response(200 if valid else 404)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        rows = []
        try:
            base = 'http://127.0.0.1:' + str(server.server_port)
            for suffix in ('', '/', '/v1', '/v1/', '/compatible-mode/v1', '/compatible-mode/v1/', '/v1/chat/completions', '/v1/chat/completions/'):
                with patch.object(worker, '_translation_base_url', return_value=base + suffix), \
                     patch.object(worker, '_translation_api_key', return_value='fixture-key'), \
                     patch.object(worker, '_translation_model', return_value='fixture-model'):
                    result = worker._translate_with_openai_compatible('Hello', 'en', 'zh')
                self.assertTrue(result['success'], result)
                rows.append(dict(configuredSuffix=suffix, requestedPath=paths[-1]))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        record('routes', dict(loopbackOnly=True, realModelCalls=0, cases=rows))

    def test_native_display_rule_accepts_multilingual_affirmations(self):
        OUT.mkdir(parents=True, exist_ok=True)
        product = (ROOT / 'src/ui/app/ApplicationEnhancement.cpp').read_text(encoding='utf-8-sig')
        code = COMMON + r'''
#include <QSet>
struct GeneratedSubtitleCue{QString sourceText,translatedText;};
''' + function(product, 'bool generatedSubtitleCueHasDisplayableTranslation(') + r'''
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject result;
for(auto cue:{GeneratedSubtitleCue{QString::fromUtf8("はい"),QString::fromUtf8("好的")},
GeneratedSubtitleCue{QString::fromUtf8("好"),QString::fromUtf8("好")},
GeneratedSubtitleCue{QString::fromUtf8("네"),QString::fromUtf8("行")},
GeneratedSubtitleCue{"yes",QString::fromUtf8("好的")},
GeneratedSubtitleCue{QString::fromUtf8("こんにちは"),QString::fromUtf8("你好")}}){result[cue.sourceText+"->"+cue.translatedText]=generatedSubtitleCueHasDisplayableTranslation(cue);}
result["emptyAffirmationSourceRejected"]=!generatedSubtitleCueHasDisplayableTranslation({"",QString::fromUtf8("好的")});
result["punctuationOnlySourceRejected"]=!generatedSubtitleCueHasDisplayableTranslation({"...",QString::fromUtf8("好的")});
result["emptyTranslationRejected"]=!generatedSubtitleCueHasDisplayableTranslation({"hello",""});
result["fillerPairStillRejected"]=!generatedSubtitleCueHasDisplayableTranslation({"uh",QString::fromUtf8("嗯")});
return finish(result);}
'''
        (OUT / 'display.cpp').write_text(code, encoding='utf-8')
        (OUT / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(SubtitleDialogue LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(display display.cpp)
target_compile_options(display PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
target_link_libraries(display PRIVATE Qt6::Core user32)
''', encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(OUT), '-B', str(OUT / 'build'), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            self.assertEqual(0, result.returncode, (result.stdout + result.stderr).decode(errors='replace')[-3000:])
        exe = OUT / 'build/Release/display.exe'
        deploy_runtime(exe, QT, (ROOT / 'build_win_full/bin/Release',))
        result = subprocess.run([str(exe)], capture_output=True, creationflags=NO_WINDOW, timeout=15)
        (OUT / 'display.log').write_bytes(result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode().splitlines()[-1])
        (OUT / 'display.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
        self.assertTrue(report['passed'], report)


if __name__ == '__main__':
    unittest.main()
