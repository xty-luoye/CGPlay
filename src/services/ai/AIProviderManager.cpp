#include "AIProviderManager.h"

#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QFutureWatcher>
#include <QMutexLocker>
#include <QUuid>

#include <QtConcurrent>

#include <algorithm>

namespace cgplay {

namespace {

QVector<AIProviderInfo> sortProviderInfos(QVector<AIProviderInfo> infos)
{
    std::sort(infos.begin(), infos.end(), [](const AIProviderInfo& lhs, const AIProviderInfo& rhs) {
        return lhs.providerId < rhs.providerId;
    });
    return infos;
}

} // namespace

AIProviderManager::AIProviderManager(IEventBus* eventBus, QObject* parent)
    : QObject(parent)
    , _eventBus(eventBus)
{
}

QString AIProviderManager::defaultProviderId() const
{
    QMutexLocker lock(&_mutex);
    return _defaultProviderId;
}

void AIProviderManager::setDefaultProviderId(const QString& providerId)
{
    QMutexLocker lock(&_mutex);
    const QString normalized = providerId.trimmed();
    if (normalized.isEmpty()) {
        _defaultProviderId.clear();
        return;
    }
    if (_providers.contains(normalized)) {
        _defaultProviderId = normalized;
    }
}

QVector<AIProviderInfo> AIProviderManager::providers() const
{
    QVector<AIProviderInfo> infos;
    QMutexLocker lock(&_mutex);
    infos.reserve(_providers.size());
    for (auto it = _providers.cbegin(); it != _providers.cend(); ++it) {
        if (!it.value()) {
            continue;
        }
        infos.push_back(AIProviderInfo{
            it.value()->providerId(),
            it.value()->providerName(),
            it.value()->isAvailable(),
            it.value()->capabilities()
        });
    }
    return sortProviderInfos(infos);
}

bool AIProviderManager::hasProvider(const QString& providerId) const
{
    QMutexLocker lock(&_mutex);
    return _providers.contains(providerId.trimmed());
}

bool AIProviderManager::registerProvider(std::shared_ptr<IAIProvider> provider)
{
    if (!provider || provider->providerId().trimmed().isEmpty()) {
        return false;
    }

    const QString providerId = provider->providerId().trimmed();
    const bool available = provider->isAvailable();
    {
        QMutexLocker lock(&_mutex);
        _providers.insert(providerId, std::move(provider));
        if (_defaultProviderId.isEmpty()) {
            _defaultProviderId = providerId;
        }
    }

    if (_eventBus) {
        _eventBus->publish(AIProviderAvailabilityChangedEvent{ providerId, available });
    }
    return true;
}

bool AIProviderManager::unregisterProvider(const QString& providerId)
{
    const QString normalized = providerId.trimmed();
    if (normalized.isEmpty()) {
        return false;
    }

    bool removed = false;
    {
        QMutexLocker lock(&_mutex);
        const auto removedCount = _providers.remove(normalized);
        removed = removedCount != 0;
        if (!removed) {
            return false;
        }
        if (_defaultProviderId == normalized) {
            _defaultProviderId.clear();
            if (!_providers.isEmpty()) {
                _defaultProviderId = _providers.cbegin().key();
            }
        }
    }

    if (_eventBus) {
        _eventBus->publish(AIProviderAvailabilityChangedEvent{ normalized, false });
    }
    return removed;
}

QString AIProviderManager::submit(const AIRequest& request)
{
    AIRequest normalizedRequest = request;
    const QString jobId = _makeJobId();
    normalizedRequest.jobId = jobId;

    std::shared_ptr<IAIProvider> provider;
    QString providerId;
    auto cancelRequested = std::make_shared<std::atomic_bool>(false);

    {
        QMutexLocker lock(&_mutex);
        providerId = normalizedRequest.providerId.trimmed();
        if (providerId.isEmpty()) {
            providerId = _defaultProviderId;
            if (providerId.isEmpty() && !_providers.isEmpty()) {
                providerId = _providers.cbegin().key();
            }
        }
        normalizedRequest.providerId = providerId;

        JobRecord record;
        record.cancelRequested = cancelRequested;
        record.snapshot.jobId = jobId;
        record.snapshot.providerId = providerId;
        record.snapshot.state = AIJobState::Pending;
        record.snapshot.submittedAt = QDateTime::currentDateTimeUtc();
        record.snapshot.request = normalizedRequest;
        _jobs.insert(jobId, record);

        provider = _providers.value(providerId);
    }

    if (_eventBus) {
        _eventBus->publish(AIAnalysisRequestedEvent{ jobId, providerId, normalizedRequest });
    }

    if (!provider || !provider->isAvailable()) {
        AIJobSnapshot failedSnapshot;
        {
            QMutexLocker lock(&_mutex);
            auto it = _jobs.find(jobId);
            if (it != _jobs.end()) {
                it->snapshot.state = AIJobState::Failed;
                it->snapshot.errorMessage = QStringLiteral("AI provider is unavailable");
                it->snapshot.finishedAt = QDateTime::currentDateTimeUtc();
                it->snapshot.response.jobId = jobId;
                it->snapshot.response.providerId = providerId;
                it->snapshot.response.success = false;
                it->snapshot.response.errorMessage = it->snapshot.errorMessage;
                it->snapshot.response.finishedAt = it->snapshot.finishedAt;
                failedSnapshot = it->snapshot;
            }
        }
        if (_eventBus) {
            _eventBus->publish(AIAnalysisFailedEvent{
                jobId,
                providerId,
                failedSnapshot.errorMessage,
                normalizedRequest
            });
        }
        return jobId;
    }

    const QDateTime startedAt = QDateTime::currentDateTimeUtc();
    {
        QMutexLocker lock(&_mutex);
        auto it = _jobs.find(jobId);
        if (it != _jobs.end()) {
            it->snapshot.state = AIJobState::Running;
            it->snapshot.startedAt = startedAt;
        }
    }

    auto* watcher = new QFutureWatcher<AIResponse>(this);
    connect(watcher, &QFutureWatcher<AIResponse>::finished, this, [this, watcher, jobId, cancelRequested]() {
        const AIResponse taskResponse = watcher->result();
        watcher->deleteLater();

        AIJobSnapshot snapshot;
        {
            QMutexLocker lock(&_mutex);
            auto it = _jobs.find(jobId);
            if (it == _jobs.end()) {
                return;
            }

            AIResponse response = taskResponse;
            if (response.jobId.isEmpty()) {
                response.jobId = jobId;
            }
            if (response.providerId.isEmpty()) {
                response.providerId = it->snapshot.providerId;
            }
            if (!response.startedAt.isValid()) {
                response.startedAt = it->snapshot.startedAt;
            }
            if (!response.finishedAt.isValid()) {
                response.finishedAt = QDateTime::currentDateTimeUtc();
            }
            if (response.durationMs <= 0 && response.startedAt.isValid()) {
                response.durationMs = response.startedAt.msecsTo(response.finishedAt);
            }

            it->snapshot.response = response;
            it->snapshot.finishedAt = response.finishedAt;

            if (cancelRequested && cancelRequested->load()) {
                it->snapshot.state = AIJobState::Canceled;
                it->snapshot.errorMessage = QStringLiteral("Canceled");
                it->snapshot.response.success = false;
                it->snapshot.response.errorMessage = it->snapshot.errorMessage;
            } else {
                it->snapshot.errorMessage = response.errorMessage;
                it->snapshot.state = response.success ? AIJobState::Succeeded : AIJobState::Failed;
            }

            snapshot = it->snapshot;
        }

        if (!_eventBus) {
            return;
        }

        if (snapshot.state == AIJobState::Succeeded) {
            _eventBus->publish(AIAnalysisCompletedEvent{
                snapshot.jobId,
                snapshot.providerId,
                snapshot.response
            });
        } else {
            _eventBus->publish(AIAnalysisFailedEvent{
                snapshot.jobId,
                snapshot.providerId,
                snapshot.errorMessage,
                snapshot.request
            });
        }
    });

    const AIRequest requestForTask = normalizedRequest;
    watcher->setFuture(QtConcurrent::run([provider, requestForTask, startedAt]() {
        AIResponse response = provider->chat(requestForTask);
        if (!response.startedAt.isValid()) {
            response.startedAt = startedAt;
        }
        if (!response.finishedAt.isValid()) {
            response.finishedAt = QDateTime::currentDateTimeUtc();
        }
        if (response.durationMs <= 0 && response.startedAt.isValid()) {
            response.durationMs = response.startedAt.msecsTo(response.finishedAt);
        }
        return response;
    }));

    return jobId;
}

AIResponse AIProviderManager::chatSync(const AIRequest& request)
{
    AIResponse result;
    result.jobId = request.jobId.trimmed();
    result.providerId = request.providerId.trimmed();
    result.model = request.model.trimmed();
    result.startedAt = QDateTime::currentDateTimeUtc();

    std::shared_ptr<IAIProvider> provider;
    QString providerId = result.providerId;
    {
        QMutexLocker lock(&_mutex);
        if (providerId.isEmpty()) {
            providerId = _defaultProviderId;
            if (providerId.isEmpty() && !_providers.isEmpty()) {
                providerId = _providers.cbegin().key();
            }
        }
        provider = _providers.value(providerId);
    }

    result.providerId = providerId;
    if (!provider || !provider->isAvailable()) {
        result.success = false;
        result.errorMessage = QStringLiteral("AI provider is unavailable");
        result.finishedAt = QDateTime::currentDateTimeUtc();
        result.durationMs = result.startedAt.msecsTo(result.finishedAt);
        return result;
    }

    result = provider->chat(request);
    if (result.providerId.isEmpty()) {
        result.providerId = providerId;
    }
    if (!request.model.trimmed().isEmpty() && result.model.isEmpty()) {
        result.model = request.model.trimmed();
    }
    if (!request.jobId.trimmed().isEmpty() && result.jobId.isEmpty()) {
        result.jobId = request.jobId.trimmed();
    }
    if (!result.startedAt.isValid()) {
        result.startedAt = QDateTime::currentDateTimeUtc();
    }
    if (!result.finishedAt.isValid()) {
        result.finishedAt = QDateTime::currentDateTimeUtc();
    }
    if (result.durationMs <= 0) {
        result.durationMs = result.startedAt.msecsTo(result.finishedAt);
    }
    return result;
}

AIAudioTranscriptionResult AIProviderManager::transcribe(const AIAudioTranscriptionRequest& request)
{
    AIAudioTranscriptionResult result;
    result.providerId = request.providerId.trimmed();
    result.model = request.model.trimmed();
    result.startedAt = QDateTime::currentDateTimeUtc();

    std::shared_ptr<IAIProvider> provider;
    QString providerId = result.providerId;
    {
        QMutexLocker lock(&_mutex);
        if (providerId.isEmpty()) {
            providerId = _defaultProviderId;
            if (providerId.isEmpty() && !_providers.isEmpty()) {
                providerId = _providers.cbegin().key();
            }
        }
        provider = _providers.value(providerId);
    }

    result.providerId = providerId;
    if (!provider || !provider->isAvailable()) {
        result.success = false;
        result.errorMessage = QStringLiteral("AI provider is unavailable");
        result.finishedAt = QDateTime::currentDateTimeUtc();
        result.durationMs = result.startedAt.msecsTo(result.finishedAt);
        return result;
    }

    result = provider->transcribe(request);
    if (result.providerId.isEmpty()) {
        result.providerId = providerId;
    }
    if (!result.startedAt.isValid()) {
        result.startedAt = QDateTime::currentDateTimeUtc();
    }
    if (!result.finishedAt.isValid()) {
        result.finishedAt = QDateTime::currentDateTimeUtc();
    }
    if (result.durationMs <= 0) {
        result.durationMs = result.startedAt.msecsTo(result.finishedAt);
    }
    return result;
}

bool AIProviderManager::cancel(const QString& jobId)
{
    QMutexLocker lock(&_mutex);
    auto it = _jobs.find(jobId);
    if (it == _jobs.end()) {
        return false;
    }
    if (it->snapshot.state == AIJobState::Succeeded ||
        it->snapshot.state == AIJobState::Failed ||
        it->snapshot.state == AIJobState::Canceled) {
        return false;
    }

    if (it->cancelRequested) {
        it->cancelRequested->store(true);
    }
    it->snapshot.state = AIJobState::Canceled;
    it->snapshot.errorMessage = QStringLiteral("Canceled");
    it->snapshot.finishedAt = QDateTime::currentDateTimeUtc();
    return true;
}

AIJobSnapshot AIProviderManager::jobSnapshot(const QString& jobId) const
{
    QMutexLocker lock(&_mutex);
    const auto it = _jobs.constFind(jobId);
    if (it == _jobs.cend()) {
        AIJobSnapshot snapshot;
        snapshot.jobId = jobId;
        snapshot.state = AIJobState::Unknown;
        return snapshot;
    }
    return it->snapshot;
}

QString AIProviderManager::_makeJobId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace cgplay
