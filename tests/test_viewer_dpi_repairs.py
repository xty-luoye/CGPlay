"""R70: real Qt DPR, production mapping/input code and upstream fit formula."""
import json
import os
from pathlib import Path
import subprocess
import unittest

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage14'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

STUBS = r'''
#include <QApplication>
#include <QWidget>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <memory>
#include <cmath>
#include <algorithm>
namespace ftk {
struct V2I {int x=0,y=0;V2I()=default;V2I(int a,int b):x(a),y(b){}bool operator==(const V2I&)const=default;};
struct Size2I {int w=0,h=0;bool isValid()const{return w>0&&h>0;}};
struct Box2I {int w_=0,h_=0;Box2I()=default;Box2I(int,int,int w,int h):w_(w),h_(h){}Size2I size()const{return {w_,h_};}};
}
template<class T>struct Value {T value{};bool setIfChanged(const T&v){bool changed=!(value==v);value=v;return changed;}};
class Viewport {public:
struct P {std::shared_ptr<Value<std::pair<ftk::V2I,double>>>viewPosZoom=std::make_shared<Value<std::pair<ftk::V2I,double>>>();std::shared_ptr<Value<ftk::V2I>>viewPos=std::make_shared<Value<ftk::V2I>>();std::shared_ptr<Value<double>>zoom=std::make_shared<Value<double>>();} _p;
ftk::Box2I geometry;ftk::Size2I image{200,100};bool framed=true;
const ftk::Box2I&getGeometry()const{return geometry;}ftk::Size2I _getRenderSize()const{return image;}
void _frameView();void setGeometry(ftk::Box2I b){geometry=b;}
const ftk::V2I&getViewPos()const{return _p.viewPos->value;}double getZoom()const{return _p.zoom->value;}bool hasFrameView()const{return framed;}
void setViewPosAndZoom(ftk::V2I pos,double z){_p.viewPos->value=pos;_p.zoom->value=z;framed=false;}
};
#define FTK_P() auto& p=_p
FRAME_VIEW
class TlViewport:public QWidget {public:
struct P{std::shared_ptr<Viewport>viewport=std::make_shared<Viewport>();std::shared_ptr<Viewport>window=viewport;qreal geometryDevicePixelRatio=0;bool hasLastEmittedViewTransform=false;};std::unique_ptr<P>_p=std::make_unique<P>();
qreal testDpr=0;qreal devicePixelRatioF()const{return testDpr>0?testDpr:QWidget::devicePixelRatioF();}
const ftk::V2I&viewPos()const{return _p->viewport->getViewPos();}double zoom()const{return _p->viewport->getZoom();}
void _emitViewPosAndZoomChangedIfNeeded(ftk::V2I,double){}
void setFrameView(bool b){_p->viewport->framed=b;}void setViewPosAndZoom(ftk::V2I p,double z){_p->viewport->setViewPosAndZoom(p,z);}
void setZoom(double z,ftk::V2I focus={}){const auto old=viewPos();const double ratio=z/zoom();setViewPosAndZoom({qRound(focus.x+(old.x-focus.x)*ratio),qRound(focus.y+(old.y-focus.y)*ratio)},z);}
QPointF imageToWidget(double,double,int,int)const;QPointF widgetToImage(double,double,int,int)const;void _setGeometry();
};
class ViewerWidget:public QWidget {public:
struct P{TlViewport*viewport;bool autoFit=false,panning=false;QPoint panGlobalStart;QElapsedTimer panThrottle;};std::unique_ptr<P>_p=std::make_unique<P>();int _mediaW=200,_mediaH=100;
ViewerWidget(TlViewport*v){_p->viewport=v;}
QPointF imageToWidget(const QPointF&,int,int)const;QPointF widgetToImage(const QPointF&,int,int)const;QRectF visibleVideoRect()const;QPointF viewPos()const;double zoom()const;
void setZoom(double);void zoom1to1();bool _applyWheelZoom(QWheelEvent*,const QPoint&);void mouseMoveEvent(QMouseEvent*)override;
};
'''

MAIN = r'''
bool closePoint(QPointF a,QPointF b,double epsilon=.01){return std::abs(a.x()-b.x())<epsilon&&std::abs(a.y()-b.y())<epsilon;}
int main(int argc,char**argv){QApplication app(argc,argv);TlViewport native;native.resize(500,250);native._setGeometry();native._p->viewport->_frameView();ViewerWidget viewer(&native);QJsonObject report;
report["devicePixelRatio"]=native.devicePixelRatioF();
report["sourceCenterMapsToLogicalCenter"]=closePoint(viewer.imageToWidget({100,50},200,100),{250,125});
report["logicalClickStoresSourcePixel"]=closePoint(viewer.widgetToImage({250,125},200,100),{100,50});
report["visibleRectUsesLogicalUnits"]=viewer.visibleVideoRect()==QRectF(0,0,500,250);
report["mapperZoomIsLogical"]=std::abs(viewer.zoom()-2.5)<.01&&viewer.viewPos()==QPointF(0,0);
viewer.setZoom(1.5);report["manualZoomMatchesGetter"]=std::abs(viewer.zoom()-1.5)<.01;
viewer.zoom1to1();report["oneToOneIsLogical"]=std::abs(viewer.zoom()-1.0)<.01;
const QPoint focus(75,45);const auto sourceBefore=native.widgetToImage(focus.x(),focus.y(),200,100);
QWheelEvent wheel(QPointF(focus),QPointF(focus),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
viewer._applyWheelZoom(&wheel,focus);report["wheelKeepsLogicalFocus"]=closePoint(sourceBefore,native.widgetToImage(focus.x(),focus.y(),200,100),1.0);
const auto topLeft=viewer.imageToWidget({0,0},200,100);viewer._p->panning=true;viewer._p->panGlobalStart={0,0};
QMouseEvent move(QEvent::MouseMove,QPointF(20,10),QPointF(20,10),Qt::NoButton,Qt::MiddleButton,Qt::NoModifier);viewer.mouseMoveEvent(&move);
report["panUsesLogicalDelta"]=closePoint(viewer.imageToWidget({0,0},200,100)-topLeft,{20,10},.01);
native.testDpr=1;native._setGeometry();native.setViewPosAndZoom({30,20},2);const auto before=viewer.imageToWidget({100,50},200,100);
native.testDpr=2;native._setGeometry();report["manualTransformSurvivesDprChange"]=closePoint(before,viewer.imageToWidget({100,50},200,100))&&std::abs(viewer.zoom()-2)<.01&&closePoint(viewer.viewPos(),{30,20});
return finish(report);}
'''


class ViewerDpiRepairs(unittest.TestCase):
    def test_mapping_and_input_at_100_150_200_percent(self):
        OUT.mkdir(parents=True, exist_ok=True)
        viewport = (ROOT / 'src/ui/viewer/TlViewport.cpp').read_text(encoding='utf-8-sig')
        viewer = (ROOT / 'src/ui/viewer/ViewerWidget.cpp').read_text(encoding='utf-8-sig')
        upstream = Path('E:/rj/CGV/tlRender-main/lib/tlRender/UI/Viewport.cpp').read_text(encoding='utf-8-sig')
        code = COMMON + STUBS.replace('FRAME_VIEW', function(upstream, 'void Viewport::_frameView('))
        code += '\n'.join(function(viewport, signature) for signature in (
            'QPointF TlViewport::imageToWidget(', 'QPointF TlViewport::widgetToImage(', 'void TlViewport::_setGeometry('))
        code += '\n'.join(function(viewer, signature) for signature in (
            'QPointF ViewerWidget::imageToWidget(', 'QPointF ViewerWidget::widgetToImage(', 'QRectF ViewerWidget::visibleVideoRect(',
            'double ViewerWidget::zoom(', 'QPointF ViewerWidget::viewPos(', 'void ViewerWidget::setZoom(',
            'void ViewerWidget::zoom1to1(', 'bool ViewerWidget::_applyWheelZoom(', 'void ViewerWidget::mouseMoveEvent('))
        (OUT / 'dpi.cpp').write_text(code + MAIN, encoding='utf-8')
        (OUT / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(ViewerDpiRepairs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
add_executable(dpi dpi.cpp)
target_compile_options(dpi PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
target_link_libraries(dpi PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets user32)
''', encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(OUT), '-B', str(OUT / 'build'), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            self.assertEqual(0, result.returncode, (result.stdout + result.stderr).decode(errors='replace')[-4000:])
        exe = OUT / 'build/Release/dpi.exe'
        deploy_runtime(exe, QT, (ROOT / 'build_win_full/bin/Release',))
        for dpr in ('1', '1.5', '2'):
            result = subprocess.run([str(exe)], capture_output=True, creationflags=NO_WINDOW, timeout=20,
                                    env=dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_SCALE_FACTOR=dpr, QT_PLUGIN_PATH=str(QT / 'plugins')))
            (OUT / ('dpi_' + dpr + '.log')).write_bytes(result.stdout + result.stderr)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            report = json.loads(result.stdout.decode().splitlines()[-1])
            (OUT / ('dpi_' + dpr + '.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
            self.assertTrue(report['passed'], report)
            self.assertEqual(float(dpr), report['devicePixelRatio'])


if __name__ == '__main__':
    unittest.main()
