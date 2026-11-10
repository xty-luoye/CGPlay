"""R61/R69/R75/R81: preserve legitimate dialogue, ordered sources, explicit local units."""
import ctypes
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT, function

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage20'
sys.path.insert(0, str(ROOT / 'tools/cgplay/ai'))
import translate_visual_source_cache as translator

NATIVE = r'''
#include "ai/TranslationFusionController.h"
#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
using namespace cgplay;
static QHash<QString,QVector<GeneratedSubtitleCue>>parsed;
namespace cgplay{QVector<GeneratedSubtitleCue>SubtitleGenerationService::readSubtitleFile(const QString&p){return parsed.value(p);}}
HELPERS
QJsonObject parse(QString normalized){QJsonArray actions;auto appendAction=[&](QString type,QJsonObject action){action["type"]=type;actions.append(action);};
PARSE
return actions.first().toObject();}
double apply(QJsonObject action){struct Playback{float volume=.5f;float getVolume(){return volume;}void setVolume(float v){volume=v;}}p;auto*playback=&p;
APPLY
return p.volume;}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;bool changed=false;
report["validGovernmentTermPreserved"]=semanticRepairText(QString::fromUtf8("他在国家机关工作"),&changed)==QString::fromUtf8("他在国家机关工作")&&!changed;
report["validLeadingHanPreserved"]=stripLeadingIsolatedHanOcrNoise(QString::fromUtf8("请 注意安全"))==QString::fromUtf8("请 注意安全");
report["ordinaryDialoguePreserved"]=stripLeadingIsolatedHanOcrNoise(QString::fromUtf8("今天天气很好"))==QString::fromUtf8("今天天气很好");
for(int v:{0,1,2,50,100})for(QString suffix:{QString(),QString("%")}){double value=apply(parse(QString::fromUtf8("音量 %1%2").arg(v).arg(suffix)));report[QString("localVolume%1%2").arg(v).arg(suffix)]=std::abs(value-v/100.0)<.00001;}
const QDir dir(QString::fromUtf8(argv[1]));QString quick=dir.filePath("quick.zh.vtt"),online=dir.filePath("test.online.translated.zh.vtt"),ocr=dir.filePath("test.ocr.translated.zh.vtt"),peer=dir.filePath("peer.online.translated.zh.vtt");
GeneratedSubtitleCue cue;cue.index=1;cue.startSeconds=0;cue.endSeconds=2;cue.translatedText=QString::fromUtf8("在线原文");parsed[quick]={cue};parsed[online]={cue};cue.translatedText=QString::fromUtf8("不同文本");parsed[ocr]={cue};parsed[peer]={cue};
for(const auto&path:{quick,online,ocr,peer}){QFile f(path);f.open(QIODevice::WriteOnly);f.write("fixture");}
TranslationFusionController controller;TranslationManualFusionRequest request;request.quickTranslatedVttPath=quick;request.outputEnhancedVttPath=dir.filePath("enhanced.vtt");request.manualRequested=true;request.candidateCachePaths={online,ocr};auto result=controller.writeManualFusionCache(request);
QFile f(request.outputEnhancedVttPath);f.open(QIODevice::ReadOnly);const auto output=f.readAll();f.close();
report["higherReliabilityNotReversedByDifferentText"]=result.success&&result.primaryAcceptedSourceType=="online-subtitle"&&QString::fromUtf8(output).contains(QString::fromUtf8("在线原文"));
request.candidateCachePaths={online,peer};result=controller.writeManualFusionCache(request);f.open(QIODevice::ReadOnly);report["sameSourceTiePrefersDistinctText"]=result.success&&QString::fromUtf8(f.readAll()).contains(QString::fromUtf8("不同文本"));f.close();
request.candidateCachePaths={ocr};result=controller.writeManualFusionCache(request);report["ocrOnlyStillWorks"]=result.success&&result.primaryAcceptedSourceType=="hard-sub-ocr";return finish(report);}
'''


class SubtitlePolicyBoundaryRepairs(unittest.TestCase):
    def test_native_dialogue_order_and_local_volume(self):
        source = (ROOT / 'src/services/ai/TranslationEnhancementSupport.cpp').read_text(encoding='utf-8-sig')
        helpers = function(source, 'QString semanticRepairText(') + '\n' + function(source, 'QString stripLeadingIsolatedHanOcrNoise(')
        control = (ROOT / 'src/ui/app/AIAgentWorkspaceControl.cpp').read_text(encoding='utf-8-sig')
        start = control.index('    QRegularExpression volumePattern(')
        parse = control[start:control.index('    const bool explicitPlayCommand', start)]
        start = control.index('            double volume = action.value(')
        apply = control[start:control.index('            results <<', start)]
        build_and_run(OUT / 'native', NATIVE.replace('HELPERS', helpers).replace('PARSE', parse).replace('APPLY', apply),
                      sources=['src/services/ai/TranslationFusionController.cpp'])

    def test_translator_empty_partial_complete_reports(self):
        directory = OUT / 'python'; directory.mkdir(parents=True, exist_ok=True)
        source = directory / 'source.vtt'
        source.write_text('WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nThe elevator is broken.\n\n00:00:03.000 --> 00:00:04.000\nA train is coming.\n\n', encoding='utf-8')
        reports = {}
        for name, values in [('empty', ['', '']), ('partial', ['电梯坏了。', '']), ('complete', ['电梯坏了。', '火车来了。'])]:
            output = directory / (name + '.vtt'); report = directory / (name + '.json')
            with patch.object(translator, '_reg_value', return_value=''), patch.object(translator, '_read_credential', return_value=''), \
                 patch.object(translator, '_translate_rule_based', return_value=''), patch.object(translator, '_translate_local', side_effect=values):
                code = translator.main(['--source', str(source), '--output', str(output), '--report', str(report)])
            result = json.loads(report.read_text(encoding='utf-8'))
            self.assertEqual(name == 'complete', result['success'])
            self.assertEqual(name == 'complete', code == 0)
            self.assertEqual(sum(bool(t) for t in values), result['translatedCueCount'])
            self.assertEqual(sum(bool(t) for t in values), result['batches'][-1]['translatedCount'])
            reports[name] = {'exitCode': code, 'report': result}
        # This Python-only branch neither creates GUI objects nor starts child processes.
        visible = []
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def inspect(window, _):
            pid = ctypes.c_ulong(); ctypes.windll.user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == os.getpid() and ctypes.windll.user32.IsWindowVisible(window):
                visible.append(int(window))
            return True
        callback = callback_type(inspect); ctypes.windll.user32.EnumWindows(callback, 0)
        self.assertEqual([], visible)
        (OUT / 'python.json').write_text(json.dumps({'cases': reports, 'backgroundMode': True, 'visiblePlatformWindows': len(visible),
            'boundaries': ['registry and credential lookup injected', 'local translator output injected'], 'passed': True}, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    unittest.main()
