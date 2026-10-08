#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include <windows.h>

class QMenu;
class QSystemTrayIcon;

namespace cgplay {
class CacheManager;
class OcioManager;
}

namespace cgplay::quicklook {

class PreviewWindow;
class QuickLookKeyboardHook;

class QuickLookApp : public QObject
{
    Q_OBJECT
public:
    explicit QuickLookApp(QObject* parent = nullptr);
    ~QuickLookApp() override;

    bool start();

private:
    bool _handleKeyEvent(WPARAM wParam, const KBDLLHOOKSTRUCT* hookData);
    void _togglePreview();
    void _closePreview();
    bool _registerHotkey();
    void _unregisterHotkey();
    bool _isPreviewWindowForeground() const;
    bool _isVideoFile(const QString& path) const;
    void _setupTray();
    void _showTrayMessage() const;

    std::shared_ptr<CacheManager> _cache;
    std::shared_ptr<OcioManager> _ocio;
    PreviewWindow* _window = nullptr;
    QuickLookKeyboardHook* _hook = nullptr;
    QSystemTrayIcon* _trayIcon = nullptr;
    QMenu* _trayMenu = nullptr;
    bool _hotkeyRegistered = false;
    bool _spacePressed = false;
};

} // namespace cgplay::quicklook
