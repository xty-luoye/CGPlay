#pragma once
// CGPlay ThreadPool.h — v1.1 性能层
// 可配置线程池，用于异步图像读取、预读缓存等任务。
// 封装 QThreadPool，支持动态调整线程数、优先级和任务队列统计。

#include <QObject>
#include <QThreadPool>
#include <cstddef>
#include <atomic>

namespace cgplay {

// ─── ThreadPool ────────────────────────────────────────────────────────────────
// 全局线程池管理器，提供统一的异步执行能力。
// ────────────────────────────────────────────────────────────────────────────────
class ThreadPool : public QObject
{
    Q_OBJECT
public:
    explicit ThreadPool(QObject* parent = nullptr);
    ~ThreadPool() override;

    // ── 获取单例 ─────────────────────────────────────────────────────────
    static ThreadPool* instance();

    // ── 线程数 ────────────────────────────────────────────────────────────
    int   threadCount()        const;
    void  setThreadCount(int n);      // 设置工作线程数（默认 = CPU核心数）

    // ── 任务提交 ──────────────────────────────────────────────────────────
    // submit: 提交一个可调用对象，返回 true 表示成功入队
    template<typename Func>
    bool submit(Func&& f) {
        ++_queued;
        _pool->start([this, f = std::forward<Func>(f)]() mutable {
            _active.fetch_add(1);
            f();
            _active.fetch_sub(1);
            _completed.fetch_add(1);
        });
        return true;
    }

    // ── 统计 ──────────────────────────────────────────────────────────────
    int activeTaskCount()    const;    // 当前正在执行的任务数
    int queuedTaskCount()    const;    // 提交后尚未完成的任务数
    long long completedCount() const;  // 累计完成数

    // ── 控制 ──────────────────────────────────────────────────────────────
    void waitForDone(int msecs = -1);
    void clear();                      // 清空等待队列（正在执行的不受影响）

Q_SIGNALS:
    void threadCountChanged(int n);
    void allTasksDone();               // 所有排队任务完成时发射

private:
    QThreadPool* _pool = nullptr;
    std::atomic<int>   _active{0};
    std::atomic<long long> _queued{0};
    std::atomic<long long> _completed{0};
};

} // namespace cgplay
