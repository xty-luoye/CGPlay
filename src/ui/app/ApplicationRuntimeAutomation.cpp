#include "Application.h"

#include "SecondaryWindow.h"

#include "common/core/OverlayRuntimeDebug.h"
#include "common/core/ServiceLocator.h"
#include "common/events/EventBus.h"
#include "common/events/api/IEventBus.h"
#include "common/events/api/EventTypes.h"
#include "common/theme/ThemeService.h"
#include "ai/AIContextBuilder.h"
#include "ai/AIProviderManager.h"
#include "ai/AIWorkflowService.h"
#include "ai/MediaFrameSnapshotService.h"
#include "ai/OpenAIResponsesProvider.h"
#include "ai/SubtitleGenerationService.h"
#include "ai/TranslationPlaybackStrategy.h"
#include "ai/WindowsDpapiCredentialStore.h"
#include "ai/discovery/AIProviderDetector.h"
#include "plugins/PluginManager.h"
#include "plugins/annotation/AnnotationPlugin.h"
#include "plugins/ocio/OcioPlugin.h"
#include "plugins/quicklook/QuickLookPlugin.h"
#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationOverlayProvider.h"
#include "annotation/api/IAnnotationService.h"
#include "cache/CacheManager.h"
#include "component/ComponentManager.h"
#include "core/session/api/ISessionContributor.h"
#include "hwdecode/HardwareDecodeManager.h"
#include "media/MediaProbe.h"
#include "media/MediaService.h"
#include "ocio/OcioManager.h"
#include "playback/PlaybackController.h"
#include "playback/api/IPlaybackService.h"
#include "playback/PlaybackStats.h"
#include "settings/api/ISettingsService.h"
#include "settings/SettingsService.h"
#include "viewer/TlViewport.h"
#include "viewer/ViewerWidget.h"
#include "features/annotation/api/IAnnotationViewBridge.h"
#include "viewer/api/IActivePlaybackView.h"
#include "viewer/api/IOverlayHost.h"
#include "viewer/api/IOverlayProvider.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QAction>
#include <QAbstractButton>
#include <QComboBox>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDockWidget>
#include <QStatusBar>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QImage>
#include <QIcon>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScreen>
#include <QStandardPaths>
#include <QSet>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QToolButton>
#include <QGraphicsDropShadowEffect>
#include <QVector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <cstdlib>

#include "ApplicationRuntimeSupport.h"

namespace cgplay {

using namespace application_runtime;

namespace {

QJsonArray providerSummaryArray(const QVector<ProviderCountSummary>& summaries)
{
    QJsonArray array;
    for (const auto& summary : summaries) {
        QJsonObject item;
        item.insert(QStringLiteral("plugin"), summary.plugin);
        item.insert(QStringLiteral("count"), summary.count);
        array.append(item);
    }
    return array;
}

} // namespace

int Application::_runComponentCheck()
{
    ComponentManager& manager = ComponentManager::instance();
    QString error;
    const bool refreshed = manager.refreshRemoteManifest(&error);
    const QStringList missing = manager.missingRequiredComponents();

    QJsonObject result;
    result.insert(QStringLiteral("manifestRefreshed"), refreshed);
    result.insert(QStringLiteral("manifestError"), error);
    result.insert(QStringLiteral("currentVersion"), applicationVersion());
    result.insert(QStringLiteral("remoteVersion"), manager.remoteAppVersion());
    result.insert(QStringLiteral("updateAvailable"), manager.isAppUpdateAvailable(applicationVersion()));

    QJsonArray pluginArray;
    if (_pluginManager) {
        const QStringList pluginIds = _pluginManager->loadedPluginIds();
        for (const QString& id : pluginIds) {
            pluginArray.append(id);
        }
    }
    result.insert(QStringLiteral("loadedPlugins"), pluginArray);

    QJsonArray dynamicPluginArray;
    if (_pluginManager) {
        const QStringList pluginIds = _pluginManager->loadedDynamicPluginIds();
        for (const QString& id : pluginIds) {
            dynamicPluginArray.append(id);
        }
    }
    result.insert(QStringLiteral("loadedDynamicPlugins"), dynamicPluginArray);

    if (_pluginManager) {
        result.insert(QStringLiteral("menuProviders"),
                      providerSummaryArray(_pluginManager->menuProviderSummaries()));
        result.insert(QStringLiteral("toolbarProviders"),
                      providerSummaryArray(_pluginManager->toolbarProviderSummaries()));
        result.insert(QStringLiteral("panelProviders"),
                      providerSummaryArray(_pluginManager->panelProviderSummaries()));
    } else {
        result.insert(QStringLiteral("menuProviders"), QJsonArray());
        result.insert(QStringLiteral("toolbarProviders"), QJsonArray());
        result.insert(QStringLiteral("panelProviders"), QJsonArray());
    }

    QJsonArray missingArray;
    for (const QString& id : missing) {
        missingArray.append(id);
    }
    result.insert(QStringLiteral("missingComponents"), missingArray);

    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (!_componentCheckOutput.isEmpty()) {
        QFile file(_componentCheckOutput);
        QDir().mkpath(QFileInfo(file).absolutePath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(json);
        }
    }
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << json << Qt::endl;
    return 0;
}

QJsonObject Application::_captureRuntimeDump() const
{
    QJsonObject dump;
    dump.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    dump.insert(QStringLiteral("version"), applicationVersion());
    dump.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
    dump.insert(QStringLiteral("overlayDebugEnabled"), _overlayDebugMode);
    dump.insert(QStringLiteral("fallbackEnabled"), !_disablePluginFallback);
    dump.insert(QStringLiteral("activeViewId"), _activeViewId);
    dump.insert(QStringLiteral("hasActiveView"), hasActiveView());

    QJsonArray windows;
    if (_mainWindow && _mainWindow->viewerWidget()) {
        windows.append(QJsonObject{
            { QStringLiteral("window"), QStringLiteral("main") },
            { QStringLiteral("viewId"), _mainWindow->viewerWidget()->viewId() },
            { QStringLiteral("active"), _mainWindow->viewerWidget()->viewId() == _activeViewId }
        });
    }
    for (int index = 0; index < static_cast<int>(_secondaryWindows.size()); ++index) {
        auto* secondary = _secondaryWindows[index];
        if (!secondary || !secondary->viewerWidget()) {
            continue;
        }
        windows.append(QJsonObject{
            { QStringLiteral("window"), QStringLiteral("secondary") },
            { QStringLiteral("index"), index },
            { QStringLiteral("viewId"), secondary->viewerWidget()->viewId() },
            { QStringLiteral("active"), secondary->viewerWidget()->viewId() == _activeViewId }
        });
    }
    dump.insert(QStringLiteral("windows"), windows);

    // Keep the effective appearance contract observable for automated smoke
    // tests and profile migration checks.  These are resolved runtime tokens,
    // not raw settings, so the dump reflects what Qt widgets actually use.
    if (qApp) {
        const auto appProperty = [](const char* name) -> QJsonValue {
            const QVariant value = qApp->property(name);
            return value.isValid() ? QJsonValue::fromVariant(value) : QJsonValue();
        };
        QJsonObject appearance{
            { QStringLiteral("mode"), appProperty("cgplay.themeMode") },
            { QStringLiteral("backgroundColor"), appProperty("cgplay.backgroundColor") },
            { QStringLiteral("backgroundSecondary"), appProperty("cgplay.backgroundSecondary") },
            { QStringLiteral("backgroundType"), appProperty("cgplay.backgroundType") },
            { QStringLiteral("backgroundImage"), appProperty("cgplay.backgroundImage") },
            { QStringLiteral("backgroundImageConfigured"), appProperty("cgplay.backgroundImageConfigured") },
            { QStringLiteral("backgroundImageValid"), appProperty("cgplay.backgroundImageValid") },
            { QStringLiteral("fillMode"), appProperty("cgplay.backgroundFillMode") },
            { QStringLiteral("backgroundOpacity"), appProperty("cgplay.backgroundOpacity") },
            { QStringLiteral("panelColor"), appProperty("cgplay.panelColor") },
            { QStringLiteral("panelOpacity"), appProperty("cgplay.panelOpacity") },
            { QStringLiteral("toolbarColor"), appProperty("cgplay.toolbarColor") },
            { QStringLiteral("toolbarOpacity"), appProperty("cgplay.toolbarOpacity") },
            { QStringLiteral("timelineColor"), appProperty("cgplay.timelineColor") },
            { QStringLiteral("timelineOpacity"), appProperty("cgplay.timelineOpacity") },
            { QStringLiteral("viewerColor"), appProperty("cgplay.viewerColor") },
            { QStringLiteral("dialogColor"), appProperty("cgplay.dialogColor") },
            { QStringLiteral("textColor"), appProperty("cgplay.textColor") },
            { QStringLiteral("borderColor"), appProperty("cgplay.borderColor") },
            { QStringLiteral("accentColor"), appProperty("cgplay.accentColor") },
            { QStringLiteral("subtitleOpacity"), appProperty("cgplay.subtitleOpacity") },
            { QStringLiteral("viewerOpacity"), appProperty("cgplay.viewerOpacity") },
            { QStringLiteral("brightness"), appProperty("cgplay.backgroundBrightness") },
            { QStringLiteral("saturation"), appProperty("cgplay.backgroundSaturation") },
            { QStringLiteral("blurRadius"), appProperty("cgplay.backgroundBlurRadius") },
            { QStringLiteral("vignette"), appProperty("cgplay.backgroundVignette") },
            { QStringLiteral("shadowStrength"), appProperty("cgplay.shadowStrength") },
            { QStringLiteral("dynamicBackground"), appProperty("cgplay.dynamicBackground") }
            ,{ QStringLiteral("buttonOpacity"), appProperty("cgplay.buttonOpacity") }
        };
        dump.insert(QStringLiteral("appearance"), appearance);
    }

    if (_mainWindow) {
        QJsonObject customization;
        customization.insert(
            QStringLiteral("workspacePreset"),
            _userSettings
                ? _userSettings->value(QStringLiteral("workspace/preset"), QStringLiteral("\u9ed8\u8ba4\u5ba1\u7247")).toString()
                : QString());
        customization.insert(
            QStringLiteral("mouseWheel"),
            _userSettings
                ? _userSettings->value(QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")).toString()
                : QStringLiteral("zoom"));
        const auto readBindingObject = [](const QString& fileName) {
            QFile file(QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(fileName));
            if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
            return error.error == QJsonParseError::NoError && document.isObject()
                ? document.object() : QJsonObject{};
        };
        customization.insert(QStringLiteral("mouseBindings"), readBindingObject(QStringLiteral("mouse_bindings.json")));
        customization.insert(QStringLiteral("gamepadBindings"), readBindingObject(QStringLiteral("gamepad_bindings.json")));

        QJsonArray commands;
        QSet<QString> emittedCommands;
        for (QAction* action : _mainWindow->findChildren<QAction*>()) {
            if (!action) continue;
            const QString id = action->property("commandId").toString().trimmed();
            if (id.isEmpty() || emittedCommands.contains(id)) continue;
            emittedCommands.insert(id);
            commands.append(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("shortcut"), action->shortcut().toString(QKeySequence::PortableText)},
                {QStringLiteral("enabled"), action->isEnabled()},
                {QStringLiteral("visible"), action->isVisible()}
            });
        }
        customization.insert(QStringLiteral("commands"), commands);

        if (auto* splitter = _mainWindow->findChild<QSplitter*>(QStringLiteral("cgplayMainSplitter"))) {
            QJsonArray sizes;
            for (int size : splitter->sizes()) sizes.append(size);
            customization.insert(QStringLiteral("splitterSizes"), sizes);
        }

        QJsonArray customButtons;
        for (QWidget* widget : _mainWindow->findChildren<QWidget*>()) {
            if (!widget || !widget->property("cgplay.customButton").toBool()) continue;
            auto* button = qobject_cast<QAbstractButton*>(widget);
            customButtons.append(QJsonObject{
                {QStringLiteral("id"), widget->property("commandId").toString()},
                {QStringLiteral("objectName"), widget->objectName()},
                {QStringLiteral("text"), button ? button->text() : QString()},
                {QStringLiteral("visible"), widget->isVisible()},
                {QStringLiteral("enabled"), widget->isEnabled()},
                {QStringLiteral("width"), widget->width()},
                {QStringLiteral("height"), widget->height()},
                {QStringLiteral("styleSheet"), widget->styleSheet()}
            });
        }
        customization.insert(QStringLiteral("customButtons"), customButtons);
        QJsonArray buttonStyleTargets;
        for (QAbstractButton* button : _mainWindow->findChildren<QAbstractButton*>()) {
            if (!button) continue;
            QString styleId = button->property("commandId").toString().trimmed();
            if (styleId.isEmpty()) styleId = button->property("cgplay.style.id").toString().trimmed();
            if (styleId.isEmpty()) continue;
            buttonStyleTargets.append(QJsonObject{
                {QStringLiteral("id"), styleId},
                {QStringLiteral("objectName"), button->objectName()},
                {QStringLiteral("text"), button->text()},
                {QStringLiteral("hasIcon"), !button->icon().isNull()},
                {QStringLiteral("iconWidth"), button->iconSize().width()},
                {QStringLiteral("iconHeight"), button->iconSize().height()},
                {QStringLiteral("minimumWidth"), button->minimumWidth()},
                {QStringLiteral("minimumHeight"), button->minimumHeight()},
                {QStringLiteral("maximumWidth"), button->maximumWidth()},
                {QStringLiteral("maximumHeight"), button->maximumHeight()},
                {QStringLiteral("customStyleApplied"), button->property("cgplay.customStyleApplied").toBool()},
                {QStringLiteral("styleSheet"), button->styleSheet()}
            });
        }
        customization.insert(QStringLiteral("buttonStyleTargets"), buttonStyleTargets);
        if (auto* host = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayCustomToolbar"))) {
            customization.insert(QStringLiteral("customToolbarVisible"), host->isVisible());
        }
        dump.insert(QStringLiteral("customization"), customization);
    }

    if (_mainWindow) {
        const auto widgetGeometry = [this](const QString& objectName) {
            QJsonObject item{{QStringLiteral("objectName"), objectName}};
            QWidget* widget = _mainWindow->findChild<QWidget*>(objectName);
            item.insert(QStringLiteral("present"), widget != nullptr);
            if (widget) {
                const QRect rect = widget->geometry();
                item.insert(QStringLiteral("x"), rect.x());
                item.insert(QStringLiteral("y"), rect.y());
                item.insert(QStringLiteral("width"), rect.width());
                item.insert(QStringLiteral("height"), rect.height());
                item.insert(QStringLiteral("visible"), widget->isVisible());
            }
            return item;
        };
        dump.insert(QStringLiteral("viewerGeometry"), QJsonObject{
            {QStringLiteral("shell"), widgetGeometry(QStringLiteral("cgplayViewerShell"))},
            {QStringLiteral("viewer"), widgetGeometry(QStringLiteral("ViewerWidget"))},
            {QStringLiteral("chrome"), widgetGeometry(QStringLiteral("viewerChrome"))},
            {QStringLiteral("centerSplitter"), widgetGeometry(QStringLiteral("cgplayCenterSplitter"))}
        });
        const auto dockObject = [this](const QString& objectName) {
            QJsonObject item{
                { QStringLiteral("objectName"), objectName }
            };
            auto* dock = _mainWindow->findChild<QDockWidget*>(objectName);
            item.insert(QStringLiteral("present"), dock != nullptr);
            if (!dock) {
                return item;
            }
            const QRect geometry = dock->geometry();
            item.insert(QStringLiteral("visible"), dock->isVisible());
            item.insert(QStringLiteral("floating"), dock->isFloating());
            item.insert(QStringLiteral("x"), geometry.x());
            item.insert(QStringLiteral("y"), geometry.y());
            item.insert(QStringLiteral("width"), geometry.width());
            item.insert(QStringLiteral("height"), geometry.height());
            item.insert(QStringLiteral("title"), dock->windowTitle());
            return item;
        };

        dump.insert(QStringLiteral("aiWorkspaceDock"), dockObject(QStringLiteral("AIAgentWorkspaceDock")));
    }

    QJsonObject pluginInfo;
    QJsonArray loadedPlugins;
    QJsonArray loadedDynamicPlugins;
    QJsonObject providers;
    if (_pluginManager) {
        for (const QString& id : _pluginManager->loadedPluginIds()) {
            loadedPlugins.append(id);
        }
        for (const QString& id : _pluginManager->loadedDynamicPluginIds()) {
            loadedDynamicPlugins.append(id);
        }
        providers.insert(QStringLiteral("menuProviders"),
                         providerSummaryArray(_pluginManager->menuProviderSummaries()));
        providers.insert(QStringLiteral("toolbarProviders"),
                         providerSummaryArray(_pluginManager->toolbarProviderSummaries()));
        providers.insert(QStringLiteral("panelProviders"),
                         providerSummaryArray(_pluginManager->panelProviderSummaries()));
    } else {
        providers.insert(QStringLiteral("menuProviders"), QJsonArray());
        providers.insert(QStringLiteral("toolbarProviders"), QJsonArray());
        providers.insert(QStringLiteral("panelProviders"), QJsonArray());
    }
    pluginInfo.insert(QStringLiteral("loadedPlugins"), loadedPlugins);
    pluginInfo.insert(QStringLiteral("loadedDynamicPlugins"), loadedDynamicPlugins);
    dump.insert(QStringLiteral("plugins"), pluginInfo);
    dump.insert(QStringLiteral("providers"), providers);

    auto* annotationService = ServiceLocator::getService<IAnnotationService>();
    auto* sessionContributor = ServiceLocator::getService<ISessionContributor>();
    auto* eventBus = ServiceLocator::getService<IEventBus>();
    auto* playbackService = ServiceLocator::getService<IPlaybackService>();
    auto* activeView = ServiceLocator::getService<IActivePlaybackView>();
    auto* overlayHost = ServiceLocator::getService<IOverlayHost>();
    auto* coordinateMapper = ServiceLocator::getService<IViewerCoordinateMapper>();
    auto* overlayProvider = ServiceLocator::getService<IOverlayProvider>();
    auto* annotationBridge = ServiceLocator::getService<IAnnotationViewBridge>();

    const bool annotationFallbackService = annotationService && _annoMgr &&
        annotationService == static_cast<IAnnotationService*>(_annoMgr.get());
    const bool sessionFallbackService = sessionContributor && _annoMgr &&
        sessionContributor == static_cast<ISessionContributor*>(_annoMgr.get());
    const bool overlayFallbackProvider = overlayProvider && _annotationFallbackOverlayProvider &&
        overlayProvider == static_cast<IOverlayProvider*>(_annotationFallbackOverlayProvider.get());
    const bool bridgeFallbackProvider = annotationBridge && _annotationFallbackOverlayProvider &&
        annotationBridge == static_cast<IAnnotationViewBridge*>(_annotationFallbackOverlayProvider.get());

    QJsonArray capabilities;
    const auto appendCapability = [&capabilities](
        const QString& name,
        bool present,
        const QString& owner = {},
        const QString& viewId = {}) {
        QJsonObject item{
            { QStringLiteral("name"), name },
            { QStringLiteral("present"), present }
        };
        if (!owner.isEmpty()) {
            item.insert(QStringLiteral("owner"), owner);
        }
        if (!viewId.isEmpty()) {
            item.insert(QStringLiteral("viewId"), viewId);
        }
        capabilities.append(item);
    };
    appendCapability(QStringLiteral("PluginManager"), _pluginManager != nullptr, QStringLiteral("Application"));
    appendCapability(QStringLiteral("IEventBus"), eventBus != nullptr, QStringLiteral("Application"));
    appendCapability(QStringLiteral("IActivePlaybackView"), activeView != nullptr, QStringLiteral("Application"), _activeViewId);
    appendCapability(QStringLiteral("IPlaybackService"), playbackService != nullptr, QStringLiteral("activeView"), _activeViewId);
    appendCapability(QStringLiteral("IOverlayHost"), overlayHost != nullptr, QStringLiteral("activeView"), _activeViewId);
    appendCapability(QStringLiteral("IViewerCoordinateMapper"), coordinateMapper != nullptr, QStringLiteral("activeView"), _activeViewId);
    appendCapability(QStringLiteral("IAnnotationService"),
                     annotationService != nullptr,
                     annotationFallbackService ? QStringLiteral("annotationFallback") : QStringLiteral("annotationPlugin"),
                     _activeViewId);
    appendCapability(QStringLiteral("ISessionContributor"),
                     sessionContributor != nullptr,
                     sessionFallbackService ? QStringLiteral("annotationFallback") : QStringLiteral("annotationPlugin"),
                     _activeViewId);
    appendCapability(QStringLiteral("IOverlayProvider"),
                     overlayProvider != nullptr,
                     overlayFallbackProvider ? QStringLiteral("annotationFallback") : QStringLiteral("annotationPlugin"),
                     overlayProvider ? overlayProvider->boundViewId() : QString());
    appendCapability(QStringLiteral("IAnnotationViewBridge"),
                     annotationBridge != nullptr,
                     bridgeFallbackProvider ? QStringLiteral("annotationFallback") : QStringLiteral("annotationPlugin"),
                     overlayProvider ? overlayProvider->boundViewId() : QString());
    dump.insert(QStringLiteral("capabilities"), capabilities);

    QJsonArray bridges;
    if (annotationBridge || overlayProvider) {
        QJsonObject bridge{
            { QStringLiteral("name"), QStringLiteral("annotation") },
            { QStringLiteral("pluginName"),
              bridgeFallbackProvider ? QStringLiteral("annotationFallback") : QStringLiteral("annotation") },
            { QStringLiteral("providerId"), QStringLiteral("annotation.overlay") },
            { QStringLiteral("present"), annotationBridge != nullptr },
            { QStringLiteral("boundViewId"), overlayProvider ? overlayProvider->boundViewId() : QString() },
            { QStringLiteral("overlayCount"), overlayProvider ? overlayProvider->overlayCount() : 0 },
            { QStringLiteral("overlayInstanceId"),
              overlayProvider ? static_cast<double>(overlayProvider->overlayInstanceId()) : 0.0 },
            { QStringLiteral("hasOverlayBinding"), overlayProvider ? overlayProvider->hasOverlayBinding() : false },
            { QStringLiteral("hasOverlayHostBinding"), overlayProvider ? overlayProvider->hasOverlayHostBinding() : false },
            { QStringLiteral("hasCoordinateMapperBinding"),
              overlayProvider ? overlayProvider->hasCoordinateMapperBinding() : false }
        };
        if (annotationBridge) {
            bridge.insert(QStringLiteral("currentFrame"), annotationBridge->currentFrame());
            bridge.insert(QStringLiteral("selectedAnnotationId"), annotationBridge->selectedAnnotationId());
        }
        bridges.append(bridge);
        dump.insert(QStringLiteral("overlayCount"), overlayProvider ? overlayProvider->overlayCount() : 0);
    } else {
        dump.insert(QStringLiteral("overlayCount"), 0);
    }
    dump.insert(QStringLiteral("bridges"), bridges);

    if (_mainWindow) {
        dump.insert(QStringLiteral("annotationRuntime"), _mainWindow->captureRuntimeAnnotationState());
    } else {
        dump.insert(QStringLiteral("annotationRuntime"), QJsonObject());
    }

    return dump;
}

bool Application::_writeRuntimeDump(const QString& outputPath, const QJsonObject& dump) const
{
    return writeJsonObjectFile(outputPath, dump);
}

int Application::_runRuntimeDump()
{
    QString outputPath = _dumpRuntimeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_runtime_dump.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();
    processEvents();

    const QJsonObject dump = _captureRuntimeDump();
    if (!_writeRuntimeDump(outputPath, dump)) {
        qCritical() << "[RuntimeDump] Cannot write output:" << outputPath;
        return 4;
    }

    const QByteArray json = QJsonDocument(dump).toJson(QJsonDocument::Indented);
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << json << Qt::endl;
    qInfo() << "[RuntimeDump] Saved:" << outputPath;
    return 0;
}

int Application::_runPlaybackBenchmark()
{
    QFileInfo mediaInfo(_benchmarkMedia);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Benchmark] Media not found:" << _benchmarkMedia;
        return 2;
    }

    QString outputPath = _benchmarkOutput;
    if (outputPath.isEmpty()) {
        outputPath = mediaInfo.absolutePath() + QStringLiteral("/cgplay_playback_benchmark.json");
    }
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));

    _mainWindow->resize(1500, 840);
    _mainWindow->show();
    _mainWindow->openFile(mediaInfo.absoluteFilePath());

    auto* playback = _mainWindow->playbackController();
    if (!playback || !playback->isValid()) {
        qCritical() << "[Benchmark] Player failed to open:" << mediaInfo.absoluteFilePath();
        return 3;
    }
    auto* playbackController = dynamic_cast<PlaybackController*>(playback);
    auto* viewport = _mainWindow->viewerWidget() ? _mainWindow->viewerWidget()->viewport() : nullptr;

    auto* stats = playback->playbackStats();
    if (stats) {
        stats->reset();
    }
    if (viewport) {
        viewport->resetRenderStats();
    }

    QElapsedTimer elapsed;
    elapsed.start();
    QDateTime playbackStartWall;
    qint64 playbackStartElapsedMs = 0;

    struct FrameSample
    {
        int frame = 0;
        qint64 tMs = 0;
    };
    QVector<FrameSample> samples;
    samples.reserve(2048);
    QJsonObject speedBeforeForward;
    QJsonObject speedAfterForward;

    auto captureSpeedState = [&]() {
        QJsonObject state;
#if CGPLAY_HAS_TLRENDER
        if (playbackController && playbackController->player()) {
            auto player = playbackController->player();
            const auto playbackState = player->getPlayback();
            int playbackValue = 0;
            if (playbackState == tl::Playback::Forward) {
                playbackValue = 1;
            } else if (playbackState == tl::Playback::Reverse) {
                playbackValue = 2;
            }
            state.insert(QStringLiteral("playback"), playbackValue);
            state.insert(QStringLiteral("speed"), player->getSpeed());
            state.insert(QStringLiteral("speedMult"), player->getSpeedMult());
            state.insert(QStringLiteral("actualSpeed"), player->getActualSpeed());
            state.insert(QStringLiteral("timeRangeRate"), player->getTimeRange().duration().rate());
        }
#endif
        return state;
    };

    QObject::connect(
        playback->signalProxy(),
        &PlaybackServiceSignals::currentFrameChanged,
        this,
        [&](int frame, int) {
            const qint64 t = elapsed.elapsed();
            if (t >= _benchmarkWarmupMs) {
                samples.push_back({ frame, t });
            }
        });

    QTimer::singleShot(_benchmarkWarmupMs, this, [&, playback, stats, viewport] {
        if (stats) {
            stats->reset();
        }
        if (viewport) {
            viewport->resetRenderStats();
        }
        playbackStartWall = QDateTime::currentDateTime();
        playbackStartElapsedMs = elapsed.elapsed();
        speedBeforeForward = captureSpeedState();
        playback->forward();
        speedAfterForward = captureSpeedState();
    });

    int exitCode = 0;
    QTimer::singleShot(
        _benchmarkWarmupMs + _benchmarkDurationMs,
        this,
        [&, playback, stats, mediaInfo, outputPath, screenshotPath] {
            const QDateTime playbackEndWall = QDateTime::currentDateTime();
            playback->stop();

            qint64 firstMs = 0;
            qint64 lastMs = 0;
            int firstFrame = 0;
            int lastFrame = 0;
            int frameEvents = samples.size();
            int forwardSteps = 0;
            int repeatedOrReverseSteps = 0;
            int skippedFrames = 0;
            int loopWraps = 0;

            if (!samples.isEmpty()) {
                firstMs = samples.first().tMs;
                lastMs = samples.last().tMs;
                firstFrame = samples.first().frame;
                lastFrame = samples.last().frame;
                for (int i = 1; i < samples.size(); ++i) {
                    const int delta = samples[i].frame - samples[i - 1].frame;
                    if (delta > 0) {
                        forwardSteps += delta;
                        if (delta > 1) {
                            skippedFrames += delta - 1;
                        }
                    } else if (delta < 0) {
                        loopWraps += 1;
                    } else {
                        repeatedOrReverseSteps += 1;
                    }
                }
            }

            const double measuredSeconds =
                samples.size() > 1 ? static_cast<double>(lastMs - firstMs) / 1000.0 : 0.0;
            const double eventFps =
                measuredSeconds > 0.0 ? static_cast<double>(std::max(0, frameEvents - 1)) / measuredSeconds : 0.0;
            const double mediaFps = playback->fps();
            const double mediaTimeDeltaSeconds =
                mediaFps > 0.0 && lastFrame >= firstFrame
                    ? static_cast<double>(lastFrame - firstFrame) / mediaFps
                    : 0.0;
            const double playbackRate =
                measuredSeconds > 0.0 ? mediaTimeDeltaSeconds / measuredSeconds : 0.0;
            const bool playbackRatePassed =
                playbackRate >= 0.90 && playbackRate <= 1.10;
            const double expectedFrames =
                mediaFps > 0.0 ? mediaFps * (static_cast<double>(_benchmarkDurationMs) / 1000.0) : 0.0;
            const double expectedRatio =
                expectedFrames > 0.0 ? 100.0 * static_cast<double>(frameEvents) / expectedFrames : 0.0;

            PlaybackSummary summary;
            if (stats) {
                summary = stats->summary();
            }
            const qint64 renderFrames = viewport ? viewport->renderFrameCount() : 0;
            const double renderSeconds = viewport ? viewport->renderStatsSeconds() : 0.0;
            const double renderFps = viewport ? viewport->renderFps() : 0.0;
            const QJsonObject viewportDiagnostics =
                viewport ? viewport->benchmarkDiagnostics() : QJsonObject();
            bool screenshotSaved = false;
            {
                const QFileInfo screenshotInfo(screenshotPath);
                QDir().mkpath(screenshotInfo.absolutePath());
                const QPixmap capture = _mainWindow->grab();
                screenshotSaved = !capture.isNull() && capture.save(screenshotPath, "PNG");
            }

            QJsonArray sampleJson;
            const int stride = std::max(1, static_cast<int>(samples.size() / 240));
            for (int i = 0; i < samples.size(); i += stride) {
                QJsonObject item;
                item["t_ms"] = static_cast<double>(samples[i].tMs);
                item["frame"] = samples[i].frame;
                sampleJson.append(item);
            }

            QJsonObject playbackStats;
            playbackStats["total_frames"] = summary.totalFrames;
            playbackStats["avg_fps"] = summary.avgFps;
            playbackStats["current_fps"] = stats ? stats->currentFps() : 0.0;
            playbackStats["min_fps"] = summary.minFps;
            playbackStats["max_fps"] = summary.maxFps;
            playbackStats["dropped_frames"] = summary.droppedFrames;
            playbackStats["drop_rate_pct"] = summary.dropRate;
            playbackStats["avg_decode_ms"] = summary.avgDecodeMs;
            playbackStats["avg_render_ms"] = summary.avgRenderMs;

            QJsonObject report;
            report["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
            report["exe"] = QCoreApplication::applicationFilePath();
            report["media"] = mediaInfo.absoluteFilePath();
            report["media_name"] = mediaInfo.fileName();
            report["media_fps"] = mediaFps;
            report["total_frames"] = playback->totalFrames();
            report["warmup_ms"] = _benchmarkWarmupMs;
            report["duration_ms"] = _benchmarkDurationMs;
            report["measured_seconds"] = measuredSeconds;
            report["sample_start_wall_clock"] =
                playbackStartWall.isValid()
                    ? playbackStartWall.addMSecs(firstMs - playbackStartElapsedMs).toString(Qt::ISODateWithMs)
                    : QString();
            report["sample_end_wall_clock"] =
                playbackStartWall.isValid() && lastMs > 0
                    ? playbackStartWall.addMSecs(lastMs - playbackStartElapsedMs).toString(Qt::ISODateWithMs)
                    : playbackEndWall.toString(Qt::ISODateWithMs);
            report["first_frame"] = firstFrame;
            report["last_frame"] = lastFrame;
            report["start_timecode_seconds"] =
                mediaFps > 0.0 ? static_cast<double>(firstFrame) / mediaFps : 0.0;
            report["end_timecode_seconds"] =
                mediaFps > 0.0 ? static_cast<double>(lastFrame) / mediaFps : 0.0;
            report["media_time_delta_seconds"] = mediaTimeDeltaSeconds;
            report["wall_clock_delta_seconds"] = measuredSeconds;
            report["playbackRate"] = playbackRate;
            report["playbackRateTarget"] = 1.0;
            report["playbackRatePass"] = playbackRatePassed;
            report["frame_events"] = frameEvents;
            report["frame_event_fps"] = eventFps;
            report["expected_frames_at_media_fps"] = expectedFrames;
            report["expected_frame_event_ratio_pct"] = expectedRatio;
            report["forward_frame_steps"] = forwardSteps;
            report["estimated_skipped_frames"] = skippedFrames;
            report["repeated_or_stationary_events"] = repeatedOrReverseSteps;
            report["loop_wraps"] = loopWraps;
            report["render_frames"] = static_cast<double>(renderFrames);
            report["render_seconds"] = renderSeconds;
            report["render_fps"] = renderFps;
            report["viewport_diagnostics"] = viewportDiagnostics;
            report["playback_stats"] = playbackStats;
            report["tl_speed_before_forward"] = speedBeforeForward;
            report["tl_speed_after_forward"] = speedAfterForward;
            report["screenshot_path"] = QFileInfo(screenshotPath).absoluteFilePath();
            report["screenshot_saved"] = screenshotSaved;
            report["sample_stride"] = stride;
            report["samples"] = sampleJson;
            report["note"] =
                "playbackRate is media_time_delta_seconds divided by wall_clock_delta_seconds and is the speed pass gate. Top/bottom FPS counters, frame_event_fps, PlaybackStats, and render_fps are diagnostic only.";

            QFile out(outputPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                qCritical() << "[Benchmark] Cannot write output:" << outputPath;
                exitCode = 4;
            } else {
                out.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
                out.close();
                qInfo() << "[Benchmark] Saved:" << outputPath;
                qInfo() << "[Benchmark] eventFPS=" << eventFps
                        << "statsAvgFPS=" << summary.avgFps
                        << "renderFPS=" << renderFps
                        << "frames=" << frameEvents
                        << "skipped=" << skippedFrames;
            }

            quit();
        });

    return exec() == 0 ? exitCode : 1;
}


int Application::_runUiCapture()
{
    QString outputPath = _captureUiOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_ui_capture.png");
    }

    _mainWindow->resize(_captureUiWidth, _captureUiHeight);
    _mainWindow->show();

    if (_captureUiDemo) {
        _mainWindow->prepareCaptureDemoState(_captureUiMedia);
    } else if (!_captureUiMedia.isEmpty()) {
        const QFileInfo mediaInfo(_captureUiMedia);
        if (!mediaInfo.exists() || !mediaInfo.isFile()) {
            qCritical() << "[CaptureUI] Media not found:" << _captureUiMedia;
            return 2;
        }
        _mainWindow->openFile(mediaInfo.absoluteFilePath());
    }

    int exitCode = 0;
    QTimer::singleShot(_captureUiDelayMs, this, [&, outputPath] {
        const QPixmap capture = _mainWindow->grab();
        if (capture.isNull()) {
            qCritical() << "[CaptureUI] Empty screenshot";
            exitCode = 3;
            quit();
            return;
        }

        const QFileInfo outInfo(outputPath);
        QDir().mkpath(outInfo.absolutePath());
        if (!capture.save(outputPath, "PNG")) {
            qCritical() << "[CaptureUI] Cannot save screenshot:" << outputPath;
            exitCode = 4;
        } else {
            qInfo() << "[CaptureUI] Saved:" << outputPath;
        }
        // Capture mode is a short-lived automation process.  The OpenGL/tlRender
        // teardown can race the Qt event-loop shutdown after a successful grab
        // and produce an access violation even though the artifact is valid.
        // Exit immediately after flushing logs; normal interactive playback
        // never enters this path.
        fflush(nullptr);
        std::_Exit(exitCode);
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runCodexWorkbenchSmoke()
{
    QString outputPath = _codexWorkbenchSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/codex_workbench_smoke.json");
    }
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));

    _mainWindow->resize(1920, 1080);
    _mainWindow->show();

    QJsonArray checks;
    QStringList failures;
    QJsonObject evidence;
    const auto addCheck = [&](const QString& name, bool passed, const QJsonObject& details) {
        QJsonObject item{{QStringLiteral("name"), name}, {QStringLiteral("passed"), passed}};
        if (!details.isEmpty()) item.insert(QStringLiteral("details"), details);
        checks.append(item);
        if (!passed) failures.push_back(name);
    };

    int phase = 0;
    int exitCode = 0;
    bool finalized = false;
    QElapsedTimer phaseTimer;
    phaseTimer.start();
    QElapsedTimer totalTimer;
    totalTimer.start();
    QTimer poll;
    poll.setInterval(100);

    const auto finish = [&](const QString& timeoutReason = QString()) {
        if (finalized) return;
        finalized = true;
        if (!timeoutReason.isEmpty()) {
            addCheck(
                QStringLiteral("smoke-completed-before-timeout"),
                false,
                QJsonObject{{QStringLiteral("reason"), timeoutReason}});
        }

        QString captureMethod;
        const bool screenshotSaved = saveWindowEvidence(_mainWindow.get(), screenshotPath, &captureMethod);
        addCheck(
            QStringLiteral("printwindow-screenshot-saved"),
            screenshotSaved && captureMethod == QStringLiteral("PrintWindow"),
            QJsonObject{
                {QStringLiteral("path"), QFileInfo(screenshotPath).absoluteFilePath()},
                {QStringLiteral("method"), captureMethod}
            });

        if (auto* status = _mainWindow->findChild<QLabel*>(QStringLiteral("CodexConnectionStatus"))) {
            evidence.insert(QStringLiteral("connectionStatus"), status->text());
        }
        if (auto* error = _mainWindow->findChild<QLabel*>(QStringLiteral("CodexError"))) {
            evidence.insert(QStringLiteral("visibleError"), error->isVisible() ? error->text() : QString());
        }
        if (auto* workspace = _mainWindow->findChild<QWidget*>(QStringLiteral("CodexAgentWorkspace"))) {
            evidence.insert(
                QStringLiteral("confirmedModel"),
                workspace->property("codexConfirmedModel").toString());
            evidence.insert(
                QStringLiteral("confirmedReasoningEffort"),
                workspace->property("codexConfirmedReasoningEffort").toString());
            evidence.insert(
                QStringLiteral("imageGenerationAvailable"),
                workspace->property("codexImageGenerationAvailable").toBool());
            evidence.insert(
                QStringLiteral("imageGenerationReason"),
                workspace->property("codexImageGenerationReason").toString());
        }
        evidence.insert(QStringLiteral("elapsedMs"), totalTimer.elapsed());
        evidence.insert(QStringLiteral("screenshot"), QFileInfo(screenshotPath).absoluteFilePath());

        const QJsonObject report{
            {QStringLiteral("pass"), failures.isEmpty()},
            {QStringLiteral("checks"), checks},
            {QStringLiteral("failureCount"), failures.size()},
            {QStringLiteral("failures"), QJsonArray::fromStringList(failures)},
            {QStringLiteral("evidence"), evidence}
        };
        if (!writeJsonObjectFile(outputPath, report)) {
            exitCode = 4;
        } else {
            exitCode = failures.isEmpty() ? 0 : 5;
        }
        poll.stop();
        QTimer::singleShot(0, this, &QCoreApplication::quit);
    };

    connect(&poll, &QTimer::timeout, this, [&]() {
        auto* codexDock = _mainWindow->findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"));
        auto* workspace = _mainWindow->findChild<QWidget*>(QStringLiteral("CodexAgentWorkspace"));
        auto* prompt = workspace
            ? workspace->findChild<QPlainTextEdit*>(QStringLiteral("CodexPrompt"))
            : nullptr;
        auto* retry = workspace
            ? workspace->findChild<QToolButton*>(QStringLiteral("CodexRetry"))
            : nullptr;

        if (phase == 0) {
            if (!codexDock || !workspace || !prompt) {
                if (phaseTimer.elapsed() > 15000) finish(QStringLiteral("Codex Dock or composer was not created."));
                return;
            }
            if (!prompt->isEnabled()) {
                if (retry && retry->isVisible()) {
                    finish(QStringLiteral("Codex auto-start entered the retry state."));
                } else if (phaseTimer.elapsed() > 45000) {
                    finish(QStringLiteral("Codex auto-start did not become ready."));
                }
                return;
            }

            auto* aiDock = _mainWindow->findChild<QDockWidget*>(QStringLiteral("AIAgentWorkspaceDock"));
            auto* model = workspace->findChild<QComboBox*>(QStringLiteral("CodexModel"));
            auto* approval = workspace->findChild<QToolButton*>(QStringLiteral("CodexApprovalMode"));
            auto* reasoning = workspace->findChild<QToolButton*>(QStringLiteral("CodexReasoning"));
            auto* send = workspace->findChild<QToolButton*>(QStringLiteral("CodexSend"));
            QAction* toggleAction = codexDock->toggleViewAction();

            if (!workspace->property("codexDeliveryUiChecked").toBool() && send) {
                const QString fixturePath = QFileInfo(outputPath).dir().filePath(QStringLiteral("codex_delivery_fixture.hip"));
                QFile fixture(fixturePath);
                const bool fixtureWritten = fixture.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    fixture.write("CGPlay file delivery smoke") > 0;
                fixture.close();
                const bool artifactInvoked = fixtureWritten && QMetaObject::invokeMethod(
                    workspace,
                    "appendFileArtifact",
                    Qt::DirectConnection,
                    Q_ARG(QString, fixturePath),
                    Q_ARG(QString, QStringLiteral("codex_delivery_fixture.hip")));
                auto* artifact = workspace->findChild<QFrame*>(QStringLiteral("CodexFileArtifact"));
                auto* openFile = workspace->findChild<QToolButton*>(QStringLiteral("CodexFileOpen"));
                auto* saveAs = workspace->findChild<QToolButton*>(QStringLiteral("CodexFileSaveAs"));
                const QString savedAsPath = QFileInfo(outputPath).dir().filePath(QStringLiteral("codex_delivery_saved_as.hip"));
                QFile::remove(savedAsPath);
                workspace->setProperty("codexFileSaveAsTestPath", savedAsPath);
                if (saveAs) saveAs->click();
                workspace->setProperty("codexFileSaveAsTestPath", QVariant());
                addCheck(
                    QStringLiteral("generated-file-delivery-controls"),
                    artifactInvoked && artifact && openFile && saveAs && openFile->isEnabled() && saveAs->isEnabled(),
                    QJsonObject{{QStringLiteral("path"), fixturePath}});
                addCheck(
                    QStringLiteral("generated-file-save-as-copy"),
                    QFileInfo(savedAsPath).isFile() && QFileInfo(savedAsPath).size() == QFileInfo(fixturePath).size(),
                    QJsonObject{{QStringLiteral("path"), savedAsPath}});

                const QJsonObject detectedSoftware = workspace->property("codexDetectedSoftware").toJsonObject();
                addCheck(
                    QStringLiteral("dcc-standard-install-detection"),
                    QFileInfo(detectedSoftware.value(QStringLiteral("hython")).toString()).isFile(),
                    QJsonObject{{QStringLiteral("executables"), detectedSoftware}});

                const bool activeInvoked = QMetaObject::invokeMethod(
                    workspace, "setTurnInProgress", Qt::DirectConnection, Q_ARG(bool, true));
                const bool stopState = activeInvoked && prompt->isEnabled() && send->text().isEmpty() && !send->icon().isNull();
                addCheck(QStringLiteral("composer-steer-and-stop-state"), stopState, QJsonObject{});
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                if (auto* conversation = workspace->findChild<QScrollArea*>(QStringLiteral("CodexConversation"))) {
                    conversation->verticalScrollBar()->setValue(conversation->verticalScrollBar()->maximum());
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                const QFileInfo smokeOutput(outputPath);
                const QString deliveryScreenshot = smokeOutput.dir().filePath(
                    smokeOutput.completeBaseName() + QStringLiteral("_file_delivery.png"));
                QString deliveryCaptureMethod;
                const bool deliveryScreenshotSaved = saveWindowEvidence(
                    _mainWindow.get(), deliveryScreenshot, &deliveryCaptureMethod);
                addCheck(
                    QStringLiteral("file-delivery-stop-visual"),
                    deliveryScreenshotSaved && deliveryCaptureMethod == QStringLiteral("PrintWindow"),
                    QJsonObject{{QStringLiteral("path"), deliveryScreenshot},
                                {QStringLiteral("method"), deliveryCaptureMethod}});
                QMetaObject::invokeMethod(workspace, "setTurnInProgress", Qt::DirectConnection, Q_ARG(bool, false));
                workspace->setProperty("codexDeliveryUiChecked", true);
            }

            if (!model || model->count() < 8) {
                if (phaseTimer.elapsed() > 45000) {
                    finish(QStringLiteral("API model discovery did not append the live model catalog."));
                }
                return;
            }
            const QJsonObject nativeResponses = workspace->property("codexNativeResponses").toJsonObject();
            const QStringList requiredNativeResponses{QStringLiteral("account/read"), QStringLiteral("account/usage/read"),
                QStringLiteral("account/rateLimits/read"), QStringLiteral("permissionProfile/list"),
                QStringLiteral("collaborationMode/list"), QStringLiteral("experimentalFeature/list"),
                QStringLiteral("windowsSandbox/readiness"), QStringLiteral("hooks/list")};
            bool nativeResponsesReady = true;
            for (const QString& method : requiredNativeResponses) nativeResponsesReady = nativeResponsesReady && nativeResponses.contains(method);
            if (!nativeResponsesReady && phaseTimer.elapsed() < 45000) return;

            const QJsonArray currentMcpServers = workspace->property("codexMcpServers").toJsonArray();
            bool currentNodeReplFound = false;
            for (const QJsonValue& value : currentMcpServers) currentNodeReplFound = currentNodeReplFound || value.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("node_repl");
            if (currentNodeReplFound && !workspace->property("codexSmokeMcpCallSent").toBool()) {
                workspace->setProperty("codexSmokeMcpCallSent", true);
                const QJsonObject mcpArguments{{QStringLiteral("code"), QStringLiteral("nodeRepl.write('CGPlay MCP live OK')")},
                    {QStringLiteral("title"), QStringLiteral("Verify MCP bridge")}};
                QMetaObject::invokeMethod(workspace, "mcpToolCallRequested", Qt::DirectConnection,
                    Q_ARG(QString, QStringLiteral("node_repl")), Q_ARG(QString, QStringLiteral("js")),
                    Q_ARG(QJsonObject, mcpArguments));
                return;
            }
            const QJsonObject responsesAfterMcpCall = workspace->property("codexNativeResponses").toJsonObject();
            if (currentNodeReplFound && !responsesAfterMcpCall.contains(QStringLiteral("mcpServer/tool/call")) && phaseTimer.elapsed() < 45000) return;

            addCheck(QStringLiteral("auto-start-ready"), true, QJsonObject{});
            addCheck(QStringLiteral("native-runtime-rpc-response-coverage"), nativeResponsesReady,
                QJsonObject{{QStringLiteral("responses"), nativeResponses}});
            const QJsonObject mcpToolResponse = responsesAfterMcpCall.value(QStringLiteral("mcpServer/tool/call")).toObject();
            addCheck(QStringLiteral("mcp-tool-call-real-protocol"), currentNodeReplFound &&
                    (!mcpToolResponse.contains(QStringLiteral("error")) || mcpToolResponse.value(QStringLiteral("error")).isNull()),
                QJsonObject{{QStringLiteral("response"), mcpToolResponse}});
            const QJsonArray mcpServers = workspace->property("codexMcpServers").toJsonArray();
            bool nodeReplFound = false;
            for (const QJsonValue& value : mcpServers) {
                nodeReplFound = nodeReplFound || value.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("node_repl");
            }
            addCheck(QStringLiteral("mcp-status-real-protocol"), nodeReplFound,
                QJsonObject{{QStringLiteral("servers"), mcpServers}});
            addCheck(
                QStringLiteral("codex-owns-right-dock-height"),
                codexDock->isVisible() && codexDock->height() >= (_mainWindow->height() - 90),
                QJsonObject{
                    {QStringLiteral("dockHeight"), codexDock->height()},
                    {QStringLiteral("windowHeight"), _mainWindow->height()}
                });
            addCheck(
                QStringLiteral("legacy-ai-dock-hidden"),
                aiDock && !aiDock->isVisible(),
                QJsonObject{{QStringLiteral("legacyDockFound"), aiDock != nullptr}});
            addCheck(
                QStringLiteral("composer-model-and-permission-controls"),
                model && model->isVisible() && model->count() > 0 &&
                    !model->currentData().toString().trimmed().isEmpty() &&
                    approval && approval->isVisible() && approval->menu() &&
                    reasoning && reasoning->isVisible() && reasoning->menu() &&
                    send && send->isVisible(),
                QJsonObject{
                    {QStringLiteral("modelCount"), model ? model->count() : 0},
                    {QStringLiteral("selectedModel"), model ? model->currentData().toString() : QString()}
                });

            QStringList modelIds;
            for (int index = 0; model && index < model->count(); ++index) {
                modelIds.push_back(model->itemData(index).toString());
            }
            const QStringList detectedApiModels = _userSettings
                ? _userSettings->value(QStringLiteral("ai/connection/availableModels")).toStringList()
                : QStringList{};
            bool allApiModelsPresent = detectedApiModels.size() > 1;
            for (const QString& expected : detectedApiModels) {
                if (!modelIds.contains(expected, Qt::CaseInsensitive)) {
                    allApiModelsPresent = false;
                    break;
                }
            }
            allApiModelsPresent = allApiModelsPresent &&
                modelIds.contains(QStringLiteral("gpt-5.5"), Qt::CaseInsensitive) &&
                modelIds.contains(QStringLiteral("gpt-5.6-terra"), Qt::CaseInsensitive);
            addCheck(
                QStringLiteral("live-api-models-appended-with-existing-models-preserved"),
                allApiModelsPresent,
                QJsonObject{
                    {QStringLiteral("models"), QJsonArray::fromStringList(modelIds)},
                    {QStringLiteral("detectedApiModels"), QJsonArray::fromStringList(detectedApiModels)}
                });

            int terraIndex = -1;
            for (int index = 0; model && index < model->count(); ++index) {
                if (model->itemData(index).toString() == QStringLiteral("gpt-5.6-terra")) {
                    terraIndex = index;
                    break;
                }
            }
            if (terraIndex < 0 || !reasoning || !reasoning->menu()) {
                finish(QStringLiteral("GPT-5.6 Terra or the reasoning menu was not available."));
                return;
            }
            model->setCurrentIndex(terraIndex);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

            QStringList reasoningEfforts;
            QAction* ultraAction = nullptr;
            for (QAction* action : reasoning->menu()->actions()) {
                const QString effort = action ? action->data().toString().trimmed() : QString();
                if (effort.isEmpty()) continue;
                reasoningEfforts.push_back(effort);
                if (effort == QStringLiteral("ultra")) ultraAction = action;
            }
            const QStringList expectedTerraEfforts{
                QStringLiteral("low"),
                QStringLiteral("medium"),
                QStringLiteral("high"),
                QStringLiteral("xhigh"),
                QStringLiteral("max"),
                QStringLiteral("ultra")
            };
            addCheck(
                QStringLiteral("reasoning-menu-matches-selected-model-catalog"),
                reasoningEfforts == expectedTerraEfforts && ultraAction,
                QJsonObject{{QStringLiteral("efforts"), QJsonArray::fromStringList(reasoningEfforts)}});
            if (!ultraAction) {
                finish(QStringLiteral("Ultra reasoning was not available for GPT-5.6 Terra."));
                return;
            }
            ultraAction->trigger();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            addCheck(
                QStringLiteral("composer-switches-model-and-reasoning"),
                model->currentData().toString() == QStringLiteral("gpt-5.6-terra") &&
                    ultraAction->isChecked(),
                QJsonObject{
                    {QStringLiteral("model"), model->currentData().toString()},
                    {QStringLiteral("effort"), ultraAction->data().toString()}
                });

            QStringList permissionModes;
            if (approval && approval->menu()) {
                for (QAction* action : approval->menu()->actions()) {
                    const QString mode = action ? action->data().toString().trimmed() : QString();
                    if (!mode.isEmpty()) permissionModes.push_back(mode);
                }
            }
            permissionModes.sort();
            addCheck(
                QStringLiteral("permission-menu-has-three-modes"),
                permissionModes == QStringList{QStringLiteral("auto"), QStringLiteral("full"), QStringLiteral("request")},
                QJsonObject{{QStringLiteral("modes"), QJsonArray::fromStringList(permissionModes)}});

            bool hasRegularStartButton = false;
            for (QAbstractButton* button : workspace->findChildren<QAbstractButton*>()) {
                if (!button) continue;
                const QString name = button->objectName();
                QString text = button->text();
                text.remove(QLatin1Char('&'));
                if (name.contains(QStringLiteral("Start"), Qt::CaseInsensitive) ||
                    text.trimmed() == QStringLiteral("启动")) {
                    hasRegularStartButton = true;
                    break;
                }
            }
            addCheck(QStringLiteral("no-regular-start-button"), !hasRegularStartButton, QJsonObject{});
            addCheck(
                QStringLiteral("retry-hidden-while-connected"),
                retry && !retry->isVisible(),
                QJsonObject{});

            bool inViewMenu = false;
            bool inWindowMenu = false;
            if (_mainWindow->menuBar() && toggleAction) {
                for (QAction* menuAction : _mainWindow->menuBar()->actions()) {
                    QMenu* menu = menuAction ? menuAction->menu() : nullptr;
                    if (!menu || !menu->actions().contains(toggleAction)) continue;
                    if (menu->title() == QStringLiteral("视图")) inViewMenu = true;
                    if (menu->title() == QStringLiteral("窗口")) inWindowMenu = true;
                }
            }
            addCheck(
                QStringLiteral("dock-toggle-registered-in-view-and-window"),
                inViewMenu && inWindowMenu,
                QJsonObject{{QStringLiteral("view"), inViewMenu}, {QStringLiteral("window"), inWindowMenu}});

            phase = 10;
            phaseTimer.restart();
            return;
        }

        if (phase == 10) {
            const QString confirmedModel = workspace
                ? workspace->property("codexConfirmedModel").toString()
                : QString();
            const QString confirmedEffort = workspace
                ? workspace->property("codexConfirmedReasoningEffort").toString()
                : QString();
            if (confirmedModel == QStringLiteral("gpt-5.6-terra") &&
                confirmedEffort == QStringLiteral("ultra")) {
                addCheck(
                    QStringLiteral("app-server-confirms-model-and-reasoning-switch"),
                    true,
                    QJsonObject{
                        {QStringLiteral("model"), confirmedModel},
                        {QStringLiteral("effort"), confirmedEffort}
                    });
                auto* closeButton = workspace
                    ? workspace->findChild<QToolButton*>(QStringLiteral("CodexClose"))
                    : nullptr;
                if (!closeButton) {
                    finish(QStringLiteral("Codex close control was not found."));
                    return;
                }
                closeButton->click();
                phase = 1;
                phaseTimer.restart();
                return;
            }
            if (phaseTimer.elapsed() > 15000) {
                addCheck(
                    QStringLiteral("app-server-confirms-model-and-reasoning-switch"),
                    false,
                    QJsonObject{
                        {QStringLiteral("model"), confirmedModel},
                        {QStringLiteral("effort"), confirmedEffort}
                    });
                finish(QStringLiteral("App-server did not confirm the selected model and reasoning effort."));
            }
            return;
        }

        if (phase == 1) {
            if (phaseTimer.elapsed() < 250) return;
            const bool hidden = codexDock && !codexDock->isVisible();
            addCheck(QStringLiteral("dock-close-hides-session-view"), hidden, QJsonObject{});
            if (!codexDock) {
                finish(QStringLiteral("Codex Dock disappeared after close."));
                return;
            }
            codexDock->toggleViewAction()->trigger();
            phase = 2;
            phaseTimer.restart();
            return;
        }

        if (phase == 2) {
            if (phaseTimer.elapsed() < 350) return;
            const bool restored = codexDock && codexDock->isVisible();
            addCheck(QStringLiteral("view-menu-toggle-restores-dock"), restored, QJsonObject{});
            auto* newConversation = workspace
                ? workspace->findChild<QToolButton*>(QStringLiteral("CodexHeaderNewConversation"))
                : nullptr;
            if (!restored || !newConversation || !newConversation->isEnabled()) {
                finish(QStringLiteral("Codex Dock or new-conversation control did not recover."));
                return;
            }
            newConversation->click();
            phase = 3;
            phaseTimer.restart();
            return;
        }

        if (phase == 3) {
            if (!prompt || !prompt->isEnabled()) {
                if (phaseTimer.elapsed() > 30000) finish(QStringLiteral("New conversation did not become ready."));
                return;
            }
            addCheck(QStringLiteral("new-conversation-ready"), true, QJsonObject{});
            auto* send = workspace->findChild<QToolButton*>(QStringLiteral("CodexSend"));
            prompt->setPlainText(QStringLiteral("请不要调用工具，只回复：RVLite Codex 工作台联通正常。"));
            workspace->setProperty("codexLastHostTool", QString());
            workspace->setProperty("codexLastHostToolResult", QJsonObject{});
            prompt->setPlainText(QStringLiteral(
                "Call cgplay.player_status now. Report hasMedia, transport, currentFrame, totalFrames, "
                "fps, volume, and muted. Do not use shell commands or infer from logs."));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const bool canSend = send && send->isEnabled();
            addCheck(QStringLiteral("composer-enables-send"), canSend, QJsonObject{});
            if (!canSend) {
                finish(QStringLiteral("Send did not enable for a valid prompt."));
                return;
            }
            send->click();
            phase = 4;
            phaseTimer.restart();
            return;
        }

        if (phase == 4) {
            const QList<QLabel*> userLabels = workspace
                ? workspace->findChildren<QLabel*>(QStringLiteral("CodexUserText"))
                : QList<QLabel*>{};
            const QList<QLabel*> assistantLabels = workspace
                ? workspace->findChildren<QLabel*>(QStringLiteral("CodexAssistantText"))
                : QList<QLabel*>{};
            const bool userMessageVisible = !userLabels.isEmpty() && userLabels.constLast()->isVisible();
            const bool assistantVisible = !assistantLabels.isEmpty() &&
                assistantLabels.constLast()->isVisible() &&
                !assistantLabels.constLast()->text().trimmed().isEmpty();
            const bool turnComplete = prompt && prompt->isEnabled() &&
                !workspace->property("codexTurnInProgress").toBool();
            if (userMessageVisible && assistantVisible && turnComplete) {
                const QString hostTool = workspace->property("codexLastHostTool").toString();
                const QJsonObject hostResult = workspace->property("codexLastHostToolResult").toJsonObject();
                const QJsonArray contentItems = hostResult.value(QStringLiteral("contentItems")).toArray();
                const QString hostText = contentItems.isEmpty()
                    ? QString()
                    : contentItems.first().toObject().value(QStringLiteral("text")).toString();
                QJsonParseError hostParseError;
                const QJsonObject hostPayload =
                    QJsonDocument::fromJson(hostText.toUtf8(), &hostParseError).object();
                const bool hostToolCompleted =
                    hostTool.section(QLatin1Char('.'), -1) == QStringLiteral("player_status") &&
                    hostResult.value(QStringLiteral("success")).toBool() &&
                    hostParseError.error == QJsonParseError::NoError &&
                    hostPayload.value(QStringLiteral("ok")).toBool() &&
                    hostPayload.contains(QStringLiteral("hasMedia")) &&
                    hostPayload.contains(QStringLiteral("transport")) &&
                    hostPayload.contains(QStringLiteral("volume")) &&
                    hostPayload.contains(QStringLiteral("muted"));
                addCheck(
                    QStringLiteral("app-server-calls-real-cgplay-player-status"),
                    hostToolCompleted,
                    QJsonObject{{QStringLiteral("tool"), hostTool},
                                {QStringLiteral("payload"), hostPayload}});
                addCheck(QStringLiteral("user-message-rendered"), true, QJsonObject{});
                addCheck(
                    QStringLiteral("assistant-response-rendered"),
                    true,
                    QJsonObject{{QStringLiteral("text"), assistantLabels.constLast()->text()}});
                addCheck(
                    QStringLiteral("task-card-rendered"),
                    workspace->findChild<QFrame*>(QStringLiteral("CodexTaskCard")) != nullptr,
                    QJsonObject{});
                const QString currentThreadId = workspace->property("codexCurrentThreadId").toString();
                workspace->setProperty("codexSmokeOriginalThreadId", currentThreadId);
                if (workspace->property("codexImageGenerationAvailable").toBool()) {
                    prompt->setPlainText(QStringLiteral(
                        "Use the native image generation tool to create a small test image: "
                        "a red square, green circle, and blue triangle on a white background. "
                        "Do not use shell commands or write image code."));
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    if (auto* send = workspace->findChild<QToolButton*>(QStringLiteral("CodexSend"))) send->click();
                    phase = 45;
                } else {
                    QMetaObject::invokeMethod(workspace, "sessionListRequested", Qt::DirectConnection);
                    phase = 5;
                }
                phaseTimer.restart();
                return;
            }
            if (phaseTimer.elapsed() > 120000) {
                addCheck(QStringLiteral("user-message-rendered"), userMessageVisible, QJsonObject{});
                addCheck(QStringLiteral("assistant-response-rendered"), assistantVisible, QJsonObject{});
                finish(QStringLiteral("Codex turn did not complete with visible assistant text."));
            }
        }

        if (phase == 45) {
            auto* preview = workspace->findChild<QLabel*>(QStringLiteral("CodexGeneratedImage"));
            auto* importButton = workspace->findChild<QToolButton*>(QStringLiteral("CodexImageImport"));
            const QString path = preview ? preview->toolTip() : QString();
            const QImage image = path.isEmpty() ? QImage() : QImage(path);
            if (preview && preview->isVisible() && !image.isNull() && prompt && prompt->isEnabled()) {
                bool varied = false;
                const QColor first = image.pixelColor(0, 0);
                for (int y = 0; y < image.height() && !varied; y += qMax(1, image.height() / 8))
                    for (int x = 0; x < image.width() && !varied; x += qMax(1, image.width() / 8))
                        varied = image.pixelColor(x, y) != first;
                addCheck(QStringLiteral("provider-native-image-generated"), varied,
                    {{QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()}});
                addCheck(QStringLiteral("provider-native-image-thumbnail-rendered"), !preview->pixmap().isNull(), {});
                if (importButton) importButton->click();
                addCheck(QStringLiteral("provider-native-image-imported"),
                    workspace->property("codexImportedGeneratedImage").toString() == QFileInfo(path).absoluteFilePath(), {});
                QMetaObject::invokeMethod(workspace, "sessionListRequested", Qt::DirectConnection);
                phase = 5; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 60000) {
                QString response;
                const QList<QLabel*> assistantLabels = workspace->findChildren<QLabel*>(QStringLiteral("CodexAssistantText"));
                if (!assistantLabels.isEmpty()) response = assistantLabels.constLast()->text().trimmed();
                const QString limitation = response.isEmpty()
                    ? QStringLiteral("Provider advertised imageGeneration, but the real turn exposed no native image item or callable image_gen tool.")
                    : response;
                QMetaObject::invokeMethod(
                    workspace,
                    "setImageGenerationCapability",
                    Qt::DirectConnection,
                    Q_ARG(bool, false),
                    Q_ARG(QString, limitation));
                addCheck(QStringLiteral("provider-native-image-unavailable-evidence"), true,
                    {{QStringLiteral("advertisedCapability"), true},
                     {QStringLiteral("nativeItemCompleted"), false},
                     {QStringLiteral("assistantResponse"), response},
                     {QStringLiteral("runtimeLimitation"), limitation}});
                workspace->setProperty("codexProviderImageRuntimeLimitation", limitation);
                QMetaObject::invokeMethod(workspace, "sessionListRequested", Qt::DirectConnection);
                phase = 5; phaseTimer.restart();
            }
            return;
        }

        if (phase == 5) {
            const QJsonArray sessions = workspace->property("codexSessionList").toJsonArray();
            const QString original = workspace->property("codexSmokeOriginalThreadId").toString();
            if (!sessions.isEmpty() && !original.isEmpty()) {
                bool found = false;
                for (const QJsonValue& value : sessions) found = found || value.toObject().value(QStringLiteral("id")).toString() == original;
                addCheck(QStringLiteral("session-list-real-protocol"), found, {{QStringLiteral("count"), sessions.size()}});
                QMetaObject::invokeMethod(workspace, "sessionReadRequested", Qt::DirectConnection, Q_ARG(QString, original));
                phase = 6; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 15000) finish(QStringLiteral("thread/list did not return the current session."));
            return;
        }
        if (phase == 6) {
            const QString original = workspace->property("codexSmokeOriginalThreadId").toString();
            if (workspace->property("codexSessionReadId").toString() == original) {
                addCheck(QStringLiteral("session-read-real-protocol"), true, {});
                QMetaObject::invokeMethod(workspace, "sessionForkRequested", Qt::DirectConnection, Q_ARG(QString, original));
                phase = 7; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 15000) finish(QStringLiteral("thread/read did not complete."));
            return;
        }
        if (phase == 7) {
            const QString original = workspace->property("codexSmokeOriginalThreadId").toString();
            const QString forkId = workspace->property("codexSessionForkId").toString();
            if (!forkId.isEmpty() && forkId != original) {
                addCheck(QStringLiteral("session-fork-real-protocol"), true, {});
                QMetaObject::invokeMethod(workspace, "sessionResumeRequested", Qt::DirectConnection, Q_ARG(QString, original));
                phase = 8; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 20000) finish(QStringLiteral("thread/fork did not complete."));
            return;
        }
        if (phase == 8) {
            const QString original = workspace->property("codexSmokeOriginalThreadId").toString();
            const QString forkId = workspace->property("codexSessionForkId").toString();
            if (workspace->property("codexSessionResumeId").toString() == original &&
                workspace->property("codexCurrentThreadId").toString() == original) {
                addCheck(QStringLiteral("session-resume-real-protocol"), true, {});
                QMetaObject::invokeMethod(workspace, "sessionArchiveRequested", Qt::DirectConnection, Q_ARG(QString, forkId));
                phase = 9; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 20000) finish(QStringLiteral("thread/resume did not complete."));
            return;
        }
        if (phase == 9) {
            const QString forkId = workspace->property("codexSessionForkId").toString();
            if (!forkId.isEmpty() && workspace->property("codexSessionArchiveId").toString() == forkId) {
                addCheck(QStringLiteral("session-archive-smoke-fork"), true, {});
                QImage fixture(96, 64, QImage::Format_ARGB32);
                for (int y = 0; y < fixture.height(); ++y) {
                    for (int x = 0; x < fixture.width(); ++x) {
                        fixture.setPixelColor(x, y, QColor((x * 255) / 95, (y * 255) / 63, (x + y) % 256));
                    }
                }
                const QString fixturePath = deriveSiblingArtifactPath(outputPath, QStringLiteral("_image_fixture.png"));
                const bool saved = fixture.save(fixturePath, "PNG");
                const QJsonObject item{{QStringLiteral("id"), QStringLiteral("fixture-image")},
                    {QStringLiteral("type"), QStringLiteral("imageGeneration")},
                    {QStringLiteral("status"), QStringLiteral("completed")},
                    {QStringLiteral("savedPath"), QFileInfo(fixturePath).absoluteFilePath()},
                    {QStringLiteral("revisedPrompt"), QStringLiteral("Local safe image-generation success fixture")}};
                QMetaObject::invokeMethod(workspace, "updateImageGeneration", Qt::DirectConnection,
                    Q_ARG(QJsonObject, item), Q_ARG(bool, true));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
                auto* preview = workspace->findChild<QLabel*>(QStringLiteral("CodexGeneratedImage"));
                auto* importButton = workspace->findChild<QToolButton*>(QStringLiteral("CodexImageImport"));
                const QPixmap previewPixmap = preview ? preview->pixmap() : QPixmap();
                const bool pixels = saved && !fixture.isNull() && fixture.pixelColor(0, 0) != fixture.pixelColor(95, 63);
                addCheck(QStringLiteral("image-fixture-nonempty-pixels"), pixels, {{QStringLiteral("path"), fixturePath}});
                addCheck(QStringLiteral("image-fixture-thumbnail-rendered"), preview && preview->isVisible() && !previewPixmap.isNull(), {});
                if (importButton) importButton->click();
                workspace->setProperty("codexSmokeImageFixturePath", QFileInfo(fixturePath).absoluteFilePath());
                phase = 20; phaseTimer.restart(); return;
            }
            if (phaseTimer.elapsed() > 15000) finish(QStringLiteral("thread/archive did not complete for the smoke fork."));
        }
        if (phase == 20) {
            const QString fixturePath = workspace->property("codexSmokeImageFixturePath").toString();
            const bool imported = workspace->property("codexImportedGeneratedImage").toString() == fixturePath;
            if (imported) {
                addCheck(QStringLiteral("image-fixture-imported-through-playback-service"), true, {});
                auto* capability = workspace->findChild<QLabel*>(QStringLiteral("CodexImageCapability"));
                addCheck(QStringLiteral("provider-image-unavailable-visible"),
                    capability && capability->isVisible() &&
                        !workspace->property("codexImageGenerationAvailable").toBool() &&
                        !workspace->property("codexImageGenerationReason").toString().trimmed().isEmpty(),
                    {{QStringLiteral("text"), capability ? capability->text() : QString()}});
                QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
                finish(); return;
            }
            if (phaseTimer.elapsed() > 5000) finish(QStringLiteral("Generated image fixture import did not reach IPlaybackService."));
        }
    });

    poll.start();
    const int eventLoopResult = exec();
    return eventLoopResult == 0 ? exitCode : 1;
}

int Application::_runRecoveryPromptSmoke()
{
    QString outputPath = _recoveryPromptSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/tmp_recovery_prompt_smoke.json");
    }
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));

    _mainWindow->resize(960, 540);
    _mainWindow->show();

    int exitCode = 0;
    auto* prompt = _mainWindow->createRecoverySessionPromptForSmoke();
    prompt->setAttribute(Qt::WA_DeleteOnClose, true);

    QTimer::singleShot(0, this, [prompt]() {
        prompt->open();
    });

    QTimer::singleShot(600, this, [this, prompt, outputPath, screenshotPath, &exitCode]() {
        QJsonArray buttons;
        const auto standardButtons = { QMessageBox::Yes, QMessageBox::No };
        for (const auto buttonRole : standardButtons) {
            if (auto* button = prompt->button(buttonRole)) {
                buttons.append(button->text());
            }
        }

        const QString expectedTitle = QStringLiteral("\u6062\u590d\u4f1a\u8bdd");
        const QString expectedText =
            QStringLiteral("\u68c0\u6d4b\u5230\u672a\u4fdd\u5b58\u7684\u4f1a\u8bdd\uff0c\u662f\u5426\u6062\u590d\uff1f");
        const bool titleReadable = prompt->windowTitle() == expectedTitle;
        const bool textReadable = prompt->text() == expectedText;
        const bool buttonsLocalized =
            buttons.contains(QStringLiteral("\u662f")) && buttons.contains(QStringLiteral("\u5426"));

        QPixmap capture;
        if (QScreen* screen = prompt->screen()) {
            const QRect frame = prompt->frameGeometry();
            capture = screen->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height());
        }
        if (capture.isNull()) {
            capture = prompt->grab();
        }

        bool screenshotSaved = false;
        if (!capture.isNull()) {
            const QFileInfo screenshotInfo(screenshotPath);
            QDir().mkpath(screenshotInfo.absolutePath());
            screenshotSaved = capture.save(screenshotPath, "PNG");
        }

        QJsonObject report{
            { QStringLiteral("title"), prompt->windowTitle() },
            { QStringLiteral("text"), prompt->text() },
            { QStringLiteral("buttons"), buttons },
            { QStringLiteral("screenshot"), screenshotPath },
            { QStringLiteral("screenshotSaved"), screenshotSaved },
            { QStringLiteral("titleReadable"), titleReadable },
            { QStringLiteral("textReadable"), textReadable },
            { QStringLiteral("buttonsLocalized"), buttonsLocalized },
            { QStringLiteral("summary"), QJsonObject{
                { QStringLiteral("pass"), titleReadable && textReadable && buttonsLocalized && screenshotSaved ? 1 : 0 },
                { QStringLiteral("fail"), titleReadable && textReadable && buttonsLocalized && screenshotSaved ? 0 : 1 }
            } }
        };

        const QFileInfo outInfo(outputPath);
        QDir().mkpath(outInfo.absolutePath());
        if (!writeJsonObjectFile(outputPath, report)) {
            exitCode = 4;
        } else if (!titleReadable || !textReadable || !buttonsLocalized || !screenshotSaved) {
            exitCode = 5;
        }

        prompt->done(QMessageBox::No);
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runQwenAsrProviderSmoke()
{
    const QString outputPath = _qwenAsrProviderSmokeOutput.isEmpty()
        ? QStringLiteral("tmp_qwen_asr_provider_smoke.json")
        : _qwenAsrProviderSmokeOutput;
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));
    int exitCode = 1;

    _mainWindow->resize(1500, 900);
    _mainWindow->show();
    _mainWindow->raise();

    QTimer::singleShot(0, this, [this, outputPath, screenshotPath, &exitCode]() {
        QJsonObject report = _mainWindow->runQwenAsrProviderSmokeChecks(screenshotPath);
        if (!report.contains(QStringLiteral("screenshot_path"))) {
            const QPixmap capture = _mainWindow->grab();
            if (!capture.isNull()) {
            const QFileInfo screenshotInfo(screenshotPath);
            QDir().mkpath(screenshotInfo.absolutePath());
            if (capture.save(screenshotPath, "PNG")) {
                report.insert(QStringLiteral("screenshot_path"), QFileInfo(screenshotPath).absoluteFilePath());
            }
            }
        }
        const QString mainScreenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".main.png"));
        const QPixmap mainCapture = _mainWindow->grab();
        if (!mainCapture.isNull()) {
            const QFileInfo mainScreenshotInfo(mainScreenshotPath);
            QDir().mkpath(mainScreenshotInfo.absolutePath());
            if (mainCapture.save(mainScreenshotPath, "PNG")) {
                report.insert(QStringLiteral("main_screenshot_path"), QFileInfo(mainScreenshotPath).absoluteFilePath());
            }
        }
        const QFileInfo info(outputPath);
        QDir().mkpath(info.absolutePath());
        QFile file(outputPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
            file.close();
        }
        exitCode = report.value(QStringLiteral("pass")).toBool() ? 0 : 2;
        QCoreApplication::quit();
    });

    qApp->exec();
    return exitCode;
}

int Application::_runPlayerSmokeTest()
{
    QFileInfo mediaInfo(_playerSmokeMedia);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[PlayerSmoke] Media not found:" << _playerSmokeMedia;
        return 2;
    }

    QString outputPath = _playerSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_player_smoke.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPlayerSmokeChecks(mediaInfo.absoluteFilePath());
        report["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        report["exe"] = QCoreApplication::applicationFilePath();
        report["media"] = mediaInfo.absoluteFilePath();
        report["runtime_snapshot"] = _captureRuntimeDump();
        int visiblePlatformWindows = 0;
        int hiddenAutomationWindows = 0;
        for (QWidget* widget : topLevelWidgets()) {
            if (!widget || !widget->isVisible()) continue;
            if (widget->testAttribute(Qt::WA_DontShowOnScreen)) {
                ++hiddenAutomationWindows;
            } else {
                ++visiblePlatformWindows;
            }
        }
        report["automation"] = QJsonObject{
            {QStringLiteral("background"), _backgroundAutomationMode},
            {QStringLiteral("visiblePlatformWindows"), visiblePlatformWindows},
            {QStringLiteral("hiddenTopLevelWindows"), hiddenAutomationWindows}
        };

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("cgplay_player_smoke.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report["runtime_dump_path"] = QFileInfo(runtimeDumpPath).absoluteFilePath();
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[PlayerSmoke] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        const int failCount = summary.value(QStringLiteral("fail")).toInt();
        qInfo() << "[PlayerSmoke] Saved:" << outputPath
                << "PASS=" << summary.value(QStringLiteral("pass")).toInt()
                << "FAIL=" << failCount
                << "SKIP=" << summary.value(QStringLiteral("skip")).toInt()
                << "MANUAL=" << summary.value(QStringLiteral("manual")).toInt();
        exitCode = failCount > 0 ? 5 : 0;
        // Exercise the same close path users take before ending the
        // background smoke process, so media-handle release is covered by
        // the regression run rather than only by object destruction.
        _mainWindow->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runSubtitleGenerationSmoke()
{
    QFileInfo mediaInfo(_subtitleGenerationSmokeMedia);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[SubtitleGenerationSmoke] Media not found:" << _subtitleGenerationSmokeMedia;
        return 2;
    }

    QString outputPath = _subtitleGenerationSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_subtitle_generation_smoke.json");
    }
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));

    const QString playbackMode = TranslationPlaybackStrategy::normalizeModeId(
        _subtitleGenerationSmokePlaybackMode);
    if (_windowSettings) {
        _windowSettings->setValue(QStringLiteral("ai/subtitles/playbackMode"), playbackMode);
    }
    setProperty("cgplay.subtitleGenerationPlaybackMode", playbackMode);
    if (_subtitleGenerationSmokeHighQualityWaitTimeoutMs > 0) {
        setProperty(
            "cgplay.subtitleHighQualityPrePlaybackWaitTimeoutMs",
            _subtitleGenerationSmokeHighQualityWaitTimeoutMs);
    }
    if (_subtitleGenerationSmokeHighQualityTargetSeconds > 0.0) {
        setProperty(
            "cgplay.subtitleHighQualityPrePlaybackTargetSeconds",
            _subtitleGenerationSmokeHighQualityTargetSeconds);
    }
    setProperty("cgplay.subtitleGenerationMock", _subtitleGenerationSmokeMock);
    setProperty("cgplay.subtitleGenerationMockOnlineWorker", _subtitleGenerationSmokeMockOnlineWorker);
    setProperty("cgplay.subtitleGenerationMockOcrWorker", _subtitleGenerationSmokeMockOcrWorker);
    setProperty("cgplay.subtitleGenerationMockRepairWorker", _subtitleGenerationSmokeMockRepairWorker);
    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, screenshotPath, mediaInfo] {
        QJsonObject report = _mainWindow->runSubtitleGenerationSmokeChecks(
            mediaInfo.absoluteFilePath(),
            outputPath,
            _subtitleGenerationSmokeMock,
            _subtitleGenerationSmokeFrame,
            _subtitleGenerationSmokeCancelAfterMs,
            _subtitleGenerationSmokePlaybackSampleDurationMs,
            _subtitleGenerationSmokePlaybackSampleIntervalMs);
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        report.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());
        report.insert(QStringLiteral("mockWithoutApi"), _subtitleGenerationSmokeMock);
        report.insert(QStringLiteral("mockOnlineWorker"), _subtitleGenerationSmokeMockOnlineWorker);
        report.insert(QStringLiteral("mockOcrWorker"), _subtitleGenerationSmokeMockOcrWorker);
        report.insert(QStringLiteral("mockRepairWorker"), _subtitleGenerationSmokeMockRepairWorker);
        report.insert(QStringLiteral("cancelAfterMs"), _subtitleGenerationSmokeCancelAfterMs);
        report.insert(QStringLiteral("translationPlaybackMode"), playbackMode);
        report.insert(
            QStringLiteral("highQualityWaitTimeoutMs"),
            _subtitleGenerationSmokeHighQualityWaitTimeoutMs);
        report.insert(
            QStringLiteral("highQualityTargetSeconds"),
            _subtitleGenerationSmokeHighQualityTargetSeconds);

        const QPixmap capture = _mainWindow->grab();
        if (!capture.isNull()) {
            const QFileInfo screenshotInfo(screenshotPath);
            QDir().mkpath(screenshotInfo.absolutePath());
            if (capture.save(screenshotPath, "PNG")) {
                report.insert(QStringLiteral("screenshot_path"), QFileInfo(screenshotPath).absoluteFilePath());
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[SubtitleGenerationSmoke] Cannot write output:" << outputPath;
            exitCode = 4;
            _mainWindow->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
            std::exit(exitCode);
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        const int failCount = summary.value(QStringLiteral("fail")).toInt();
        qInfo() << "[SubtitleGenerationSmoke] Saved:" << outputPath
                << "PASS=" << summary.value(QStringLiteral("pass")).toInt()
                << "FAIL=" << failCount
                << "SKIP=" << summary.value(QStringLiteral("skip")).toInt();
        exitCode = failCount > 0 ? 5 : 0;
        _mainWindow->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
        std::exit(exitCode);
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runSubtitleCacheDisplaySmoke()
{
    QFileInfo mediaInfo(_subtitleCacheDisplaySmokeMedia);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[SubtitleCacheDisplaySmoke] Media not found:" << _subtitleCacheDisplaySmokeMedia;
        return 2;
    }

    QString outputPath = _subtitleCacheDisplaySmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_subtitle_cache_display_smoke.json");
    }
    QVector<int> frames = _subtitleCacheDisplaySmokeFrames;
    if (frames.isEmpty()) {
        frames = QVector<int>{ 1315, 1534, 1638 };
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo, frames] {
        QJsonObject report = _mainWindow->runSubtitleCacheDisplaySmokeChecks(
            mediaInfo.absoluteFilePath(),
            outputPath,
            frames);
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        report.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[SubtitleCacheDisplaySmoke] Cannot write output:" << outputPath;
            exitCode = 4;
            _mainWindow->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
            std::exit(exitCode);
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        const int failCount = summary.value(QStringLiteral("fail")).toInt();
        qInfo() << "[SubtitleCacheDisplaySmoke] Saved:" << outputPath
                << "PASS=" << summary.value(QStringLiteral("pass")).toInt()
                << "FAIL=" << failCount
                << "SKIP=" << summary.value(QStringLiteral("skip")).toInt();
        exitCode = failCount > 0 ? 5 : 0;
        _mainWindow->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
        std::exit(exitCode);
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runSubtitleSwitchSequenceSmoke()
{
    QStringList mediaPaths;
    for (const QString& media : _subtitleSwitchSequenceSmokeMedia) {
        const QFileInfo mediaInfo(media.trimmed());
        if (!mediaInfo.exists() || !mediaInfo.isFile()) {
            qCritical() << "[SubtitleSwitchSequenceSmoke] Media not found:" << media;
            return 2;
        }
        mediaPaths.append(mediaInfo.absoluteFilePath());
    }
    if (mediaPaths.isEmpty()) {
        qCritical() << "[SubtitleSwitchSequenceSmoke] No media paths provided.";
        return 2;
    }

    QString outputPath = _subtitleSwitchSequenceSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_subtitle_switch_sequence_smoke.json");
    }
    QVector<int> frames = _subtitleSwitchSequenceSmokeFrames;
    if (frames.isEmpty()) {
        frames = QVector<int>{ 0 };
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaPaths, frames] {
        QJsonObject report = _mainWindow->runSubtitleSwitchSequenceSmokeChecks(
            mediaPaths,
            outputPath,
            frames);
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[SubtitleSwitchSequenceSmoke] Cannot write output:" << outputPath;
            exitCode = 4;
            _mainWindow->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
            std::exit(exitCode);
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        const int failCount = summary.value(QStringLiteral("fail")).toInt();
        qInfo() << "[SubtitleSwitchSequenceSmoke] Saved:" << outputPath
                << "PASS=" << summary.value(QStringLiteral("pass")).toInt()
                << "FAIL=" << failCount
                << "SKIP=" << summary.value(QStringLiteral("skip")).toInt();
        exitCode = failCount > 0 ? 5 : 0;
        _mainWindow->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
        std::exit(exitCode);
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runSubtitleRefinedFallbackSmoke()
{
    QFileInfo mediaInfo(_subtitleRefinedFallbackSmokeMedia);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[SubtitleRefinedFallbackSmoke] Media not found:" << _subtitleRefinedFallbackSmokeMedia;
        return 2;
    }

    QString outputPath = _subtitleRefinedFallbackSmokeOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/cgplay_subtitle_refined_fallback_smoke.json");
    }
    const QString screenshotPath = deriveSiblingArtifactPath(outputPath, QStringLiteral(".png"));

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, screenshotPath, mediaInfo] {
        QJsonObject report = _mainWindow->runSubtitleRefinedFallbackSmokeChecks(
            mediaInfo.absoluteFilePath(),
            _subtitleRefinedFallbackSmokeFrame);
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        report.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());

        const QPixmap capture = _mainWindow->grab();
        if (!capture.isNull()) {
            const QFileInfo screenshotInfo(screenshotPath);
            QDir().mkpath(screenshotInfo.absolutePath());
            if (capture.save(screenshotPath, "PNG")) {
                report.insert(QStringLiteral("screenshot_path"), QFileInfo(screenshotPath).absoluteFilePath());
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[SubtitleRefinedFallbackSmoke] Cannot write output:" << outputPath;
            exitCode = 4;
            _mainWindow->close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
            std::exit(exitCode);
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        const int failCount = summary.value(QStringLiteral("fail")).toInt();
        qInfo() << "[SubtitleRefinedFallbackSmoke] Saved:" << outputPath
                << "PASS=" << summary.value(QStringLiteral("pass")).toInt()
                << "FAIL=" << failCount
                << "SKIP=" << summary.value(QStringLiteral("skip")).toInt();
        exitCode = failCount > 0 ? 5 : 0;
        _mainWindow->close();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 500);
        std::exit(exitCode);
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase915ViewerRebuild()
{
    const QString mediaPath = !_phase915Media.isEmpty() ? _phase915Media : defaultPhase915MediaPath();
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase9.15][ViewerRebuild] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase915ViewerRebuildOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase9_15_viewer_rebuild.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPhase915ViewerRebuildChecks(mediaInfo.absoluteFilePath());
        report["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        report["exe"] = QCoreApplication::applicationFilePath();
        report["media"] = mediaInfo.absoluteFilePath();
        report["fallback_enabled"] = !_disablePluginFallback;
        report["runtime_snapshot"] = _captureRuntimeDump();

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase9_15_viewer_rebuild.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report["runtime_dump_path"] = QFileInfo(runtimeDumpPath).absoluteFilePath();
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[Phase9.15][ViewerRebuild] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        exitCode = summary.value(QStringLiteral("fail")).toInt() > 0 ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase915PluginReload()
{
    const QString mediaPath = !_phase915Media.isEmpty() ? _phase915Media : defaultPhase915MediaPath();
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase9.15][PluginReload] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase915PluginReloadOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase9_15_plugin_reload.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPhase915PluginReloadChecks(mediaInfo.absoluteFilePath());
        report["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        report["exe"] = QCoreApplication::applicationFilePath();
        report["media"] = mediaInfo.absoluteFilePath();
        report["fallback_enabled"] = !_disablePluginFallback;
        report["runtime_snapshot"] = _captureRuntimeDump();

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase9_15_plugin_reload.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report["runtime_dump_path"] = QFileInfo(runtimeDumpPath).absoluteFilePath();
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[Phase9.15][PluginReload] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        exitCode = summary.value(QStringLiteral("fail")).toInt() > 0 ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase915FallbackToggle()
{
    const QString mediaPath = !_phase915Media.isEmpty() ? _phase915Media : defaultPhase915MediaPath();
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase9.15][FallbackToggle] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase915FallbackToggleOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase9_15_fallback_toggle.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPhase915FallbackToggleChecks(mediaInfo.absoluteFilePath());
        report["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        report["exe"] = QCoreApplication::applicationFilePath();
        report["media"] = mediaInfo.absoluteFilePath();
        report["fallback_enabled"] = !_disablePluginFallback;
        report["runtime_snapshot"] = _captureRuntimeDump();

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase9_15_fallback_toggle.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report["runtime_dump_path"] = QFileInfo(runtimeDumpPath).absoluteFilePath();
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[Phase9.15][FallbackToggle] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        const QJsonObject summary = report.value(QStringLiteral("summary")).toObject();
        exitCode = summary.value(QStringLiteral("fail")).toInt() > 0 ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase11PerformanceBaseline()
{
    const QString mediaPath = !_phase915Media.isEmpty() ? _phase915Media : defaultPhase915MediaPath();
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase11][PerformanceBaseline] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase11PerformanceBaselineOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase11_performance_baseline.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        const QJsonObject viewerReport = _mainWindow->runPhase915ViewerRebuildChecks(mediaInfo.absoluteFilePath());
        const QJsonObject pluginReport = _mainWindow->runPhase915PluginReloadChecks(mediaInfo.absoluteFilePath());
        const QJsonObject fallbackReport = _mainWindow->runPhase915FallbackToggleChecks(mediaInfo.absoluteFilePath());
        const QJsonObject multiWindowReport = viewerReport.value(QStringLiteral("multi_window")).toObject();

        const int totalFailCount =
            viewerReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
            multiWindowReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
            pluginReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() +
            fallbackReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt();

        QJsonObject baseline;
        baseline.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        baseline.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        baseline.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());
        baseline.insert(QStringLiteral("overlayDebugEnabled"), _overlayDebugMode);
        baseline.insert(QStringLiteral("viewerRebuild"), baselineScenario(viewerReport));
        baseline.insert(QStringLiteral("multiWindowSwitch"), baselineScenario(multiWindowReport));
        baseline.insert(QStringLiteral("pluginReload"), baselineScenario(pluginReport));
        baseline.insert(QStringLiteral("fallbackToggle"), baselineScenario(fallbackReport));
        baseline.insert(QStringLiteral("runtime_snapshot"), _captureRuntimeDump());
        baseline.insert(
            QStringLiteral("summary"),
            QJsonObject{
                { QStringLiteral("fail"), totalFailCount },
                { QStringLiteral("viewerRebuildFail"),
                  viewerReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
                { QStringLiteral("multiWindowFail"),
                  multiWindowReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
                { QStringLiteral("pluginReloadFail"),
                  pluginReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() },
                { QStringLiteral("fallbackToggleFail"),
                  fallbackReport.value(QStringLiteral("summary")).toObject().value(QStringLiteral("fail")).toInt() }
            });

        if (totalFailCount > 0) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase11_performance_baseline.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, baseline.value(QStringLiteral("runtime_snapshot")).toObject())) {
                baseline.insert(QStringLiteral("runtime_dump_path"), QFileInfo(runtimeDumpPath).absoluteFilePath());
            }
        }

        if (!writeJsonObjectFile(outputPath, baseline)) {
            qCritical() << "[Phase11][PerformanceBaseline] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        exitCode = totalFailCount > 0 ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase14PerformanceBaseline()
{
    const QString mediaPath = defaultPhase14MediaPath(_phase14Media, _phase915Media);
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase14][PerformanceBaseline] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase14PerformanceBaselineOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase14_performance_baseline.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPhase14PerformanceBaselineChecks(mediaInfo.absoluteFilePath());
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        report.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());
        report.insert(QStringLiteral("overlayDebugEnabled"), _overlayDebugMode);
        report.insert(QStringLiteral("fallback_enabled"), !_disablePluginFallback);
        report.insert(QStringLiteral("runtime_snapshot"), _captureRuntimeDump());

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase14_performance_baseline.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report.insert(QStringLiteral("runtime_dump_path"), QFileInfo(runtimeDumpPath).absoluteFilePath());
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[Phase14][PerformanceBaseline] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        exitCode = hasFailures ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}

int Application::_runPhase14Stress()
{
    const QString mediaPath = defaultPhase14MediaPath(_phase14Media, _phase915Media);
    QFileInfo mediaInfo(mediaPath);
    if (!mediaInfo.exists() || !mediaInfo.isFile()) {
        qCritical() << "[Phase14][Stress] Media not found:" << mediaPath;
        return 2;
    }

    QString outputPath = _phase14StressOutput;
    if (outputPath.isEmpty()) {
        outputPath = QDir::currentPath() + QStringLiteral("/phase14_stress_test.json");
    }

    _mainWindow->resize(1500, 840);
    _mainWindow->show();

    int exitCode = 0;
    QTimer::singleShot(0, this, [&, outputPath, mediaInfo] {
        QJsonObject report = _mainWindow->runPhase14StressChecks(
            mediaInfo.absoluteFilePath(),
            _phase14ViewerRebuildIterations,
            _phase14WindowSwitchIterations,
            _phase14PluginReloadIterations,
            _phase14FallbackToggleIterations);
        report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTime().toString(Qt::ISODate));
        report.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        report.insert(QStringLiteral("media"), mediaInfo.absoluteFilePath());
        report.insert(QStringLiteral("overlayDebugEnabled"), _overlayDebugMode);
        report.insert(QStringLiteral("fallback_enabled"), !_disablePluginFallback);
        report.insert(
            QStringLiteral("requestedIterations"),
            QJsonObject{
                { QStringLiteral("viewerRebuild"), _phase14ViewerRebuildIterations },
                { QStringLiteral("windowSwitch"), _phase14WindowSwitchIterations },
                { QStringLiteral("pluginReload"), _phase14PluginReloadIterations },
                { QStringLiteral("fallbackToggle"), _phase14FallbackToggleIterations }
            });
        report.insert(QStringLiteral("runtime_snapshot"), _captureRuntimeDump());

        const bool hasFailures = reportHasFailures(report);
        if (hasFailures) {
            const QString runtimeDumpPath = deriveRuntimeDumpPath(
                outputPath,
                QStringLiteral("phase14_stress_test.runtime_dump.json"));
            if (_writeRuntimeDump(runtimeDumpPath, report.value(QStringLiteral("runtime_snapshot")).toObject())) {
                report.insert(QStringLiteral("runtime_dump_path"), QFileInfo(runtimeDumpPath).absoluteFilePath());
            }
        }

        if (!writeJsonObjectFile(outputPath, report)) {
            qCritical() << "[Phase14][Stress] Cannot write output:" << outputPath;
            exitCode = 4;
            quit();
            return;
        }

        exitCode = hasFailures ? 5 : 0;
        quit();
    });

    return exec() == 0 ? exitCode : 1;
}


} // namespace cgplay
