"""R32/R65 real Qt layout + production transition methods; R86 real QUrl."""
import json
import os
from pathlib import Path
import subprocess
import unittest

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage13'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

WINDOW = r'''
#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>
#include <QResizeEvent>
struct Viewport:QWidget {void setFrameView(bool){}};
struct Viewer:QWidget {Viewport*vp;bool chrome=true;Viewer(){vp=new Viewport;auto*l=new QVBoxLayout(this);l->setContentsMargins(0,0,0,0);l->addWidget(vp);}void setChromeVisible(bool b){chrome=b;}Viewport*viewport(){return vp;}};
class SecondaryWindow:public QMainWindow{public:
Viewer*_viewer;QWidget*_timeline;QTimer*_fullscreenChromeTimer=nullptr;
bool _fullscreenTimeline=true,_fullscreenViewerChrome=true,_fullscreenChromeVisible=false,_fullscreenCursorHidden=false,_fullscreenTimelineOverlay=false;
quint64 _fullscreenGeneration=0;
SecondaryWindow(){setAttribute(Qt::WA_DontShowOnScreen);setAttribute(Qt::WA_ShowWithoutActivating);auto*c=new QWidget;auto*l=new QVBoxLayout(c);l->setContentsMargins(0,0,0,0);l->setSpacing(0);_viewer=new Viewer;_timeline=new QWidget;_timeline->setFixedHeight(101);l->addWidget(_viewer,1);l->addWidget(_timeline,0);setCentralWidget(c);resize(960,640);show();}
void _toggleFullScreen();void _showFullScreenChromeTemporarily();void _hideFullScreenChrome();void _setFullScreenChromeVisible(bool,bool=false);void _setFullScreenCursorHidden(bool);void _positionFullScreenTimeline();void resizeEvent(QResizeEvent*)override;
};
'''

WINDOW_MAIN = r'''
int main(int argc,char**argv){QApplication app(argc,argv);QJsonObject report;SecondaryWindow window;QCoreApplication::processEvents();
window._toggleFullScreen();QCoreApplication::processEvents();window.centralWidget()->layout()->activate();
const QSize hidden=window._viewer->viewport()->size();window._setFullScreenChromeVisible(true);QCoreApplication::processEvents();window.centralWidget()->layout()->activate();
report["overlayKeepsViewportSize"]=window.isFullScreen()&&hidden==window._viewer->viewport()->size()&&!window._timeline->isHidden();
report["timelineRemovedFromVideoLayout"]=window.centralWidget()->layout()->indexOf(window._timeline)==-1;
window.centralWidget()->resize(1100,720);window._positionFullScreenTimeline();
report["overlayTracksCentralGeometry"]=window._timeline->geometry().width()==window.centralWidget()->width()&&window._timeline->geometry().bottom()==window.centralWidget()->rect().bottom();
window._toggleFullScreen();QCoreApplication::processEvents();
report["exitRestoresLayoutAndCursor"]=!window.isFullScreen()&&window.centralWidget()->layout()->indexOf(window._timeline)==1&&!window._timeline->isHidden()&&!window._fullscreenCursorHidden&&window._viewer->chrome;
report["exitState"]=QJsonObject{{"full",window.isFullScreen()},{"index",window.centralWidget()->layout()->indexOf(window._timeline)},{"timelineHidden",window._timeline->isHidden()},{"cursorHidden",window._fullscreenCursorHidden},{"viewerChrome",window._viewer->chrome}};
bool stable=true;for(int i=0;i<5;++i){window._toggleFullScreen();window._toggleFullScreen();QCoreApplication::processEvents();stable=stable&&!window.isFullScreen()&&!window._timeline->isHidden()&&!window._fullscreenCursorHidden&&window._viewer->chrome;}
report["rapidEnterExitIgnoresStaleCallbacks"]=stable;
window._toggleFullScreen();window._toggleFullScreen();window._toggleFullScreen();QCoreApplication::processEvents();
report["enterExitEnterKeepsNewestGeneration"]=window.isFullScreen()&&window._timeline->isHidden()&&window._fullscreenCursorHidden;
window._toggleFullScreen();QCoreApplication::processEvents();window.close();return finish(report);}
'''

URL_MAIN = r'''
#include "common/core/MediaStatusUrl.h"
#include <QUrlQuery>
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;
const QString id=QString::fromUtf8("任务/123?x=1&y=a+b%25#end");
for(const QString raw:{QString("https://example.invalid/tasks/{id}"),QString("https://example.invalid/tasks/%7Bid%7D"),QString("https://example.invalid/tasks/%7bid%7d")}){
const auto url=cgplay::mediaStatusUrl(raw,id);report[raw]=url.isValid()&&url.path(QUrl::FullyDecoded)=="/tasks/"+id&&url.query().isEmpty()&&url.fragment().isEmpty()&&url.toEncoded().contains("%2F123%3F");}
const auto queryUrl=cgplay::mediaStatusUrl("https://example.invalid/status?task={id}&mode=poll",id);const QUrlQuery query(queryUrl);
report["queryIdIsOneEncodedValue"]=query.queryItemValue("task",QUrl::FullyDecoded)==id&&query.queryItems().size()==2&&queryUrl.fragment().isEmpty();
const QString saved=cgplay::normalizeMediaStatusTemplate(QUrl("https://example.invalid/tasks/{id}").toString(QUrl::FullyEncoded));
report["saveLoadUsesLiteralPlaceholder"]=saved.contains("{id}")&&cgplay::mediaStatusUrl(saved,"task-123").path()=="/tasks/task-123";
report["missingPlaceholderRejected"]=cgplay::mediaStatusUrl("https://example.invalid/tasks","123").isEmpty();
report["emptyIdRejected"]=cgplay::mediaStatusUrl("https://example.invalid/tasks/{id}",{}).isEmpty();return finish(report);}
'''


class SecondaryFullscreenRepairs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        product = (ROOT / 'src/ui/app/SecondaryWindow.cpp').read_text(encoding='utf-8-sig')
        methods = '\n'.join(function(product, signature) for signature in (
            'void SecondaryWindow::_toggleFullScreen(', 'void SecondaryWindow::_showFullScreenChromeTemporarily(',
            'void SecondaryWindow::_hideFullScreenChrome(', 'void SecondaryWindow::_setFullScreenChromeVisible(',
            'void SecondaryWindow::_setFullScreenCursorHidden(', 'void SecondaryWindow::_positionFullScreenTimeline(',
            'void SecondaryWindow::resizeEvent('))
        (OUT / 'window.cpp').write_text(COMMON + WINDOW + methods + WINDOW_MAIN, encoding='utf-8')
        (OUT / 'url.cpp').write_text(COMMON + URL_MAIN, encoding='utf-8')
        (OUT / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
project(SecondaryFullscreenRepairs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
foreach(target window url)
add_executable(${{target}} ${{target}}.cpp)
target_include_directories(${{target}} PRIVATE "{ROOT.as_posix()}/src")
target_compile_options(${{target}} PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
target_link_libraries(${{target}} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets user32)
endforeach()
''', encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(OUT), '-B', str(OUT / 'build'), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                raise AssertionError((result.stdout + result.stderr).decode(errors='replace')[-4000:])
        deploy_runtime(OUT / 'build/Release/window.exe', QT, (ROOT / 'build_win_full/bin/Release',))

    def probe(self, name):
        result = subprocess.run([str(OUT / ('build/Release/' + name + '.exe'))], capture_output=True, creationflags=NO_WINDOW, timeout=30,
                                env=dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(QT / 'plugins')))
        (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode().splitlines()[-1])
        (OUT / (name + '.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
        self.assertTrue(report['passed'], report)

    def test_secondary_fullscreen_overlay_and_rapid_transitions(self):
        self.probe('window')

    def test_media_status_template_and_escaped_task_ids(self):
        self.probe('url')


if __name__ == '__main__':
    unittest.main()
