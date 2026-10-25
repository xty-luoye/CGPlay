#include "SystemPerformanceMonitor.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

namespace {
struct SamplerCounters {
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

struct SampleValues {
    double cpu = 0.0;
    double gpu = 0.0;
    double memory = 0.0;
    double durationMs = 0.0;
    quint64 sequence = 0;
};

SampleValues collectSample(SamplerCounters& counters)
{
    SampleValues sample;
#ifdef Q_OS_WIN
    sample.cpu = _sampleSystemCpuPercent(counters.lastIdle100ns,
        counters.lastKernel100ns, counters.lastUser100ns);
    sample.gpu = _sampleSystemGpuPercent(counters.gpuQuery, counters.gpuCounter);
    sample.memory = _sampleSystemMemoryPercent();
#endif
    return sample;
}

struct SamplerSession {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopRequested = false;
    int intervalMs = 1000;
    quint64 revision = 0;
    std::atomic_bool finished{false};
    std::atomic_int inFlight{0};
    std::atomic_int maxConcurrentSamples{0};
    std::thread worker;
    std::function<void()> notify;
    std::mutex mailboxMutex;
    SampleValues latest;
    bool pendingNotification = false;
    quint64 superseded = 0;
    QString samplerThreadId;
    double maxSampleMs = 0.0;

    void requestStop() {
        { std::lock_guard<std::mutex> lock(mutex); stopRequested = true; }
        wake.notify_one();
    }
    bool stopping() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopRequested;
    }
    void setInterval(int value) {
        { std::lock_guard<std::mutex> lock(mutex); intervalMs = value; ++revision; }
        wake.notify_one();
    }
    void run() {
        // Native handles, CPU baselines and buffer allocations belong only to
        // this worker. Delivery only posts to a separate context; window guards
        // are inspected on the consumer thread, never inside native sampling.
        SamplerCounters counters;
        {
            std::lock_guard<std::mutex> lock(mailboxMutex);
            samplerThreadId = QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()));
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopRequested; });
        }
        while (!stopping()) {
            const auto started = std::chrono::steady_clock::now();
            const int concurrent = inFlight.fetch_add(1) + 1;
            int previousMax = maxConcurrentSamples.load();
            while (previousMax < concurrent &&
                !maxConcurrentSamples.compare_exchange_weak(previousMax, concurrent)) {}
            SampleValues sample;
            try { sample = collectSample(counters); }
            catch (...) { /* A failed native sample must not terminate the worker. */ }
            inFlight.fetch_sub(1);
            sample.durationMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            bool post = false;
            {
                std::lock_guard<std::mutex> lock(mailboxMutex);
                sample.sequence = latest.sequence + 1;
                latest = sample;
                maxSampleMs = std::max(maxSampleMs, sample.durationMs);
                if (pendingNotification) ++superseded;
                else { pendingNotification = true; post = true; }
            }
            if (post) notify(); // Never invoke external code while holding a lock.
            std::unique_lock<std::mutex> lock(mutex);
            auto seenRevision = revision;
            while (!stopRequested) {
                if (!wake.wait_for(lock, std::chrono::milliseconds(intervalMs),
                    [this, seenRevision] { return stopRequested || revision != seenRevision; })) break;
                seenRevision = revision;
            }
        }
#ifdef Q_OS_WIN
        _closeGpuQuery(counters.gpuQuery, counters.gpuCounter);
#endif
        finished.store(true);
        notify(); // Also wakes a pending stop/start handoff with no GUI polling.
    }
};

// A consumer can disappear during PDH enumeration. Keep the thread and delivery
// context independent of that window, and join every worker before app teardown.
struct SamplerRegistry {
    std::mutex mutex;
    std::vector<std::shared_ptr<SamplerSession>> sessions;
    std::unique_ptr<QObject> deliveryContext;
    std::atomic_bool closing{false};
    ~SamplerRegistry() { shutdown(); }
    QObject* context() {
        if (!deliveryContext) deliveryContext = std::make_unique<QObject>();
        return deliveryContext.get();
    }
    void reap() {
        std::vector<std::shared_ptr<SamplerSession>> completed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = sessions.begin(); it != sessions.end();) {
                if ((*it)->finished.load()) {
                    completed.push_back(*it);
                    it = sessions.erase(it);
                } else ++it;
            }
        }
        // Even a finished worker may still be returning from its final queued
        // notification. Never join while holding the registry mutex.
        for (const auto& session : completed)
            if (session->worker.joinable()) session->worker.join();
    }
    void shutdown() {
        closing.store(true);
        std::vector<std::shared_ptr<SamplerSession>> owned;
        { std::lock_guard<std::mutex> lock(mutex); owned.swap(sessions); }
        for (const auto& session : owned) session->requestStop();
        for (const auto& session : owned) if (session->worker.joinable()) session->worker.join();
        deliveryContext.reset();
    }
};

SamplerRegistry& registry() {
    static SamplerRegistry value;
    return value;
}

} // namespace

class SystemPerformanceMonitor::Private {
public:
    std::shared_ptr<SamplerSession> session;
    bool active = false;
    bool restartPending = false;
    int intervalMs = 1000;
    quint64 deliveredSequence = 0;
};

SystemPerformanceMonitor::SystemPerformanceMonitor(QObject* parent)
    : QObject(parent)
    , _p(new Private)
{}

SystemPerformanceMonitor::~SystemPerformanceMonitor()
{
    stop();
    delete _p;
}

void SystemPerformanceMonitor::start(int intervalMs)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto& owner = registry();
    if (owner.closing.load()) return;
    _p->active = true;
    _p->intervalMs = qMax(250, intervalMs);
    if (_p->session && !_p->session->finished.load()) {
        if (_p->session->stopping()) _p->restartPending = true;
        else _p->session->setInterval(_p->intervalMs);
        return;
    }
    owner.reap();
    _p->restartPending = false;
    _p->deliveredSequence = 0;
    auto session = std::make_shared<SamplerSession>();
    session->intervalMs = _p->intervalMs;
    QObject* context = owner.context();
    const QPointer<SystemPerformanceMonitor> receiver(this);
    session->notify = [receiver, context] {
        if (registry().closing.load()) return;
        QMetaObject::invokeMethod(context, [receiver] {
            if (receiver) receiver->_sample();
            registry().reap();
        }, Qt::QueuedConnection);
    };
    _p->session = session;
    { std::lock_guard<std::mutex> lock(owner.mutex); owner.sessions.push_back(session); }
    try { session->worker = std::thread([session] { session->run(); }); }
    catch (const std::exception& error) {
        session->finished.store(true);
        _p->active = false;
        owner.reap();
        qWarning() << "[PerformanceMonitor] Cannot start sampling worker:" << error.what();
    }
}

void SystemPerformanceMonitor::stop()
{
    _p->active = false;
    _p->restartPending = false;
    if (_p->session) _p->session->requestStop();
}

void SystemPerformanceMonitor::_sample()
{
    if (!_p->session) return;
    if (_p->restartPending) {
        if (_p->active && _p->session->finished.load()) start(_p->intervalMs);
        return; // An in-flight sample from the stopped session is stale.
    }
    SampleValues sample;
    {
        std::lock_guard<std::mutex> lock(_p->session->mailboxMutex);
        sample = _p->session->latest;
        _p->session->pendingNotification = false;
    }
    if (!_p->active || sample.sequence <= _p->deliveredSequence) return;
    _p->deliveredSequence = sample.sequence;
    Q_EMIT sampleReady(sample.cpu, sample.gpu, sample.memory);
}

QJsonObject SystemPerformanceMonitor::diagnostics() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    QJsonObject out{{"guiThreadId", QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()))},
        {"active", _p->active}, {"restartPending", _p->restartPending},
        {"deliveredSequence", static_cast<double>(_p->deliveredSequence)}};
    if (_p->session) {
        std::lock_guard<std::mutex> lock(_p->session->mailboxMutex);
        out["samplerThreadId"] = _p->session->samplerThreadId;
        out["sampleCount"] = static_cast<double>(_p->session->latest.sequence);
        out["maxSampleMs"] = _p->session->maxSampleMs;
        out["inFlight"] = _p->session->inFlight.load() != 0;
        out["workerFinished"] = _p->session->finished.load();
        out["pendingNotifications"] = _p->session->pendingNotification ? 1 : 0;
        out["supersededSamples"] = static_cast<double>(_p->session->superseded);
        out["maxConcurrentSamples"] = _p->session->maxConcurrentSamples.load();
    }
    return out;
}

void SystemPerformanceMonitor::shutdownWorkers() { registry().shutdown(); }
int SystemPerformanceMonitor::activeWorkerCount()
{
    auto& owner = registry();
    std::lock_guard<std::mutex> lock(owner.mutex);
    return static_cast<int>(std::count_if(owner.sessions.begin(), owner.sessions.end(),
        [](const auto& session) { return !session->finished.load(); }));
}

} // namespace cgplay
