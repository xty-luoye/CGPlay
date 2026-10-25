#pragma once
#include "api/RecordingTypes.h"
#include <QObject>
#include <memory>

namespace cgplay {

// qApp-owned GUI-thread facade. Recording and file IO occur in isolated
// processes; ordinary window close requests never synchronously wait here.
class RecordingService final : public QObject {
    Q_OBJECT
public:
    explicit RecordingService(QObject* parent = nullptr);
    ~RecordingService() override;
    void refreshCatalog(const RecordingRuntimeSnapshot&, bool synthetic = false);
    bool start(const RecordingStartRequest&, QString* error = nullptr);
    bool recover(const RecordingRecoveryRequest&, QString* error = nullptr);
    bool isBusy() const;
    QJsonObject status() const;
    static bool hasActiveSessions();
    static void shutdownAndDrainAll(int timeoutMs = 30000);

public Q_SLOTS:
    void setPaused(bool paused);
    void setAudio(const QJsonObject& values);
    void setPlayerCrop(const QJsonObject& geometry);
    void stop();
    void requestStopForClose();

Q_SIGNALS:
    void catalogReady(const QJsonObject& catalog);
    void stateChanged(const QJsonObject& status);
    void finished(const QJsonObject& result);
    void closeReady();

private:
    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
