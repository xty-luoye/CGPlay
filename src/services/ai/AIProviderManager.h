#pragma once

#include "ai/api/IAIProviderManager.h"

#include <QObject>

#include <QHash>
#include <QMutex>

#include <atomic>
#include <memory>

namespace cgplay {

class IEventBus;

class AIProviderManager : public QObject, public IAIProviderManager
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IAIProviderManager)
public:
    explicit AIProviderManager(IEventBus* eventBus = nullptr, QObject* parent = nullptr);

    QString defaultProviderId() const override;
    void setDefaultProviderId(const QString& providerId) override;

    QVector<AIProviderInfo> providers() const override;
    bool hasProvider(const QString& providerId) const override;
    bool registerProvider(std::shared_ptr<IAIProvider> provider) override;
    bool unregisterProvider(const QString& providerId) override;

    QString submit(const AIRequest& request) override;
    AIResponse chatSync(const AIRequest& request) override;
    AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest& request) override;
    bool cancel(const QString& jobId) override;
    AIJobSnapshot jobSnapshot(const QString& jobId) const override;

private:
    struct JobRecord
    {
        AIJobSnapshot snapshot;
        std::shared_ptr<std::atomic_bool> cancelRequested;
    };

    QString _makeJobId() const;

    mutable QMutex _mutex;
    QHash<QString, std::shared_ptr<IAIProvider>> _providers;
    QHash<QString, JobRecord> _jobs;
    QString _defaultProviderId;
    IEventBus* _eventBus = nullptr;
};

} // namespace cgplay
