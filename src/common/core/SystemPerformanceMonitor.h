#pragma once

#include <QObject>

namespace cgplay {

class SystemPerformanceMonitor : public QObject
{
    Q_OBJECT
public:
    explicit SystemPerformanceMonitor(QObject* parent = nullptr);
    ~SystemPerformanceMonitor() override;

    void start(int intervalMs = 1000);
    void stop();

Q_SIGNALS:
    void sampleReady(double cpuPercent, double gpuPercent, double memoryPercent);

private:
    void _sample();

    class Private;
    Private* _p = nullptr;
};

} // namespace cgplay
