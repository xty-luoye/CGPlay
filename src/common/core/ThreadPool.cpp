// CGPlay ThreadPool.cpp — v1.1 性能层

#include "ThreadPool.h"
#include <QThread>
#include <QDebug>

namespace cgplay {

// ─── 单例 ──────────────────────────────────────────────────────────────────────
static ThreadPool* g_instance = nullptr;

ThreadPool* ThreadPool::instance()
{
    if (!g_instance)
        g_instance = new ThreadPool();
    return g_instance;
}

// ─── 构造 / 析构 ───────────────────────────────────────────────────────────────
ThreadPool::ThreadPool(QObject* parent)
    : QObject(parent)
{
    _pool = new QThreadPool(this);
    // 默认线程数 = 最优线程数
    _pool->setMaxThreadCount(QThread::idealThreadCount());
    _pool->setExpiryTimeout(5000); // 5秒空闲后回收
    qDebug() << "[ThreadPool] Initialized with"
             << _pool->maxThreadCount() << "threads";
}

ThreadPool::~ThreadPool()
{
    _pool->clear();
    _pool->waitForDone(3000);
    if (g_instance == this)
        g_instance = nullptr;
}

// ─── 线程数 ────────────────────────────────────────────────────────────────────
int ThreadPool::threadCount() const
{
    return _pool->maxThreadCount();
}

void ThreadPool::setThreadCount(int n)
{
    if (n < 1) n = 1;
    if (n > QThread::idealThreadCount() * 4)
        n = QThread::idealThreadCount() * 4;
    _pool->setMaxThreadCount(n);
    Q_EMIT threadCountChanged(n);
    qDebug() << "[ThreadPool] Thread count set to" << n;
}

// ─── 统计 ──────────────────────────────────────────────────────────────────────
int ThreadPool::activeTaskCount() const
{
    return _active.load();
}

int ThreadPool::queuedTaskCount() const
{
    long long q = _queued.load();
    long long c = _completed.load();
    return static_cast<int>(q - c);
}

long long ThreadPool::completedCount() const
{
    return _completed.load();
}

// ─── 控制 ──────────────────────────────────────────────────────────────────────
void ThreadPool::waitForDone(int msecs)
{
    _pool->waitForDone(msecs < 0 ? ULONG_MAX : msecs);
    Q_EMIT allTasksDone();
}

void ThreadPool::clear()
{
    _pool->clear();
}

} // namespace cgplay
