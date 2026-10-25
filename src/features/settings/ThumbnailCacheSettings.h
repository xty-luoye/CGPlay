#pragma once
#include "media/ThumbnailDiskCache.h"
#include <QWidget>
#include <atomic>
#include <memory>
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
namespace cgplay {
class ISettingsService;
class ThumbnailCacheSettings final : public QWidget
{
public:
    explicit ThumbnailCacheSettings(std::shared_ptr<ISettingsService> settings, QWidget* parent = nullptr);
    ~ThumbnailCacheSettings() override;
private:
    void run(ThumbnailDiskCache::Policy policy, bool save, bool clear);
    void chooseDirectory();
    std::shared_ptr<ISettingsService> _settings;
    std::shared_ptr<std::atomic_bool> _cancel;
    QLineEdit* _directory = nullptr;
    QSpinBox* _quota = nullptr;
    QLabel* _usage = nullptr;
    QPushButton* _choose = nullptr;
    QPushButton* _refresh = nullptr;
    QPushButton* _clear = nullptr;
    bool _busy = false;
};
}
