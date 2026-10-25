"""Draft test payload; apply to tests/test_annotation_card_timecode.py later.

Compiles the real ReviewPanel in a hidden Qt harness. Does not run automatically.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = Path(os.environ.get('CGPLAY_SMALL_FIX_ARTIFACT_DIR', ROOT/'tests/artifacts/small_fixes_1_0_8'))/'annotation_cards'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

RUNNER = r'''
#include "annotation/ReviewPanel.h"
#include "annotation/AnnotationManager.h"
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QEvent>
#include <cmath>
#include <limits>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace cgplay {
// Only the annotation lookup dependency is isolated; the real ReviewPanel
// builds cards, formats rates, rebinds state and paints the image.
AnnotationItem* AnnotationManager::get(const QString&) { return nullptr; }
const AnnotationItem* AnnotationManager::get(const QString&) const { return nullptr; }
}
static int visibleWindows() {
    int result=0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        if(pid==GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
#endif
    return result;
}
int main(int argc,char**argv) {
    QApplication app(argc,argv);
    app.setProperty("cgplay.automationBackground",true);
    cgplay::ReviewPanel panel(nullptr);
    panel.resize(430,760);
    panel.show(); // offscreen platform only; native visibility is checked below
    cgplay::AnnotationItem note; note.id="fps-note"; note.frame=30;
    note.comment=QStringLiteral("真实媒体帧率批注检查");
    QJsonArray cases;
    auto check=[&](int frame,double fps,const QString& expected) {
        note.frame=frame;
        panel.refresh({note},frame,fps);
        app.processEvents();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        const auto labels=panel.findChildren<QLabel*>(QStringLiteral("annotationTimecode"));
        const QString actual=labels.size()==1?labels.front()->text():QString();
        const bool passed=labels.size()==1 && actual==expected;
        cases.push_back(QJsonObject{{"frame",frame},{"fps",std::isfinite(fps)?fps:0.0},
            {"expected",expected},{"actual",actual},{"passed",passed}});
        return passed;
    };
    bool ok=true;
    ok=check(30,30.0,QStringLiteral("00:00:01:00"))&&ok;
    // Same annotation data and frame; changing only FPS must rebuild the label.
    ok=check(30,60.0,QStringLiteral("00:00:00:30"))&&ok;
    ok=check(30,29.97,QStringLiteral("00:00:01:00"))&&ok;
    ok=check(24,23.976,QStringLiteral("00:00:01:00"))&&ok;
    ok=check(30,0.0,QStringLiteral("帧 30（帧率未就绪）"))&&ok;
    ok=check(30,std::numeric_limits<double>::quiet_NaN(),QStringLiteral("帧 30（帧率未就绪）"))&&ok;
    ok=check(30,-1.0,QStringLiteral("帧 30（帧率未就绪）"))&&ok;
    ok=check(30,30.0,QStringLiteral("00:00:01:00"))&&ok;
    const QString image=QDir(QString::fromLocal8Bit(argv[1])).filePath(QStringLiteral("annotation_card_30fps.png"));
    panel.layout()->activate();
    const bool captured=panel.grab().save(image);
    const int visible=visibleWindows();
    const bool background=qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen";
    QJsonObject result{{"cases",cases},{"backgroundMode",background},{"visiblePlatformWindows",visible},
        {"image",image},{"imageSaved",captured},{"passed",ok&&captured&&background&&visible==0}};
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return result["passed"].toBool()?0:1;
}
'''


class AnnotationCardTimecodeRuntime(unittest.TestCase):
    def test_real_cards_use_media_rate_and_rebind(self):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        source = ARTIFACT/'source'
        build = ARTIFACT/'build'
        source.mkdir(exist_ok=True)
        qt = os.environ.get('CGPLAY_TEST_QT_ROOT') or os.environ.get('QT_ROOT')
        if not qt:
            cache = (ROOT/'build_win_full/CMakeCache.txt').read_text(encoding='utf-8')
            match = re.search(r'^Qt6_DIR:[^=]*=(.+)$', cache, re.MULTILINE)
            self.assertIsNotNone(match, 'Set CGPLAY_TEST_QT_ROOT to the Qt installation used by this build')
            qt = str(Path(match.group(1).strip()).parents[2])
        (source/'runner.cpp').write_text(RUNNER, encoding='utf-8')
        (source/'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(CardRateRuntime LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n'
            'find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)\n'
            f'add_executable(card_rate_runner runner.cpp "{(ROOT/"src/features/annotation/ReviewPanel.cpp").as_posix()}" "{(ROOT/"src/features/annotation/ReviewPanel.h").as_posix()}")\n'
            f'target_include_directories(card_rate_runner PRIVATE "{(ROOT/"src").as_posix()}" "{(ROOT/"src/features").as_posix()}")\n'
            'target_compile_options(card_rate_runner PRIVATE /utf-8)\n'
            'target_link_libraries(card_rate_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)\n', encoding='utf-8')
        for name, args in (
            ('configure', ['cmake','-S',str(source),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_PREFIX_PATH='+qt]),
            ('build', ['cmake','--build',str(build),'--config','Release','--parallel','2'])):
            result = subprocess.run(args, capture_output=True, creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT/(name+'.log')).write_bytes(result.stdout+result.stderr)
            self.assertEqual(result.returncode, 0, (result.stdout+result.stderr)[-5000:])
        environment = os.environ.copy()
        environment['QT_QPA_PLATFORM'] = 'offscreen'
        environment['PATH'] = str(Path(qt)/'bin')+';'+environment.get('PATH','')
        result = subprocess.run([str(build/'Release/card_rate_runner.exe'),str(ARTIFACT)],
            env=environment, capture_output=True, creationflags=NO_WINDOW, timeout=20)
        (ARTIFACT/'runtime.log').write_bytes(result.stdout+result.stderr)
        self.assertEqual(result.returncode, 0, (result.stdout+result.stderr)[-5000:])
        report = json.loads(result.stdout.decode('utf-8').strip().splitlines()[-1])
        (ARTIFACT/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        self.assertTrue(report['passed'])
        self.assertEqual(report['visiblePlatformWindows'], 0)
        self.assertTrue(report['backgroundMode'])
        self.assertEqual(len(report['cases']), 8)
        self.assertTrue(all(case['passed'] for case in report['cases']))


if __name__ == '__main__':
    unittest.main()
