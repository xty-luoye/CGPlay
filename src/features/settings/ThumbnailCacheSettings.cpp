#include "ThumbnailCacheSettings.h"
#include "settings/api/ISettingsService.h"
#include <QApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <exception>

namespace cgplay {
namespace {
QString sizeText(quint64 bytes)
{ return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1); }
struct Result { ThumbnailDiskCache::Policy policy; ThumbnailDiskCache::Statistics stats; };
}
ThumbnailCacheSettings::ThumbnailCacheSettings(std::shared_ptr<ISettingsService> settings, QWidget* parent)
    : QWidget(parent), _settings(std::move(settings))
{
    setObjectName(QStringLiteral("ThumbnailDiskCacheSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    _directory = new QLineEdit(this);
    _directory->setObjectName(QStringLiteral("thumbnailCacheDirectory"));
    _directory->setReadOnly(true);
    _directory->setText(ThumbnailDiskCache::policy().baseDirectory);
    _directory->setCursorPosition(0);
    form->addRow(QStringLiteral("保存位置"), _directory);
    _quota = new QSpinBox(this);
    _quota->setObjectName(QStringLiteral("thumbnailCacheQuotaGB"));
    _quota->setRange(1, 64);
    _quota->setSuffix(QStringLiteral(" GB"));
    _quota->setValue(static_cast<int>(std::max<quint64>(1, ThumbnailDiskCache::policy().quotaBytes / (1024ULL * 1024 * 1024))));
    form->addRow(QStringLiteral("容量上限"), _quota);
    layout->addLayout(form);
    auto* hint = new QLabel(QStringLiteral("只管理 CGPlay 生成的缩略图，不清理源素材、转码文件或内存播放缓存。更改位置只影响新任务；旧位置不会自动搬移或删除，可切回后清理。活动任务使用的缩略图会保留，暂时允许超出上限。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QHBoxLayout;
    _choose = new QPushButton(QStringLiteral("更改位置…"), this);
    _choose->setObjectName(QStringLiteral("thumbnailCacheChooseDirectory"));
    _refresh = new QPushButton(QStringLiteral("刷新占用"), this);
    _refresh->setObjectName(QStringLiteral("thumbnailCacheRefresh"));
    _clear = new QPushButton(QStringLiteral("清理磁盘缩略图"), this);
    _clear->setObjectName(QStringLiteral("thumbnailCacheClear"));
    buttons->addWidget(_choose); buttons->addWidget(_refresh); buttons->addWidget(_clear); buttons->addStretch();
    layout->addLayout(buttons);
    _usage = new QLabel(QStringLiteral("正在读取占用…"), this);
    _usage->setObjectName(QStringLiteral("thumbnailCacheUsage"));
    _usage->setWordWrap(true);
    layout->addWidget(_usage);
    connect(_choose, &QPushButton::clicked, this, [this] { chooseDirectory(); });
    connect(_refresh, &QPushButton::clicked, this, [this] { run(ThumbnailDiskCache::policy(), false, false); });
    connect(_clear, &QPushButton::clicked, this, [this] { run(ThumbnailDiskCache::policy(), false, true); });
    connect(_quota, &QSpinBox::editingFinished, this, [this] {
        auto policy = ThumbnailDiskCache::policy();
        const quint64 bytes = static_cast<quint64>(_quota->value()) * 1024 * 1024 * 1024;
        if (bytes != policy.quotaBytes) { policy.quotaBytes = bytes; run(policy, true, false); }
    });
    run(ThumbnailDiskCache::policy(), false, false);
}
ThumbnailCacheSettings::~ThumbnailCacheSettings()
{ if (_cancel) _cancel->store(true); }

void ThumbnailCacheSettings::chooseDirectory()
{
    QString selected;
    if (qApp->property("cgplay.automationBackground").toBool())
        selected = property("cgplay.thumbnailCacheSelectedDirectory").toString();
    else selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择缩略图缓存位置"), _directory->text());
    if (selected.isEmpty()) return;
    auto policy = ThumbnailDiskCache::policy();
    policy.baseDirectory = selected;
    run(policy, true, false);
}

void ThumbnailCacheSettings::run(ThumbnailDiskCache::Policy policy, bool save, bool clear)
{
    if (_busy) return;
    _busy = true;
    setProperty("cgplay.thumbnailCacheBusy", true);
    _choose->setEnabled(false); _refresh->setEnabled(false); _clear->setEnabled(false); _quota->setEnabled(false);
    _usage->setText(clear ? QStringLiteral("正在后台清理…") : QStringLiteral("正在读取占用…"));
    _cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = _cancel;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, save] {
        const auto result = watcher->result();
        watcher->deleteLater();
        _busy = false;
        setProperty("cgplay.thumbnailCacheBusy", false);
        _choose->setEnabled(true); _refresh->setEnabled(true); _clear->setEnabled(true); _quota->setEnabled(true);
        if (!result.stats.error.isEmpty()) {
            _usage->setText(result.stats.error);
            const auto current = ThumbnailDiskCache::policy();
            _quota->setValue(static_cast<int>(std::max<quint64>(1, current.quotaBytes / (1024ULL * 1024 * 1024))));
            return;
        }
        if (result.stats.canceled) { _usage->setText(QStringLiteral("已取消")); return; }
        if (save) {
            ThumbnailDiskCache::configure(result.policy);
            if (_settings) {
                _settings->setValue(QStringLiteral("cache/thumbnails/baseDirectory"), result.policy.baseDirectory);
                _settings->setValue(QStringLiteral("cache/thumbnails/quotaBytes"), QVariant::fromValue(result.policy.quotaBytes));
                _settings->sync();
            }
        }
        _directory->setText(result.policy.baseDirectory);
        _directory->setCursorPosition(0);
        _directory->setToolTip(ThumbnailDiskCache::namespaceDirectory(result.policy));
        QString text = QStringLiteral("已占用 %1（%2 张）；本次清理 %3（%4 张）")
            .arg(sizeText(result.stats.bytes)).arg(result.stats.files)
            .arg(sizeText(result.stats.removedBytes)).arg(result.stats.removedFiles);
        if (result.stats.activeFiles) text += QStringLiteral("；%1 张正在使用，已保留").arg(result.stats.activeFiles);
        if (result.stats.bytes > result.policy.quotaBytes) text += QStringLiteral("；活动缓存暂时超过上限");
        _usage->setText(text);
    });
    watcher->setFuture(QtConcurrent::run([policy, clear, cancel]() mutable {
        Result result;
        result.policy = policy;
        try {
            if (ThumbnailDiskCache::prepare(result.policy, &result.stats.error))
                result.stats = ThumbnailDiskCache::maintain(result.policy, clear, [cancel] { return cancel->load(); });
        } catch (const std::exception& error) { result.stats.error = QString::fromUtf8(error.what()); }
        catch (...) { result.stats.error = QStringLiteral("无法完成缩略图缓存操作"); }
        return result;
    }));
}
} // namespace cgplay
