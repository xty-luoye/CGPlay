#include "Application.h"
#include "ApplicationPrivate.h"
#include "component/ComponentRuntimePaths.h"
#include "playback/api/IPlaybackService.h"
#include "recording/RecordingPanel.h"
#include "recording/RecordingService.h"
#include "viewer/TlViewport.h"
#include "viewer/ViewerWidget.h"

#include <tlRender/Timeline/Player.h>
#include <tlRender/Timeline/Timeline.h>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QJsonArray>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {
bool recordingSyntheticMode()
{
    return qApp->property("cgplay.automationBackground").toBool() &&
        qApp->property("cgplay.recordingSmoke").toBool();
}
QJsonObject cropValues(const QJsonObject& selection)
{
    QJsonObject result;
    for (const QString& key : {QStringLiteral("cropLeft"),QStringLiteral("cropTop"),QStringLiteral("cropWidth"),
        QStringLiteral("cropHeight"),QStringLiteral("clientWidth"),QStringLiteral("clientHeight")})
        result.insert(key,selection.value(key));
    return result;
}
QString timelineMediaPath(const ftk::Path& path)
{
    return QString::fromStdString(path.getFrames() ? path.getFrame(path.getFrames()->min(),true) : path.get());
}
} // namespace

QStringList MainWindow::_recordingProtectedPaths() const
{
    QStringList result;
    if (!_p->currentPath.isEmpty()) result.append(_p->currentPath);
    if (_p->playbackCtrl) {
        if (!_p->playbackCtrl->currentPath().isEmpty()) result.append(_p->playbackCtrl->currentPath());
        if (const auto player=_p->playbackCtrl->player()) {
            if (const auto timeline=player->getTimeline()) result.append(timelineMediaPath(timeline->getPath()));
            for (const auto& timeline : player->getCompare()) if (timeline) result.append(timelineMediaPath(timeline->getPath()));
        }
    }
    result.removeAll(QString()); result.removeDuplicates(); return result;
}

QJsonObject MainWindow::_captureRecordingPlayerSelection(QString* error) const
{
    const auto fail=[&](const QString& message) { if (error) *error=message; return QJsonObject(); };
    if (!_p->viewer || !_p->viewer->viewport()) return fail(QStringLiteral("当前播放器画面不可用。"));
    auto* viewport=_p->viewer->viewport(); auto* top=viewport->window();
    if (!top || top->width()<=0 || top->height()<=0 || viewport->width()<=0 || viewport->height()<=0)
        return fail(QStringLiteral("播放器窗口尺寸无效。"));
#ifdef Q_OS_WIN
    // Only obtain the existing top-level HWND. Calling winId() on the OpenGL
    // child would force a native child and change its composition lifecycle.
    const HWND hwnd=reinterpret_cast<HWND>(top->winId());
    RECT client{};
    if (!hwnd || !GetClientRect(hwnd,&client) || client.right<=0 || client.bottom<=0)
        return fail(QStringLiteral("无法读取播放器窗口区域。"));
    const QString handle=QString::number(reinterpret_cast<quintptr>(hwnd));
    QJsonObject selected;
    for (const auto& value : _p->recordingCatalog.value(QStringLiteral("windows")).toArray()) {
        const auto plan=value.toObject().value(QStringLiteral("plan")).toObject();
        if (plan.value(QStringLiteral("hwnd")).toString()==handle) { selected=plan; break; }
    }
    if (selected.isEmpty()) return fail(QStringLiteral("来源列表中未找到当前播放器窗口，请刷新后再试。"));
    const QRect logical=QRect(viewport->mapTo(top,QPoint()),viewport->size()).intersected(top->rect());
    if (logical.isEmpty()) return fail(QStringLiteral("播放器显示区域为空。"));
    // Map each edge independently, using the actual native client size. This
    // stays valid across fractional DPI and monitor changes without assuming DPR.
    const double sx=static_cast<double>(client.right)/top->width();
    const double sy=static_cast<double>(client.bottom)/top->height();
    const int left=qRound(logical.x()*sx), topEdge=qRound(logical.y()*sy);
    const int right=qRound((logical.x()+logical.width())*sx), bottom=qRound((logical.y()+logical.height())*sy);
    if (left<0 || topEdge<0 || right>client.right || bottom>client.bottom || right<=left || bottom<=topEdge)
        return fail(QStringLiteral("播放器区域超出窗口范围。"));
    selected.insert(QStringLiteral("source"),QStringLiteral("player"));
    selected.insert(QStringLiteral("cropLeft"),left); selected.insert(QStringLiteral("cropTop"),topEdge);
    selected.insert(QStringLiteral("cropWidth"),right-left); selected.insert(QStringLiteral("cropHeight"),bottom-topEdge);
    selected.insert(QStringLiteral("clientWidth"),static_cast<int>(client.right)); selected.insert(QStringLiteral("clientHeight"),static_cast<int>(client.bottom));
    selected.insert(QStringLiteral("geometryEpoch"),static_cast<double>(_p->recordingGeometryEpoch));
    return selected;
#else
    return fail(QStringLiteral("录屏功能当前只支持 Windows。"));
#endif
}

void MainWindow::_refreshRecordingCatalog()
{
    if (!_p->recordingService || !_p->recordingPanel || _p->recordingClosePending || _p->recordingService->isBusy()) return;
    const ComponentRuntimePaths paths=captureComponentRuntimePaths();
    RecordingRuntimeSnapshot runtime;
    runtime.recorderRoot=paths.recordingRoot; runtime.helperExecutable=paths.recordingExecutable;
    runtime.generation=paths.recordingGeneration; runtime.pythonExecutable=paths.pythonExecutable;
    runtime.pythonHome=paths.pythonHome; runtime.toolsRoot=paths.toolsRoot;
    runtime.finalizerScript=paths.script(QStringLiteral("cgplay/export/recording_finalize.py"));
    runtime.ffmpegExecutable=paths.ffmpegExecutable; runtime.ffprobeExecutable=paths.ffprobeExecutable;
    runtime.pythonEnvironment=paths.pythonEnvironment();
    const QString testRoot=qApp->property("cgplay.recordingSmokeRuntime").toString();
    if (recordingSyntheticMode() && !testRoot.isEmpty()) {
        runtime.recorderRoot=QFileInfo(testRoot).absoluteFilePath();
        runtime.helperExecutable=QDir(runtime.recorderRoot).filePath(QStringLiteral("CGPlayRecorder.exe"));
        runtime.generation=runtime.recorderRoot+QStringLiteral("|explicit-synthetic-test");
    }
    _p->recordingRuntime=std::make_shared<RecordingRuntimeSnapshot>(runtime);
    _p->recordingCatalog={}; _p->recordingPanel->setPlayerSelection({});
    const bool ready=QFileInfo(runtime.helperExecutable).isFile() && QFileInfo(runtime.finalizerScript).isFile() &&
        QFileInfo(runtime.pythonExecutable).isFile() && QFileInfo(runtime.ffmpegExecutable).isFile() && QFileInfo(runtime.ffprobeExecutable).isFile();
    if (!ready) {
        _p->recordingPanel->setCatalog({{"error",QStringLiteral("录屏组件不完整，请安装包含录屏组件的完整安装包后重试。")}});
        return;
    }
    _p->recordingPanel->setCatalog({{"error",QStringLiteral("正在读取可用的录制来源和声音设备…")}});
    _p->recordingService->refreshCatalog(runtime,recordingSyntheticMode());
}

bool MainWindow::_showRecordingPanel()
{
    if (_p->recordingDock) { _p->recordingDock->show(); _p->recordingDock->raise(); return true; }
    auto* service=qApp->findChild<RecordingService*>(QStringLiteral("CGPlayRecordingService"),Qt::FindDirectChildrenOnly);
    if (!service) { service=new RecordingService(qApp); service->setObjectName(QStringLiteral("CGPlayRecordingService")); }
    _p->recordingService=service;
    auto* dock=new QDockWidget(QStringLiteral("录屏"),this); dock->setObjectName(QStringLiteral("RecordingDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea);
    dock->setFeatures(QDockWidget::DockWidgetClosable|QDockWidget::DockWidgetMovable);
    auto* scroll=new QScrollArea(dock); scroll->setWidgetResizable(true);
    auto* panel=new RecordingPanel(scroll); scroll->setWidget(panel); dock->setWidget(scroll);
    _p->recordingDock=dock; _p->recordingPanel=panel;
    addDockWidget(Qt::RightDockWidgetArea,dock); resizeDocks({dock},{410},Qt::Horizontal); dock->show();
    connect(panel,&RecordingPanel::refreshRequested,this,&MainWindow::_refreshRecordingCatalog);
    connect(service,&RecordingService::catalogReady,this,[this](const QJsonObject& catalog) {
        _p->recordingCatalog=catalog;
        if (_p->recordingRuntime && !catalog.value(QStringLiteral("runtimeIdentity")).toString().isEmpty())
            _p->recordingRuntime->generation+=QLatin1Char('|')+catalog.value(QStringLiteral("runtimeIdentity")).toString();
        if (_p->recordingPanel) { _p->recordingPanel->setCatalog(catalog); _p->recordingPanel->setPlayerSelection(_captureRecordingPlayerSelection()); }
        setProperty("cgplay.recordingCatalog",catalog.toVariantMap());
    });
    connect(service,&RecordingService::stateChanged,this,[this](const QJsonObject& state) {
        setProperty("cgplay.recordingStatus",state.toVariantMap());
        if (_p->recordingPanel) _p->recordingPanel->setServiceState(state);
        if (statusBar() && state.value(QStringLiteral("busy")).toBool())
            statusBar()->showMessage(QStringLiteral("录屏：%1").arg(state.value(QStringLiteral("state")).toString()));
    });
    connect(service,&RecordingService::finished,this,[this](const QJsonObject& result) {
        if (_p->recordingGeometryTimer) _p->recordingGeometryTimer->stop();
        setProperty("cgplay.recordingResult",result.toVariantMap());
        if (_p->recordingPanel && _p->recordingService) _p->recordingPanel->setServiceState(_p->recordingService->status());
    });
    connect(service,&RecordingService::closeReady,this,[this] {
        if (!_p->recordingClosePending) return;
        _p->recordingCloseReady=true;
        QTimer::singleShot(0,this,[this] { close(); });
    });
    const auto reject=[this](const QString& error) {
        QJsonObject state=_p->recordingService && _p->recordingService->isBusy()
            ? _p->recordingService->status() : QJsonObject{{"state","Failed"},{"busy",false}};
        state.insert(QStringLiteral("error"),error);
        if (_p->recordingPanel) _p->recordingPanel->setServiceState(state);
        setProperty("cgplay.recordingStartError",error);
    };
    connect(panel,&RecordingPanel::startRequested,this,[this,reject](QJsonObject settings) {
        if (_p->recordingClosePending) { reject(QStringLiteral("窗口正在等待录像保存并关闭，不能开始新的录屏。")); return; }
        if (!_p->recordingRuntime || !_p->recordingService || _p->recordingService->isBusy()) { reject(QStringLiteral("录屏服务尚未就绪或仍在保存。")); return; }
        RecordingStartRequest request; request.runtime=*_p->recordingRuntime;
        request.finalOutputPath=settings.take(QStringLiteral("output")).toString();
        _p->recordingSourceKind=settings.value(QStringLiteral("source")).toString();
        _p->recordingGeometryEpoch=0; _p->recordingLastGeometry={};
        if (_p->recordingSourceKind==QStringLiteral("player")) {
            QString error; const auto player=_captureRecordingPlayerSelection(&error);
            if (player.isEmpty()) { reject(error); return; }
            for (auto it=player.begin();it!=player.end();++it) settings.insert(it.key(),it.value());
            _p->recordingWindowHwnd=player.value(QStringLiteral("hwnd")).toString();
            _p->recordingLastGeometry=cropValues(player);
        }
        request.synthetic=recordingSyntheticMode();
        if (request.synthetic) { settings.insert(QStringLiteral("source"),QStringLiteral("synthetic")); settings.insert(QStringLiteral("sourceWidth"),320); settings.insert(QStringLiteral("sourceHeight"),180); }
        request.helperPlan=settings; request.protectedMediaPaths=_recordingProtectedPaths();
        setProperty("cgplay.recordingLastPlan",settings.toVariantMap()); setProperty("cgplay.recordingResult",QVariant());
        QString error;
        if (!_p->recordingService->start(request,&error)) { reject(error); return; }
        if (_p->recordingSourceKind==QStringLiteral("player") && _p->recordingGeometryTimer) _p->recordingGeometryTimer->start();
    });
    connect(panel,&RecordingPanel::pauseRequested,service,&RecordingService::setPaused);
    connect(panel,&RecordingPanel::audioChanged,service,&RecordingService::setAudio);
    connect(panel,&RecordingPanel::stopRequested,service,&RecordingService::stop);
    connect(panel,&RecordingPanel::recoverRequested,this,[this,reject](const QString& sidecar,const QString& target) {
        if (_p->recordingClosePending) { reject(QStringLiteral("窗口正在等待录像保存并关闭，请稍后恢复。")); return; }
        if (!_p->recordingRuntime || !_p->recordingService) { reject(QStringLiteral("录屏服务尚未就绪。")); return; }
        RecordingRecoveryRequest request; request.runtime=*_p->recordingRuntime;
        request.sessionDescriptorPath=sidecar; request.finalOutputPath=target; request.protectedMediaPaths=_recordingProtectedPaths();
        setProperty("cgplay.recordingResult",QVariant()); QString error;
        if (!_p->recordingService->recover(request,&error)) reject(error);
    });
    _p->recordingGeometryTimer=new QTimer(this); _p->recordingGeometryTimer->setInterval(100);
    connect(_p->recordingGeometryTimer,&QTimer::timeout,this,[this] {
        if (!_p->recordingService || !_p->recordingService->isBusy() || _p->recordingSourceKind!=QStringLiteral("player")) return;
        const QString state=_p->recordingService->status().value(QStringLiteral("state")).toString();
        if (state!=QStringLiteral("Starting") && state!=QStringLiteral("Recording") && state!=QStringLiteral("Paused")) return;
        const auto selected=_captureRecordingPlayerSelection();
        if (selected.isEmpty() || selected.value(QStringLiteral("hwnd")).toString()!=_p->recordingWindowHwnd) { _p->recordingService->stop(); return; }
        auto geometry=cropValues(selected);
        if (geometry==_p->recordingLastGeometry) return;
        _p->recordingLastGeometry=geometry; geometry.insert(QStringLiteral("geometryEpoch"),static_cast<double>(++_p->recordingGeometryEpoch));
        _p->recordingService->setPlayerCrop(geometry);
    });
    panel->setServiceState(service->status()); _refreshRecordingCatalog(); return true;
}

bool MainWindow::_deferCloseForRecording()
{
    if (_p->recordingCloseReady) { _p->recordingCloseReady=false; _p->recordingClosePending=false; return false; }
    if (!_p->recordingService || (!_p->recordingService->isBusy() && !_p->recordingClosePending)) return false;
    if (!_p->recordingClosePending) {
        _p->recordingClosePending=true; _p->recordingService->requestStopForClose();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("正在停止并保存录屏，完成后关闭窗口…"));
    }
    return true;
}

} // namespace cgplay
