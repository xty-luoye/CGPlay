#pragma once

#include <QString>

namespace cgplay {

class ISessionService
{
public:
    virtual ~ISessionService() = default;

    virtual QString sessionPath() const = 0;
    virtual bool hasUnsavedChanges() const = 0;
    virtual bool hasSession() const = 0;

    virtual bool save(const QString& path = {}) = 0;
    virtual bool load(const QString& path) = 0;
    virtual bool saveAs() = 0;

    virtual void enableAutoSave(int intervalMs = 300000) = 0;
    virtual void disableAutoSave() = 0;
    virtual bool isAutoSaveEnabled() const = 0;

    virtual QString recoveryPath() const = 0;
    virtual void saveRecovery() = 0;
    virtual bool hasRecoverySession() const = 0;
    virtual bool loadRecovery() = 0;
    virtual void clearRecovery() = 0;

    virtual void newSession() = 0;
    virtual void markDirty() = 0;
};

} // namespace cgplay
