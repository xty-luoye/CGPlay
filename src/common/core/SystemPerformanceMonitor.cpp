#include "SystemPerformanceMonitor.h"

#include <QTimer>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#ifndef PDH_MORE_DATA
#define PDH_MORE_DATA 0x800007D2L
#endif
#include <pdh.h>
#include <psapi.h>
#include <vector>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace cgplay {

class SystemPerformanceMonitor::Private
{
public:
    QTimer* timer = nullptr;

#ifdef Q_OS_WIN
    quint64 lastIdle100ns = 0;
    quint64 lastKernel100ns = 0;
    quint64 lastUser100ns = 0;
    quint64 lastProcessKernel100ns = 0;
    quint64 lastProcessUser100ns = 0;
    quint64 lastProcessWall100ns = 0;
    PDH_HQUERY gpuQuery = nullptr;
    PDH_HCOUNTER gpuCounter = nullptr;
#endif
};

namespace {

#ifdef Q_OS_WIN
quint64 _fileTimeToUInt64(const FILETIME& value)
{
    ULARGE_INTEGER out;
    out.LowPart = value.dwLowDateTime;
    out.HighPart = value.dwHighDateTime;
    return out.QuadPart;
}

double _sampleSystemCpuPercent(quint64& lastIdle100ns, quint64& lastKernel100ns, quint64& lastUser100ns)
{
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return 0.0;
    }

    const quint64 idleNow = _fileTimeToUInt64(idleTime);
    const quint64 kernelNow = _fileTimeToUInt64(kernelTime);
    const quint64 userNow = _fileTimeToUInt64(userTime);

    if (!lastKernel100ns && !lastUser100ns) {
        lastIdle100ns = idleNow;
        lastKernel100ns = kernelNow;
        lastUser100ns = userNow;
        return 0.0;
    }

    const quint64 idleDelta = idleNow - lastIdle100ns;
    const quint64 kernelDelta = kernelNow - lastKernel100ns;
    const quint64 userDelta = userNow - lastUser100ns;
    const quint64 totalDelta = kernelDelta + userDelta;

    lastIdle100ns = idleNow;
    lastKernel100ns = kernelNow;
    lastUser100ns = userNow;

    if (!totalDelta || idleDelta > totalDelta) {
        return 0.0;
    }

    const double busyDelta = static_cast<double>(totalDelta - idleDelta);
    const double usage = busyDelta * 100.0 / static_cast<double>(totalDelta);
    return qBound(0.0, usage, 100.0);
}

double _sampleProcessCpuPercent(quint64& lastKernel, quint64& lastUser, quint64& lastWall)
{
    FILETIME creation, exit, kernel, user, wall;
    GetSystemTimeAsFileTime(&wall);
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0.0;
    const quint64 kernelNow = _fileTimeToUInt64(kernel);
    const quint64 userNow = _fileTimeToUInt64(user);
    const quint64 wallNow = _fileTimeToUInt64(wall);
    if (!lastWall) { lastKernel = kernelNow; lastUser = userNow; lastWall = wallNow; return 0.0; }
    const quint64 processDelta = (kernelNow - lastKernel) + (userNow - lastUser);
    const quint64 wallDelta = wallNow - lastWall;
    lastKernel = kernelNow; lastUser = userNow; lastWall = wallNow;
    SYSTEM_INFO info; GetSystemInfo(&info);
    const double cores = qMax<DWORD>(1, info.dwNumberOfProcessors);
    return wallDelta ? qBound(0.0, processDelta * 100.0 / (wallDelta * cores), 100.0) : 0.0;
}
#endif

#ifdef Q_OS_WIN
void _closeGpuQuery(PDH_HQUERY& query, PDH_HCOUNTER& counter)
{
    if (query) {
        PdhCloseQuery(query);
        query = nullptr;
        counter = nullptr;
    }
}

bool _ensureGpuQuery(PDH_HQUERY& query, PDH_HCOUNTER& counter)
{
    if (query && counter) {
        return true;
    }

    if (ERROR_SUCCESS != PdhOpenQueryW(nullptr, 0, &query)) {
        _closeGpuQuery(query, counter);
        return false;
    }

    const PDH_STATUS addStatus = PdhAddEnglishCounterW(
        query,
        L"\\GPU Engine(*)\\Utilization Percentage",
        0,
        &counter);
    if (ERROR_SUCCESS != addStatus) {
        _closeGpuQuery(query, counter);
        return false;
    }

    if (ERROR_SUCCESS != PdhCollectQueryData(query)) {
        _closeGpuQuery(query, counter);
        return false;
    }
    return true;
}

double _sampleSystemGpuPercent(PDH_HQUERY& query, PDH_HCOUNTER& counter)
{
    if (!_ensureGpuQuery(query, counter)) {
        return 0.0;
    }

    const PDH_STATUS collectStatus = PdhCollectQueryData(query);
    if (ERROR_SUCCESS != collectStatus) {
        _closeGpuQuery(query, counter);
        return 0.0;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        counter,
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        nullptr);
    if (status != PDH_MORE_DATA || !bufferSize) {
        return 0.0;
    }

    std::vector<BYTE> buffer(bufferSize);
    auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        counter,
        PDH_FMT_DOUBLE,
        &bufferSize,
        &itemCount,
        items);
    if (ERROR_SUCCESS != status) {
        return 0.0;
    }

    double busiestEngine = 0.0;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (!items[i].szName || items[i].FmtValue.CStatus != ERROR_SUCCESS) {
            continue;
        }
        busiestEngine = qMax(busiestEngine, items[i].FmtValue.doubleValue);
    }
    return qBound(0.0, busiestEngine, 100.0);
}

double _sampleSystemMemoryPercent()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return 0.0;
    }
    return qBound(0.0, static_cast<double>(status.dwMemoryLoad), 100.0);
}
#endif

} // namespace

SystemPerformanceMonitor::SystemPerformanceMonitor(QObject* parent)
    : QObject(parent)
    , _p(new Private)
{
    _p->timer = new QTimer(this);
    connect(_p->timer, &QTimer::timeout, this, [this] { _sample(); });
}

SystemPerformanceMonitor::~SystemPerformanceMonitor()
{
#ifdef Q_OS_WIN
    _closeGpuQuery(_p->gpuQuery, _p->gpuCounter);
#endif
    delete _p;
}

void SystemPerformanceMonitor::start(int intervalMs)
{
    const bool wasActive = _p->timer->isActive();
    _p->timer->start(qMax(250, intervalMs));
    if (!wasActive) {
        // Windows PDH/GPU enumeration can block for hundreds of milliseconds.
        // Do not run it inside MainWindow construction; the labels already have
        // safe defaults and will be refreshed once the event loop is running.
        QTimer::singleShot(250, this, [this] {
            if (_p->timer->isActive()) {
                _sample();
            }
        });
    }
}

void SystemPerformanceMonitor::stop()
{
    _p->timer->stop();
}

void SystemPerformanceMonitor::_sample()
{
#ifdef Q_OS_WIN
    const double cpuPercent = _sampleSystemCpuPercent(
        _p->lastIdle100ns,
        _p->lastKernel100ns,
        _p->lastUser100ns);
    const double gpuPercent = _sampleSystemGpuPercent(_p->gpuQuery, _p->gpuCounter);
    const double memoryPercent = _sampleSystemMemoryPercent();
#else
    const double cpuPercent = 0.0;
    const double gpuPercent = 0.0;
    const double memoryPercent = 0.0;
#endif
    Q_EMIT sampleReady(cpuPercent, gpuPercent, memoryPercent);
}

} // namespace cgplay
