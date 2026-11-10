"""Actual evaluator CLI, fixed checkpoint decisions and split-source audit contracts."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage23'
sys.path.insert(0,str(ROOT/'tools/cgplay/ai'))
def load(name):
    spec=importlib.util.spec_from_file_location(name, ROOT/'tools/cgplay/ai'/f'{name}.py')
    module=importlib.util.module_from_spec(spec)
    sys.modules[name]=module
    spec.loader.exec_module(module)
    return module

gate=load('headless_translation_gate')

class FinalEvidenceRepairs(unittest.TestCase):
    def test_actual_final_cli(self):
        quick=OUT/'quick.vtt'; final=OUT/'final.vtt'; output=OUT/'accuracy.json'
        good='WEBVTT\n\n00:00:10.000 --> 00:00:12.000\n这是正确的字幕\n\n'
        quick.write_text(good,encoding='utf-8')
        cases=[('correct',good,0,'这是正确的字幕'),
               ('incorrect',good.replace('这是正确的字幕','完全错误的内容'),2,'完全错误的内容'),
               ('empty','WEBVTT\n\n',2,''),
               ('gap',good.replace('00:00:10','00:00:20').replace('00:00:12','00:00:22'),2,''),
               ('missing',None,2,'')]
        outcomes=[]
        for name,content,expected,text in cases:
            if final.exists(): final.unlink()
            if content is not None: final.write_text(content,encoding='utf-8')
            command=[sys.executable,str(ROOT/'tools/cgplay/ai/accuracy_v2_evaluator.py'),
                     '--reference',str(quick),'--quick',str(quick),'--final',str(final),'--output',str(output)]
            p=subprocess.run(command,capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(expected,p.returncode,(name,p.stderr))
            report=json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual('actual-sidecar',report['finalTrackMode'])
            self.assertEqual(text,report['rows'][0]['finalDisplayedText'])
            self.assertEqual(text,report['cueDiagnostics'][0]['finalDisplayedText'])
            outcomes.append({'case':name,'exitCode':p.returncode,'finalText':text})
        command.remove('--final'); command.remove(str(final))
        p=subprocess.run(command,capture_output=True,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(0,p.returncode)
        outcomes.append({'case':'inferred','exitCode':p.returncode})
        (OUT/'accuracy_cases.json').write_text(json.dumps(outcomes,ensure_ascii=False,indent=2),encoding='utf-8')

    def test_no_subtitle_requires_evidence(self):
        media=Path('phase_test.mp4')
        cases=[('missing',[],[],False),
               ('leaked',[{'start':23,'end':24.5,'finalDisplayedText':'误显示'}],[],False),
               ('blank',[{'start':23,'end':24.5,'finalDisplayedText':''}],[],True),
               ('unknown-field',[{'start':23,'end':24.5}],[],False),
               ('suppressed',[],[{'start':23,'end':24.5,'finalDisplayedText':'','staleCueCleared':True}],True),
               ('conflict',[{'start':23,'end':24.5,'finalDisplayedText':'仍然显示'}],[{'start':23,'end':24.5,'finalDisplayedText':''}],False)]
        results=[]
        for name,cues,samples,passed in cases:
            first=gate.fixed_checkpoint_report(media,cues,samples)[0]
            self.assertEqual(passed,first['pass'],name)
            if name=='missing': self.assertFalse(first['staleCueCleared'])
            if name in ('leaked','conflict'): self.assertTrue(first['finalDisplayedText'])
            results.append({'case':name,**first})
        (OUT/'checkpoint_cases.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')

    def test_split_sources_and_missing_inputs(self):
        for name in ('static_translation_policy_parity_audit','static_translation_mock_fallback_audit'):
            module=load(name); report=module.audit(ROOT)
            self.assertTrue(report['allPassed'],name)
            fixture=OUT/name; fixture.mkdir(exist_ok=True)
            for value in report['files'].values():
                for source in value if isinstance(value,list) else [value]:
                    source=Path(source); dest=fixture/source.relative_to(ROOT)
                    dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(source,dest)
            self.assertTrue(module.audit(fixture)['allPassed'])
            (fixture/'src/ui/app/ApplicationInternal.h').unlink()
            failed=module.audit(fixture)
            self.assertFalse(failed['allPassed'])
            self.assertFalse(failed['checks']['requiredApplicationSourcesPresent'])
        qwen=load('qwen_provider_smoke')
        with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(0,qwen.main())
        (OUT/'result.json').write_text(json.dumps({'passed':True,'background':True,'visiblePlatformWindows':0,'boundary':'Python-only evaluators and static source checks; no Qt or live provider'}),encoding='utf-8')

if __name__=='__main__': unittest.main()

