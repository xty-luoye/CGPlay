#pragma once

#include <QFutureWatcher>
#include <QEvent>
#include <QPair>
#include <QPointer>
#include <QProgressDialog>
#include <QPromise>
#include <QVariant>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <QtMath>

namespace cgplay {

using VideoExportTaskResult = QPair<bool, QString>;
using VideoExportWork = std::function<VideoExportTaskResult(
    const std::atomic_bool*, const std::function<void(double)>&)>;

class VideoExportCloseCancel final : public QObject
{
public:
    VideoExportCloseCancel(std::shared_ptr<std::atomic_bool> canceled, QObject* owner)
        : QObject(owner), _canceled(std::move(canceled)) {}
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Close) _canceled->store(true, std::memory_order_relaxed);
        return false;
    }
private:
    std::shared_ptr<std::atomic_bool> _canceled;
};

// Only this GUI-thread facade owns widgets. The worker owns the cancel token
// and reports through QPromise; destroying the window cannot leave a widget
// pointer or stack reference in the worker. Qt's global pool joins its tasks.
inline bool isVideoExportTaskRunning(const QWidget* parent)
{
    return parent && parent->property("cgplay.videoExportBusy").toBool();
}

inline bool startVideoExportTask(QWidget* parent, const QString& label,
    VideoExportWork work,
    std::function<void(const VideoExportTaskResult&, bool)> finished)
{
    if (!parent || isVideoExportTaskRunning(parent)) return false;
    parent->setProperty("cgplay.videoExportBusy", true);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto* watcher = new QFutureWatcher<VideoExportTaskResult>(parent);
    parent->installEventFilter(new VideoExportCloseCancel(canceled, watcher));
    auto* dialog = new QProgressDialog(label, QObject::tr("取消"), 0, 100, parent);
    if (parent->testAttribute(Qt::WA_DontShowOnScreen))
        dialog->setAttribute(Qt::WA_DontShowOnScreen);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(0);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    QObject::connect(dialog, &QProgressDialog::canceled, watcher, [canceled] {
        canceled->store(true, std::memory_order_relaxed);
    });
    QObject::connect(watcher, &QObject::destroyed, [canceled] {
        canceled->store(true, std::memory_order_relaxed);
    });
    QObject::connect(watcher, &QFutureWatcher<VideoExportTaskResult>::progressValueChanged,
                     dialog, &QProgressDialog::setValue);
    const QPointer<QProgressDialog> guardedDialog(dialog);
    QObject::connect(watcher, &QFutureWatcher<VideoExportTaskResult>::finished, parent,
        [parent, watcher, guardedDialog, canceled, finished = std::move(finished)] {
            const VideoExportTaskResult result = watcher->result();
            const bool wasCanceled = canceled->load(std::memory_order_relaxed);
            parent->setProperty("cgplay.videoExportBusy", false);
            if (guardedDialog) {
                QObject::disconnect(guardedDialog, nullptr, watcher, nullptr);
                guardedDialog->close();
                guardedDialog->deleteLater();
            }
            watcher->deleteLater();
            if (finished) finished(result, wasCanceled);
        });
    dialog->show();
    watcher->setFuture(QtConcurrent::run(
        [work = std::move(work), canceled](QPromise<VideoExportTaskResult>& promise) {
            promise.setProgressRange(0, 100);
            try {
                if (canceled->load(std::memory_order_relaxed)) {
                    promise.addResult(qMakePair(false, QObject::tr("导出已取消。")));
                    return;
                }
                const auto progress = [&promise](double value) {
                    promise.setProgressValue(qBound(0, qRound(value * 100.0), 100));
                };
                promise.addResult(work(canceled.get(), progress));
            } catch (const std::exception& error) {
                promise.addResult(qMakePair(false, QString::fromUtf8(error.what())));
            } catch (...) {
                promise.addResult(qMakePair(false, QObject::tr("导出任务异常结束。")));
            }
        }));
    return true;
}

} // namespace cgplay
