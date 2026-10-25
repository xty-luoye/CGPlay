#pragma once

#include <QObject>
#include <QJsonObject>

namespace cgplay {

class SystemPerformanceMonitor : public QObject
{
    Q_OBJECT
public:
    explicit SystemPerformanceMonitor(QObject* parent = nullptr);
    ~SystemPerformanceMonitor() override;

    void start(int intervalMs = 1000);
    void stop();
    QJsonObject diagnostics() const;

    // Call after the interactive event loop stops, before QApplication teardown.
    // stop() itself never waits for a native PDH call on the GUI thread.
    static void shutdownWorkers();
    static int activeWorkerCount();

Q_SIGNALS:
    void sampleReady(double cpuPercent, double gpuPercent, double memoryPercent);

private:
    void _sample();

    class Private;
    Private* _p = nullptr;
};

} // namespace cgplay
