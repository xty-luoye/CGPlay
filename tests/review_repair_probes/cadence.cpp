#include "ui/viewer/TlViewport.h"
#include <tlRender/Timeline/Init.h>
#include <tlRender/Timeline/Timeline.h>
#include <tlRender/Timeline/Player.h>
#include <tlRender/Timeline/System.h>
#include <ftk/Core/Context.h>
#include <ftk/UI/Style.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <windows.h>
static int maxVisible=0;
static int visible(){int n=0;EnumWindows([](HWND h,LPARAM data)->BOOL{DWORD p;GetWindowThreadProcessId(h,&p);if(p==GetCurrentProcessId()&&IsWindowVisible(h))++*reinterpret_cast<int*>(data);return TRUE;},reinterpret_cast<LPARAM>(&n));maxVisible=std::max(maxVisible,n);return n;}
static void pump(int ms){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents(QEventLoop::AllEvents,5);visible();QThread::msleep(1);}}
int main(int argc,char**argv){
 QSurfaceFormat f;f.setVersion(4,1);f.setProfile(QSurfaceFormat::CoreProfile);f.setSwapInterval(0);QSurfaceFormat::setDefaultFormat(f);
 QApplication app(argc,argv);app.setProperty("cgplay.automationBackground",true);
 auto context=ftk::Context::create();tl::init(context);auto style=ftk::Style::create(context);
 auto timeline=tl::Timeline::create(context,argv[2]);auto player=tl::Player::create(context,timeline);
 cgplay::TlViewport viewport(context,style);viewport.setAttribute(Qt::WA_DontShowOnScreen);viewport.resize(320,180);viewport.setPlayer(player);viewport.show();
 QTimer tick;tick.setInterval(1);QObject::connect(&tick,&QTimer::timeout,[&]{context->getSystem<tl::System>()->tick();});tick.start();pump(500);
 QJsonObject report;for(double speed:{120.,6.,60.}){
 player->setSpeed(speed);player->setSpeedMult(speed==60.?2.:1.);player->setPlayback(tl::Playback::Forward);pump(400);viewport.resetRenderStats();const double before=player->getCurrentTime().to_seconds();pump(1600);
 auto phase=viewport.benchmarkDiagnostics();phase["speed"]=player->getSpeed();phase["actualSpeed"]=player->getActualSpeed();phase["sourceFps"]=player->getTimeRange().duration().rate();phase["renderFrames"]=viewport.renderFrameCount();phase["mediaDeltaSeconds"]=player->getCurrentTime().to_seconds()-before;
 report[QString::number(speed)]=phase;player->setPlayback(tl::Playback::Stop);
 }
 tick.stop();viewport.clearPlayer();player.reset();timeline.reset();report["backgroundMode"]=true;report["visiblePlatformWindowsMax"]=maxVisible;
 std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return maxVisible?1:0;
}
