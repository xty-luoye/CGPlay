#include "QuickLookApp.h"

#include "ExplorerSelection.h"
#include "QuickLookDebug.h"
#include "QuickLookKeyboardHook.h"
#include "PreviewWindow.h"
#include "common/core/ServiceLocator.h"
#include "cache/CacheManager.h"
#include "ocio/OcioManager.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QMetaObject>
#include <QStyle>
#include <QSystemTrayIcon>

#include <windows.h>

namespace cgplay::quicklook {

namespace {

constexpr DWORD kSpaceVk = VK_SPACE;
constexpr DWORD kEscapeVk = VK_ESCAPE;

} // namespace

QuickLookApp::QuickLookApp(QObject* parent)
    : QObject(parent)
{
    _cache = ServiceLocator::getSharedService<CacheManager>();
    if (!_cache) {
        _cache = std::make_shared<CacheManager>();
        ServiceLocator::registerService<CacheManager>(_cache);
    }
    _ocio = ServiceLocator::getSharedService<OcioManager>();
    if (!_ocio) {
        _ocio = std::make_shared<OcioManager>();
        _ocio->setEnabled(false);
        ServiceLocator::registerService<OcioManager>(_ocio);
    }
    _window = new PreviewWindow(_cache, _ocio);
    _hook = new QuickLookKeyboardHook(this);
    _setupTray();
}

QuickLookApp::~QuickLookApp()
{
    _spacePressed = false;
    if (_hook && _hook->isRunning()) {
        _hook->stop();
    }
    if (_trayIcon) {
        _trayIcon->hide();
    }
}

bool QuickLookApp::start()
{
    const bool ok = _hook && _hook->start([this](WPARAM wParam, const KBDLLHOOKSTRUCT* hookData) {
        return _handleKeyEvent(wParam, hookData);
    });
    logQuickLook(QStringLiteral("QuickLookApp::start installHook=%1").arg(ok));
    if (ok) {
        _showTrayMessage();
    }
    return ok;
}

bool QuickLookApp::_handleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT* hookData)
{
    if (!hookData) {
        return false;
    }

    switch (hookData->vkCode) {
    case kSpaceVk:
        if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            _spacePressed = false;
            logQuickLook(QStringLiteral("Space up; previewForeground=%1").arg(_isPreviewWindowForeground()));
            return _isPreviewWindowForeground();
        }
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            const bool explorerForeground = isExplorerForegroundWindow();
            const bool previewForeground = _isPreviewWindowForeground();
            const bool explorerTextInput = explorerForeground && isExplorerTextInputFocused();
            const bool shouldConsume = previewForeground || (explorerForeground && !explorerTextInput);
            logQuickLook(
                QStringLiteral("Space down; explorerForeground=%1 previewForeground=%2 explorerTextInput=%3 consume=%4")
                    .arg(explorerForeground)
                    .arg(previewForeground)
                    .arg(explorerTextInput)
                    .arg(shouldConsume));
            if (!shouldConsume) {
                _spacePressed = false;
                return false;
            }
            if (_spacePressed) {
                return true;
            }
            _spacePressed = true;
            QMetaObject::invokeMethod(this, [this] {
                logQuickLook(QStringLiteral("Queued Space action on Qt event loop"));
                _togglePreview();
            }, Qt::QueuedConnection);
            return true;
        }
        return false;
    case kEscapeVk:
        if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
            _window && _window->isVisible() && _isPreviewWindowForeground()) {
            logQuickLook(QStringLiteral("Escape down; closing preview"));
            QMetaObject::invokeMethod(this, [this] {
                _closePreview();
            }, Qt::QueuedConnection);
            return true;
        }
        return false;
    default:
        return false;
    }
}

void QuickLookApp::_togglePreview()
{
    if (_window && _window->isVisible() && _isPreviewWindowForeground() && _window->hasFile()) {
        logQuickLook(QStringLiteral("Toggle preview playback for current file"));
        _window->togglePlayback();
        return;
    }

    const QString path = currentExplorerSelection();
    logQuickLook(QStringLiteral("Toggle preview; selectedPath=%1").arg(path.isEmpty() ? QStringLiteral("<empty>") : path));
    if (path.isEmpty()) {
        return;
    }

    if (!_isVideoFile(path)) {
        if (_window && _window->isVisible()) {
            _window->setStatusText(QString::fromUtf8("仅支持视频文件预览"));
        }
        return;
    }

    if (_window->isShowingFile(path)) {
        logQuickLook(QStringLiteral("Selected file already showing; toggling playback"));
        _window->togglePlayback();
        return;
    }

    _window->setStatusText(QString::fromUtf8("Space 播放/暂停   Esc 关闭"));
    logQuickLook(QStringLiteral("Opening preview for %1").arg(path));
    _window->openFile(path, true);
}

void QuickLookApp::_closePreview()
{
    _spacePressed = false;
    if (_window && _window->isVisible()) {
        _window->closePreview();
    }
}

bool QuickLookApp::_registerHotkey()
{
    return true;
}

void QuickLookApp::_unregisterHotkey()
{
}

bool QuickLookApp::_isPreviewWindowForeground() const
{
    if (!_window || !_window->isVisible()) {
        return false;
    }

    const HWND previewWindow = reinterpret_cast<HWND>(_window->winId());
    return GetForegroundWindow() == previewWindow;
}

bool QuickLookApp::_isVideoFile(const QString& path) const
{
    const QString ext = QFileInfo(path).suffix().toLower();
    // Keep this list aligned with MediaProbe::videoExtensions(). QuickLook
    // must accept every container the main player can open.
    static const QStringList kVideoExtensions = {
        QStringLiteral("3g2"), QStringLiteral("3gp"), QStringLiteral("asf"),
        QStringLiteral("avi"), QStringLiteral("divx"), QStringLiteral("dv"),
        QStringLiteral("f4v"), QStringLiteral("flv"), QStringLiteral("ivf"),
        QStringLiteral("m1v"), QStringLiteral("m2ts"), QStringLiteral("m2v"),
        QStringLiteral("m4v"), QStringLiteral("mj2"), QStringLiteral("mkv"),
        QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("mpeg"),
        QStringLiteral("mpg"), QStringLiteral("mts"), QStringLiteral("mxf"),
        QStringLiteral("ogv"), QStringLiteral("prores"), QStringLiteral("rm"),
        QStringLiteral("rmvb"), QStringLiteral("ts"), QStringLiteral("vob"),
        QStringLiteral("webm"), QStringLiteral("wmv"), QStringLiteral("wtv"),
        QStringLiteral("y4m")
    };
    return kVideoExtensions.contains(ext);
}

void QuickLookApp::_setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    auto icon = QIcon(QStringLiteral(":/cgplay/CGPlay.png"));
    if (icon.isNull()) {
        icon = qApp->style()->standardIcon(QStyle::SP_MediaPlay);
    }

    _trayIcon = new QSystemTrayIcon(icon, this);
    _trayIcon->setToolTip(QString::fromUtf8("CGPlay QuickLook"));

    _trayMenu = new QMenu();
    _trayMenu->addAction(QStringLiteral("使用说明"), this, [this] {
        _showTrayMessage();
    });
    _trayMenu->addSeparator();
    _trayMenu->addAction(QStringLiteral("退出 QuickLook"), qApp, &QApplication::quit);
    _trayIcon->setContextMenu(_trayMenu);
    _trayIcon->show();

    connect(_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            _showTrayMessage();
        }
    });
}

void QuickLookApp::_showTrayMessage() const
{
    if (!_trayIcon) {
        return;
    }

    _trayIcon->showMessage(
        QStringLiteral("CGPlay QuickLook 已启动"),
        QStringLiteral("在 Windows 资源管理器中选中视频或图像序列后按 Space 预览；按 Esc 关闭。完整审片、批注与导出请打开 CGPlay。"),
        QSystemTrayIcon::Information,
        5000);
}

} // namespace cgplay::quicklook
