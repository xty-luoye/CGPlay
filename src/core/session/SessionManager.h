#pragma once
// CGPlay SessionManager.h
// v1.4 — .cgsession 工程文件：保存/恢复全部审片状态

#include "session/api/ISessionService.h"

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QTimer>
#include <memory>

namespace cgplay {

class PlaylistModel;
class AnnotationManager;
class OcioManager;
class PlaybackController;
class CompareToolbar;
class MainWindow;

// ─── SessionManager ──────────────────────────────────────────────────────────
class SessionManager : public QObject, public ISessionService
{
    Q_OBJECT
public:
    explicit SessionManager(MainWindow* mainWindow,
                            std::shared_ptr<AnnotationManager> annoMgr,
                            std::shared_ptr<OcioManager>       ocioMgr,
                            QObject* parent = nullptr);
    ~SessionManager() override;

    // ── 会话路径 ──────────────────────────────────────────────────────────
    QString sessionPath() const override { return _sessionPath; }
    bool    hasUnsavedChanges() const override { return _dirty; }
    bool    hasSession() const override { return !_sessionPath.isEmpty(); }

    // ── 保存/加载 ─────────────────────────────────────────────────────────
    bool save(const QString& path = {}) override;
    bool load(const QString& path) override;
    bool saveAs() override;

    // ── 自动保存 ──────────────────────────────────────────────────────────
    void enableAutoSave(int intervalMs = 300000) override;  // 默认 5 分钟
    void disableAutoSave() override;
    bool isAutoSaveEnabled() const override { return _autoSaveTimer != nullptr; }

    // ── 崩溃恢复 ──────────────────────────────────────────────────────────
    QString recoveryPath() const override;         // 崩溃恢复文件路径
    void saveRecovery() override;                  // 保存恢复快照
    bool hasRecoverySession() const override;
    bool loadRecovery() override;
    void clearRecovery() override;

    // ── 新建会话 ──────────────────────────────────────────────────────────
    void newSession() override;

public Q_SLOTS:
    void markDirty() override;                      // 标记为已修改

Q_SIGNALS:
    void sessionSaved(const QString& path);
    void sessionLoaded(const QString& path);
    void dirtyChanged(bool dirty);

private:
    QJsonObject _serialize() const;
    bool        _deserialize(const QJsonObject& root);
    void        _setDirty(bool dirty);

    MainWindow*                      _mainWindow;
    std::shared_ptr<AnnotationManager> _annoMgr;
    std::shared_ptr<OcioManager>       _ocioMgr;

    QString  _sessionPath;
    bool     _dirty = false;
    QTimer*  _autoSaveTimer = nullptr;
};

} // namespace cgplay
