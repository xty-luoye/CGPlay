"""R23/R48/R52/R55/R56/R67/R68 local UI and tooling regressions."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
import uuid
from unittest.mock import patch
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage24'
sys.path.insert(0,str(ROOT/'tools'))
import prepare_release_sources as builder
import rebuild_ffmpeg_hwaccel as ffmpeg
from validate_release_sources import validate

NATIVE=r'''
#include "playlist/PlaylistModel.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include "common/jobs/JobSystem.h"
#include <QApplication>
#include <QThread>
#include <QThreadPool>
#include <atomic>
#include "common/theme/BackdropRenderer.h"
#include <QSettings>
#include <QScreen>
#include <QSlider>
#include <QStyleOptionSlider>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QDateTime>
#include <QDockWidget>
#include <QMainWindow>
#include <cmath>
PLAYLIST_STUBS
SLIDERS
namespace cgplay{
constexpr auto kWindow="#111418";constexpr auto kPanel="#222222";
QColor appColorProperty(const char*,const QColor& = QColor());QString appStringProperty(const char*,const QString& = {});
BACKDROP
}
namespace sidebar {
struct TranslationPlaybackStrategy{static QString normalizeModeId(QString s){return s;}};
struct Bar{void setTranslationMode(QString){}};
class MainWindow:public QMainWindow{public:
struct State{QSettings*windowSettings=nullptr;bool windowStateRestored=false,leftVisible=true,rightVisible=true;QDockWidget*aiDock=nullptr;Bar*playbackBar=nullptr;QString translationPlaybackMode;QWidget*horzSplitter=nullptr;}state;
State*_p=&state;void _restoreState();void _applyAdaptiveSidePanelLayout(bool){}void _setTranslationStripVisible(bool,bool){} };
RESTORE
}
namespace audio {
struct Device{struct{std::string name;}id;};
struct AudioSystem{std::vector<Device> getDevices(){return {{{"A"}},{{"B"}},{{"B"}}};}};
struct Context{template<class T>std::shared_ptr<T>getSystem(){return std::make_shared<T>();}};
struct Player{struct Id{std::string name;};Id getAudioDevice(){return {"Current"};}};
class PlaybackController{public:struct State{std::shared_ptr<Player>player=std::make_shared<Player>();std::shared_ptr<Context>context=std::make_shared<Context>();}state;State*_p=&state;QStringList availableAudioDevices()const;};
namespace tl{using AudioSystem=audio::AudioSystem;}
AUDIO
}
int main(int argc,char**argv){QApplication app(argc,argv);const QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;
auto item=[](QString n,int marker){cgplay::ShotItem s;s.path="duplicate.fake";s.name=n;s.width=marker;return s;};
{cgplay::PlaylistModel model;model.addShot(item("C",1));model.addShot(item("A",2));model.addShot(item("A",3));model.setCurrentIndex(2);model.removeShotAt(0);
result["removeKeepsDuplicateRow"]=model.currentIndex()==1&&model.shotAt(1).width==3;model.moveUp(1);result["moveUpKeepsIdentity"]=model.currentIndex()==0&&model.shotAt(0).width==3;model.moveDown(0);result["moveDownKeepsIdentity"]=model.currentIndex()==1&&model.shotAt(1).width==3;
model.sort(false);result["sortKeepsDuplicateIdentity"]=model.shotAt(model.currentIndex()).width==3&&model.toJson()["current_index"].toInt()==model.currentIndex();model.removeShotAt(model.currentIndex());result["removeCurrentClearsHighlight"]=model.currentIndex()==-1&&!model.data(model.index(0),Qt::BackgroundRole).isValid();}
{cgplay::PlaylistModel model;model.addShot(item("C",1));model.addShot(item("A",2));model.addShot(item("B",3));model.setCurrentIndex(0);model.sort();result["sortMapsCurrent"]=model.currentIndex()==2&&model.shotAt(2).width==1;}
for(int type=0;type<2;++type){std::unique_ptr<QSlider>slider=type?std::make_unique<PreviewVolumeSlider>(Qt::Horizontal):std::make_unique<PreviewSeekSlider>(Qt::Horizontal);slider->resize(240,30);slider->setRange(0,100);slider->setValue(10);int presses=0,releases=0,moves=0;QObject::connect(slider.get(),&QSlider::sliderPressed,[&]{++presses;});QObject::connect(slider.get(),&QSlider::sliderReleased,[&]{++releases;});QObject::connect(slider.get(),&QSlider::sliderMoved,[&](int){++moves;});
auto send=[&](QEvent::Type t,double x,Qt::MouseButton b,Qt::MouseButtons bs){QMouseEvent e(t,QPointF(x,15),QPointF(x,15),b,bs,Qt::NoModifier);QCoreApplication::sendEvent(slider.get(),&e);};send(QEvent::MouseButtonPress,28,Qt::LeftButton,Qt::LeftButton);send(QEvent::MouseMove,180,Qt::NoButton,Qt::LeftButton);result[QString("slider%1MovesWhileHeld").arg(type)]=slider->value()>60&&slider->isSliderDown()&&releases==0;send(QEvent::MouseButtonRelease,300,Qt::LeftButton,Qt::NoButton);result[QString("slider%1ReleaseAndEndpoint").arg(type)]=slider->value()==100&&!slider->isSliderDown()&&presses==1&&releases==1&&moves>0;}
{QSettings settings(dir.filePath("sidebar.ini"),QSettings::IniFormat);settings.setValue("layout/leftPanelVisible",false);settings.setValue("layout/rightPanelVisible",false);sidebar::MainWindow w;w.state.windowSettings=&settings;w._restoreState();result["savedHiddenPanelsSurviveRestore"]=!w.state.leftVisible&&!w.state.rightVisible;settings.remove("layout/rightPanelVisible");w._restoreState();result["missingPanelKeyDefaultsVisible"]=!w.state.leftVisible&&w.state.rightVisible;}
{audio::PlaybackController controller;const auto devices=controller.availableAudioDevices();result["audioEnumeratesBackendAndDeduplicates"]=devices==QStringList{"Current","A","B"};controller.state.player.reset();result["audioEnumeratesWithoutPlayer"]=controller.availableAudioDevices()==QStringList{"A","B"};}
{cgplay::ThemedBackdrop w;w.resize(256,144);QImage canvas(w.size(),QImage::Format_RGB32);const QString path=dir.filePath("large.png");QFile::remove(path);app.setProperty("cgplay.backgroundType","image");app.setProperty("cgplay.backgroundImage",path);w.render(&canvas);result["missingBackgroundFallsBack"]=!app.property("cgplay.backgroundImageValid").toBool();QImage big(4096,2160,QImage::Format_RGB32);big.fill(Qt::red);big.save(path);w.render(&canvas);result["backgroundRetriesWhenFileAppears"]=app.property("cgplay.backgroundImageValid").toBool();auto prepared=cgplay::preparedBackdropPixmap(path,100,100,0,w.size(),"cover");result["largeBackgroundPreparedAtDisplaySize"]=prepared.width()<300&&prepared.height()<=144;const QString second=dir.filePath("second.png");big.fill(Qt::blue);big.save(second);app.setProperty("cgplay.backgroundImage",second);w.render(&canvas);result["backgroundPathChangeRendered"]=canvas.pixelColor(120,70).blue()>canvas.pixelColor(120,70).red();}
QThreadPool::globalInstance()->waitForDone();return finish(result);}
'''

class LocalUiRepairs(unittest.TestCase):
    def test_native_local_contracts(self):
        old=(ROOT/'tests/artifacts/review_full_20261031/phase18/probe.cpp').read_text(encoding='utf-8-sig')
        stubs=old[old.index('namespace cgplay {'):old.index('bool until(')]
        source=(ROOT/'src/plugins/quicklook/PreviewWindow.cpp').read_text(encoding='utf-8-sig');sliders=source[source.index('class PreviewSeekSlider'):source.index('} // namespace')]
        source=(ROOT/'src/ui/app/ApplicationInput.cpp').read_text(encoding='utf-8-sig');restore=function(source,'void MainWindow::_restoreState()')
        source=(ROOT/'src/core/playback/PlaybackController.cpp').read_text(encoding='utf-8-sig');audio=function(source,'QStringList PlaybackController::availableAudioDevices() const').replace('#if CGPLAY_HAS_TLRENDER','#if 1')
        source=(ROOT/'src/ui/app/ApplicationUiSupport.cpp').read_text(encoding='utf-8-sig')
        backdrop='\n'.join(function(source, signature) for signature in ['QColor appColorProperty(', 'QString appStringProperty(', 'int appIntProperty(', 'QColor compositeOpaque(', 'QColor adjustBackdropColor(', 'void drawBackdropPixmap(', 'void drawBackdropPixmapAligned('])+'\n'+function(source,'class ThemedBackdrop')+';'
        self.assertNotIn('_cachedSourcePixmap',source)
        code=NATIVE.replace('PLAYLIST_STUBS',stubs).replace('SLIDERS',sliders).replace('BACKDROP',backdrop).replace('RESTORE',restore).replace('AUDIO',audio)
        build_and_run(OUT/'native',code,['src/features/playlist/PlaylistModel.cpp','src/common/jobs/JobSystem.cpp','src/common/theme/BackdropRenderer.cpp'],extra_cmake='target_compile_definitions(probe PRIVATE CGPLAY_THEME_LIBRARY)\n')

    def test_dry_run_is_read_only(self):
        with patch.object(sys,'argv',['ffmpeg','--dry-run']),patch.object(ffmpeg,'find_msys2',side_effect=AssertionError('must not discover')),patch.object(ffmpeg.subprocess,'run',side_effect=AssertionError('must not spawn')),patch.object(Path,'mkdir',side_effect=AssertionError('must not create')),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0,ffmpeg.main())
        (OUT/'dry_run.json').write_text(json.dumps({'passed':True,'background':True,'visiblePlatformWindows':0,'networkProcessAndFilesystemMutationForbidden':True}),encoding='utf-8')

    def test_source_bundle_failed_writes_are_retryable(self):
        directory=OUT/('bundle_'+uuid.uuid4().hex);directory.mkdir(exist_ok=True);app=directory/'app';(app/'src').mkdir(parents=True,exist_ok=True)
        for name,text in [('README.md','fixture'),('LICENSE.txt','GNU GENERAL PUBLIC LICENSE'),('CMakeLists.txt','project(CGPlay VERSION 1.0.8 LANGUAGES CXX)'),('src/main.cpp','int main(){return 0;}')]: (app/name).write_text(text,encoding='utf-8')
        provenance=directory/'provenance.json';provenance.write_text(json.dumps({'schema':1,'version':'1.0.8','releaseBlockers':[],'requiredNotices':[],'files':[]}),encoding='utf-8');mapping=directory/'map.json';mapping.write_text('{"files":{}}',encoding='utf-8')
        for suffix in ('.assets.json','.sha256'):
            out=directory/('failure'+suffix.replace('.','_')+'.zip')
            original=Path.open
            class Fault:
                def __init__(self,stream): self.stream=stream
                def __enter__(self): return self
                def __exit__(self,*args): self.stream.close()
                def __getattr__(self,key): return getattr(self.stream,key)
                def write(self,text): self.stream.write(text[:1]);self.stream.flush();raise OSError('injected disk full')
            def opened(path,*a,**kw):
                stream=original(path,*a,**kw)
                return Fault(stream) if str(path).endswith(suffix) and a and a[0]=='x' else stream
            with patch.object(Path,'open',opened),self.assertRaises(OSError): builder.build_bundle(app,provenance,mapping,out)
            self.assertFalse(out.exists());self.assertFalse(out.with_suffix('.assets.json').exists());self.assertFalse(out.with_suffix('.zip.sha256').exists())
            report=builder.build_bundle(app,provenance,mapping,out);self.assertTrue(report['passed'])
            self.assertTrue(validate(provenance,bundle=out)['passed'])
        (OUT/'bundle.json').write_text(json.dumps({'passed':True,'failureCases':['index-write','sha-write'],'retryValidated':True,'background':True,'visiblePlatformWindows':0}),encoding='utf-8')
if __name__=='__main__':unittest.main()
