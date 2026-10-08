#include "AnnotationPlugin.h"

#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationOverlayProvider.h"
#include "annotation/AnnotationToolbar.h"
#include "annotation/ReviewPanel.h"
#include "common/core/OverlayRuntimeDebug.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "core/playback/api/IPlaybackService.h"
#include "core/session/api/ISessionService.h"
#include "plugins/api/HostExtensionPoints.h"
#include "viewer/api/IOverlayProvider.h"

#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QWidget>

namespace cgplay {

namespace {

constexpr int kDefaultTool = 0;
const QColor kDefaultColor(255, 0, 0);

void logAnnotationPluginEvent(const QString& event, const QJsonObject& details = {})
{
    QJsonObject payload = details;
    payload.insert(QStringLiteral("pluginName"), QStringLiteral("annotation"));
    runtimeAnnotationLog(QStringLiteral("AnnotationPlugin"), event, payload);
}

void logAnnotationAction(const QString& event, QJsonObject details = {})
{
    details.insert(QStringLiteral("pluginName"), QStringLiteral("annotation"));
    runtimeAnnotationLog(QStringLiteral("AnnotationPlugin"), event, std::move(details));
}

} // namespace

QString AnnotationPlugin::id() const
{
    return QStringLiteral("annotation");
}

void AnnotationPlugin::initialize()
{
    if (_initialized) {
        return;
    }
    _initialized = true;
    _eventBus = ServiceLocator::getService<IEventBus>();
    _annotationManager = ServiceLocator::getSharedService<AnnotationManager>().get();
    logAnnotationPluginEvent(
        QStringLiteral("initialize.begin"),
        QJsonObject{
            { QStringLiteral("hasEventBus"), _eventBus != nullptr },
            { QStringLiteral("hasAnnotationManager"), !_annotationManager.isNull() }
        });
    ServiceLocator::registerService<IAnnotationService>(this);
    ServiceLocator::registerService<ISessionContributor>(this);
    _overlayProvider = std::make_unique<AnnotationOverlayProvider>(_annotationManager.get());
    ServiceLocator::registerService<IOverlayProvider>(static_cast<IOverlayProvider*>(_overlayProvider.get()));
    ServiceLocator::registerService<IAnnotationViewBridge>(
        static_cast<IAnnotationViewBridge*>(_overlayProvider.get()));
    logAnnotationPluginEvent(
        QStringLiteral("capability.register"),
        QJsonObject{
            { QStringLiteral("annotationService"), true },
            { QStringLiteral("sessionContributor"), true },
            { QStringLiteral("overlayProvider"), _overlayProvider != nullptr },
            { QStringLiteral("annotationViewBridge"), _overlayProvider != nullptr }
        });
    if (!_eventBus) {
        return;
    }

    _subscriptions.push_back(
        _eventBus->subscribe<MediaOpenedEvent>([this](const MediaOpenedEvent& event) {
            Q_UNUSED(event);
            _bindOverlay();
            _syncToolbarState();
            _syncPanelState();
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<AnnotationChangedEvent>([this](const AnnotationChangedEvent& event) {
            Q_UNUSED(event);
            _bindOverlay();
            _syncToolbarState();
            _syncPanelState();
            if (auto* sessionService = ServiceLocator::getService<ISessionService>()) {
                sessionService->markDirty();
            }
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<ActiveViewChangedEvent>([this](const ActiveViewChangedEvent& event) {
            _bindOverlay();
            _syncToolbarState();
            _syncPanelState();
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<ActiveViewInvalidatedEvent>([this](const ActiveViewInvalidatedEvent& event) {
            if (event.viewId != _boundViewId) {
                return;
            }
            _unbindOverlay();
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<OverlayHostInvalidatedEvent>([this](const OverlayHostInvalidatedEvent& event) {
            if (event.viewId != _boundViewId) {
                return;
            }
            _unbindOverlay();
        }));
    _subscriptions.push_back(
        _eventBus->subscribe<CoordinateMapperInvalidatedEvent>([this](const CoordinateMapperInvalidatedEvent& event) {
            if (event.viewId != _boundViewId) {
                return;
            }
            _unbindOverlay();
        }));

    _bindOverlay();
    _syncToolbarState();
    _syncPanelState();
    logAnnotationPluginEvent(
        QStringLiteral("initialize.done"),
        QJsonObject{
            { QStringLiteral("boundViewId"), runtimeBoundViewId() },
            { QStringLiteral("subscriptionCount"), runtimeSubscriptionCount() }
        });
}

void AnnotationPlugin::shutdown()
{
    if (!_initialized) {
        return;
    }
    _initialized = false;
    logAnnotationPluginEvent(
        QStringLiteral("shutdown.begin"),
        QJsonObject{
            { QStringLiteral("boundViewId"), runtimeBoundViewId() },
            { QStringLiteral("subscriptionCount"), runtimeSubscriptionCount() }
        });
    if (_eventBus) {
        for (const auto subscriptionId : _subscriptions) {
            _eventBus->unsubscribe<MediaOpenedEvent>(subscriptionId);
            _eventBus->unsubscribe<FrameChangedEvent>(subscriptionId);
            _eventBus->unsubscribe<AnnotationChangedEvent>(subscriptionId);
            _eventBus->unsubscribe<ActiveViewChangedEvent>(subscriptionId);
            _eventBus->unsubscribe<ActiveViewInvalidatedEvent>(subscriptionId);
            _eventBus->unsubscribe<OverlayHostInvalidatedEvent>(subscriptionId);
            _eventBus->unsubscribe<CoordinateMapperInvalidatedEvent>(subscriptionId);
        }
    }
    _subscriptions.clear();
    _eventBus = nullptr;

    if (_toolbar) {
        _toolbar->deleteLater();
        _toolbar.clear();
    }
    if (_panel) {
        _panel->deleteLater();
        _panel.clear();
    }
    _unbindOverlay();
    if (ServiceLocator::getService<IAnnotationService>() == this) {
        ServiceLocator::registerService<IAnnotationService>(static_cast<IAnnotationService*>(nullptr));
    }
    if (ServiceLocator::getService<ISessionContributor>() == this) {
        ServiceLocator::registerService<ISessionContributor>(static_cast<ISessionContributor*>(nullptr));
    }
    if (ServiceLocator::getService<IOverlayProvider>() == _overlayProvider.get()) {
        ServiceLocator::registerService<IOverlayProvider>(static_cast<IOverlayProvider*>(nullptr));
    }
    if (ServiceLocator::getService<IAnnotationViewBridge>() ==
        static_cast<IAnnotationViewBridge*>(_overlayProvider.get())) {
        ServiceLocator::registerService<IAnnotationViewBridge>(static_cast<IAnnotationViewBridge*>(nullptr));
    }
    logAnnotationPluginEvent(
        QStringLiteral("capability.unregister"),
        QJsonObject{
            { QStringLiteral("annotationService"), true },
            { QStringLiteral("sessionContributor"), true },
            { QStringLiteral("overlayProvider"), _overlayProvider != nullptr },
            { QStringLiteral("annotationViewBridge"), _overlayProvider != nullptr }
        });
    _overlayProvider.reset();
    _activeView = nullptr;
    _overlayHost = nullptr;
    _coordinateMapper = nullptr;
    _viewBridge = nullptr;
    _boundViewId.clear();
    _annotationManager.clear();
}

QString AnnotationPlugin::runtimeBoundViewId() const
{
    return _overlayProvider ? _overlayProvider->boundViewId() : _boundViewId;
}

bool AnnotationPlugin::runtimeHasBridgeBinding() const
{
    return _overlayProvider ? _overlayProvider->hasBridgeBinding() : (_viewBridge != nullptr);
}

bool AnnotationPlugin::runtimeHasOverlayHostBinding() const
{
    return _overlayProvider ? _overlayProvider->hasOverlayHostBinding() : (_overlayHost != nullptr);
}

bool AnnotationPlugin::runtimeHasCoordinateMapperBinding() const
{
    return _overlayProvider ? _overlayProvider->hasCoordinateMapperBinding() : (_coordinateMapper != nullptr);
}

int AnnotationPlugin::runtimeSubscriptionCount() const
{
    return static_cast<int>(_subscriptions.size());
}

int AnnotationPlugin::count() const
{
    return _annotationManager ? _annotationManager->count() : 0;
}

QVector<AnnotationItem> AnnotationPlugin::annotations() const
{
    return _annotationManager ? _annotationManager->all() : QVector<AnnotationItem>{};
}

void AnnotationPlugin::replaceAnnotations(const QVector<AnnotationItem>& annotations)
{
    if (!_annotationManager) {
        return;
    }
    logAnnotationAction(
        QStringLiteral("annotation.replace"),
        QJsonObject{
            { QStringLiteral("count"), annotations.size() },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    _annotationManager->fromList(annotations);
}

void AnnotationPlugin::clearAnnotations()
{
    if (_annotationManager) {
        logAnnotationAction(
            QStringLiteral("annotation.clear"),
            QJsonObject{
                { QStringLiteral("count"), _annotationManager->count() },
                { QStringLiteral("viewId"), runtimeBoundViewId() }
            });
        _annotationManager->clear();
        _annotationManager->selectAnnotation(QString());
    }
    _selectedAnnotationId.clear();
}

bool AnnotationPlugin::undo()
{
    const bool result = _annotationManager ? _annotationManager->undo() : false;
    logAnnotationAction(
        QStringLiteral("annotation.undo"),
        QJsonObject{
            { QStringLiteral("result"), result },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return result;
}

bool AnnotationPlugin::redo()
{
    const bool result = _annotationManager ? _annotationManager->redo() : false;
    logAnnotationAction(
        QStringLiteral("annotation.redo"),
        QJsonObject{
            { QStringLiteral("result"), result },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return result;
}

QString AnnotationPlugin::addAnnotation(const AnnotationItem& annotation)
{
    if (!_annotationManager) {
        return {};
    }
    const QString id = _annotationManager->add(annotation);
    _selectedAnnotationId = id;
    _annotationManager->selectAnnotation(id);
    logAnnotationAction(
        QStringLiteral("annotation.add"),
        QJsonObject{
            { QStringLiteral("annotationId"), id },
            { QStringLiteral("frame"), annotation.frame },
            { QStringLiteral("type"), static_cast<int>(annotation.type) },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return id;
}

bool AnnotationPlugin::removeAnnotation(const QString& annotationId)
{
    if (!_annotationManager) {
        return false;
    }
    const bool removed = _annotationManager->remove(annotationId);
    if (removed && _selectedAnnotationId == annotationId) {
        _selectedAnnotationId.clear();
        _annotationManager->selectAnnotation(QString());
    }
    logAnnotationAction(
        QStringLiteral("annotation.remove"),
        QJsonObject{
            { QStringLiteral("annotationId"), annotationId },
            { QStringLiteral("result"), removed },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return removed;
}

bool AnnotationPlugin::removeSelectedAnnotation()
{
    return _selectedAnnotationId.isEmpty() ? false : removeAnnotation(_selectedAnnotationId);
}

bool AnnotationPlugin::updateAnnotationComment(const QString& annotationId, const QString& text)
{
    if (!_annotationManager || annotationId.isEmpty()) {
        return false;
    }
    const auto* ann = _annotationManager->get(annotationId);
    if (!ann) {
        return false;
    }

    AnnotationItem updated = *ann;
    updated.comment = text;
    if (!updated.comments.isEmpty()) {
        updated.comments.last().text = text;
    }
    const bool updatedOk = _annotationManager->update(annotationId, updated);
    logAnnotationAction(
        QStringLiteral("annotation.commentUpdated"),
        QJsonObject{
            { QStringLiteral("annotationId"), annotationId },
            { QStringLiteral("result"), updatedOk },
            { QStringLiteral("commentLength"), text.size() },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return updatedOk;
}

QString AnnotationPlugin::createNote(const QString& text)
{
    AnnotationItem ann;
    ann.frame = _viewBridge ? _viewBridge->currentFrame() : 0;
    ann.type = AnnotationType::Point;
    ann.color = kDefaultColor;
    ann.comment = text;
    ann.author = QString::fromUtf8("用户");
    ann.status = ReviewStatus::Open;
    if (_viewBridge) {
        ann.points.append(QPointF(_viewBridge->mediaWidth() * 0.5, _viewBridge->mediaHeight() * 0.5));
    }
    const QString id = addAnnotation(ann);
    logAnnotationAction(
        QStringLiteral("annotation.noteCreated"),
        QJsonObject{
            { QStringLiteral("annotationId"), id },
            { QStringLiteral("commentLength"), text.size() },
            { QStringLiteral("frame"), ann.frame },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
    return id;
}

void AnnotationPlugin::selectAnnotation(const QString& annotationId)
{
    _selectedAnnotationId = annotationId;
    if (_annotationManager) {
        _annotationManager->selectAnnotation(annotationId);
    }
    logAnnotationAction(
        QStringLiteral("annotation.select"),
        QJsonObject{
            { QStringLiteral("annotationId"), annotationId },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
}

QString AnnotationPlugin::selectedAnnotationId() const
{
    return _selectedAnnotationId;
}

void AnnotationPlugin::setTool(int tool)
{
    _currentTool = tool;
    if (_annotationManager) {
        _annotationManager->setTool(tool);
    }
    _syncToolbarState();
    logAnnotationAction(
        QStringLiteral("annotation.toolChanged"),
        QJsonObject{
            { QStringLiteral("tool"), tool },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
}

int AnnotationPlugin::currentTool() const
{
    return _currentTool;
}

void AnnotationPlugin::setToolColor(const QColor& color)
{
    _currentColor = color;
    if (_annotationManager) {
        _annotationManager->setToolColor(color);
    }
    _syncToolbarState();
    logAnnotationAction(
        QStringLiteral("annotation.colorChanged"),
        QJsonObject{
            { QStringLiteral("color"), color.name(QColor::HexArgb) },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
}

QColor AnnotationPlugin::currentToolColor() const
{
    return _currentColor;
}

QString AnnotationPlugin::sessionKey() const
{
    return QStringLiteral("annotations");
}

void AnnotationPlugin::serializeInto(QJsonObject& root) const
{
    if (!_annotationManager) {
        return;
    }

    QJsonArray annArr;
    for (const auto& a : _annotationManager->all()) {
        QJsonObject ao;
        ao["id"] = a.id;
        ao["frame"] = a.frame;
        ao["type"] = static_cast<int>(a.type);
        ao["color"] = a.color.name(QColor::HexArgb);
        ao["author"] = a.author;
        ao["created"] = a.createdTime;
        ao["comment"] = a.comment;
        ao["status"] = static_cast<int>(a.status);
        ao["assignee"] = a.assignee;

        QJsonArray pts;
        for (const auto& p : a.points) {
            pts.append(QJsonArray{ p.x(), p.y() });
        }
        ao["points"] = pts;

        QJsonArray comments;
        for (const auto& c : a.comments) {
            QJsonObject co;
            co["author"] = c.author;
            co["text"] = c.text;
            co["time"] = c.time;
            co["status"] = static_cast<int>(c.status);
            co["assignee"] = c.assignee;
            comments.append(co);
        }
        ao["comments"] = comments;
        annArr.append(ao);
    }
    root[sessionKey()] = annArr;
}

void AnnotationPlugin::deserializeFrom(const QJsonObject& root)
{
    if (!_annotationManager || !root.contains(sessionKey())) {
        return;
    }

    _annotationManager->clear();
    const QJsonArray annArr = root.value(sessionKey()).toArray();
    for (const auto& v : annArr) {
        const QJsonObject ao = v.toObject();
        AnnotationItem a;
        a.id = ao["id"].toString();
        a.frame = ao["frame"].toInt(0);
        a.type = static_cast<AnnotationType>(ao["type"].toInt(0));
        a.color = QColor(ao["color"].toString());
        a.author = ao["author"].toString("user");
        a.createdTime = ao["created"].toString();
        a.comment = ao["comment"].toString();
        a.status = static_cast<ReviewStatus>(ao["status"].toInt(0));
        a.assignee = ao["assignee"].toString("Other");

        const QJsonArray pts = ao["points"].toArray();
        for (const auto& p : pts) {
            const QJsonArray xy = p.toArray();
            a.points.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
        }

        const QJsonArray comments = ao["comments"].toArray();
        for (const auto& cv : comments) {
            const QJsonObject co = cv.toObject();
            Comment c;
            c.author = co["author"].toString();
            c.text = co["text"].toString();
            c.time = co["time"].toString();
            c.status = static_cast<ReviewStatus>(co["status"].toInt(0));
            c.assignee = co["assignee"].toString();
            a.comments.append(c);
        }
        _annotationManager->add(a);
    }
    logAnnotationAction(
        QStringLiteral("annotation.deserialize"),
        QJsonObject{
            { QStringLiteral("count"), annArr.size() },
            { QStringLiteral("viewId"), runtimeBoundViewId() }
        });
}

QVector<CommandDescriptor> AnnotationPlugin::commandDescriptors() const
{
    return {};
}

QVector<MenuContribution> AnnotationPlugin::menuContributions() const
{
    return {};
}

QVector<ToolbarContribution> AnnotationPlugin::toolbarContributions() const
{
    return {
        ToolbarContribution{
            QStringLiteral("annotation.toolbar"),
            HostExtensionSlots::kToolbarReviewPrimary,
            QString(),
            QStringLiteral("批注"),
            QStringLiteral("批注工具栏"),
            0,
            false,
            [this](QObject* parent) -> QWidget* {
                return _createToolbarWidget(parent);
            }
        }
    };
}

QVector<PanelContribution> AnnotationPlugin::panelContributions() const
{
    return {
        PanelContribution{
            QStringLiteral("annotation.review"),
            HostExtensionSlots::kPanelRight,
            QStringLiteral("审阅"),
            QStringLiteral("annotation.review"),
            QSize(320, 720),
            true,
            0,
            [this](QObject* parent) -> QWidget* {
                return _createPanelWidget(parent);
            }
        }
    };
}

QWidget* AnnotationPlugin::_createToolbarWidget(QObject* parent) const
{
    auto* self = const_cast<AnnotationPlugin*>(this);
    if (_toolbar) {
        if (parent && !_toolbar->parent()) {
            _toolbar->setParent(qobject_cast<QWidget*>(parent));
        }
        self->_syncToolbarState();
        return _toolbar;
    }

    auto* toolbar = new AnnotationToolbar(qobject_cast<QWidget*>(parent));
    toolbar->setFixedHeight(98);
    toolbar->setObjectName(QStringLiteral("AnnotationToolbar"));
    self->_connectToolbarSignals(toolbar);
    _toolbar = toolbar;
    if (_panel) {
        _panel->setToolsWidget(toolbar);
    }
    self->_syncToolbarState();
    return toolbar;
}

QWidget* AnnotationPlugin::_createPanelWidget(QObject* parent) const
{
    auto* self = const_cast<AnnotationPlugin*>(this);
    if (_panel) {
        if (parent && !_panel->parent()) {
            _panel->setParent(qobject_cast<QWidget*>(parent));
        }
        self->_syncPanelState();
        return _panel;
    }

    auto* panel = new ReviewPanel(_annotationManager ? _annotationManager.data() : nullptr, qobject_cast<QWidget*>(parent));
    panel->setObjectName(QStringLiteral("ReviewPanel"));
    self->_connectPanelSignals(panel);
    _panel = panel;
    if (_toolbar) {
        _panel->setToolsWidget(_toolbar);
    }
    self->_syncPanelState();
    return panel;
}

void AnnotationPlugin::_connectToolbarSignals(AnnotationToolbar* toolbar)
{
    if (!toolbar) {
        return;
    }

    QObject::connect(toolbar, &AnnotationToolbar::toolChanged, toolbar, [this](int tool, QColor color) {
        _currentColor = color;
        setTool(tool);
        setToolColor(color);
    });
    QObject::connect(toolbar, &AnnotationToolbar::colorChanged, toolbar, [this](QColor color) {
        setToolColor(color);
    });
    QObject::connect(toolbar, &AnnotationToolbar::deleteRequested, toolbar, [this] {
        removeSelectedAnnotation();
    });
}

void AnnotationPlugin::_connectPanelSignals(ReviewPanel* panel)
{
    if (!panel) {
        return;
    }

    QObject::connect(panel, &ReviewPanel::jumpToFrame, panel, [this](int frame) {
        if (auto* playback = ServiceLocator::getService<IPlaybackService>()) {
            playback->seekToFrame(frame);
        }
    });
    QObject::connect(panel, &ReviewPanel::annotationSelected, panel, [this](const QString& id) mutable {
        selectAnnotation(id);
    });
    QObject::connect(panel, &ReviewPanel::annotationDeleted, panel, [this](const QString& id) mutable {
        removeAnnotation(id);
    });
    QObject::connect(panel, &ReviewPanel::annotationCommentEdited, panel, [this](const QString& id, const QString& text) mutable {
        updateAnnotationComment(id, text);
    });
    QObject::connect(panel, &ReviewPanel::createNoteRequested, panel, [this](const QString& text) mutable {
        createNote(text);
    });
}

void AnnotationPlugin::_bindOverlay()
{
    _activeView = ServiceLocator::getService<IActivePlaybackView>();
    if (!_activeView || !_activeView->hasActiveView()) {
        logAnnotationPluginEvent(
            QStringLiteral("bind.skipped"),
            QJsonObject{
                { QStringLiteral("reason"), QStringLiteral("noActiveView") }
            });
        _unbindOverlay();
        return;
    }

    const QString activeViewId = _activeRuntimeViewId();
    if (activeViewId.isEmpty()) {
        logAnnotationPluginEvent(
            QStringLiteral("bind.skipped"),
            QJsonObject{
                { QStringLiteral("reason"), QStringLiteral("emptyActiveViewId") }
            });
        _unbindOverlay();
        return;
    }
    if (!_boundViewId.isEmpty() && _boundViewId == activeViewId && _viewBridge && _overlayHost && _coordinateMapper) {
        logAnnotationPluginEvent(
            QStringLiteral("bind.reuse"),
            QJsonObject{
                { QStringLiteral("viewId"), _boundViewId }
            });
        return;
    }

    _overlayHost = ServiceLocator::getService<IOverlayHost>();
    _coordinateMapper = ServiceLocator::getService<IViewerCoordinateMapper>();
    _viewBridge = ServiceLocator::getService<IAnnotationViewBridge>();
    if (!_activeView || !_overlayHost || !_coordinateMapper || !_viewBridge) {
        logAnnotationPluginEvent(
            QStringLiteral("bind.skipped"),
            QJsonObject{
                { QStringLiteral("viewId"), activeViewId },
                { QStringLiteral("hasOverlayHost"), _overlayHost != nullptr },
                { QStringLiteral("hasCoordinateMapper"), _coordinateMapper != nullptr },
                { QStringLiteral("hasViewBridge"), _viewBridge != nullptr }
            });
        _boundViewId.clear();
        return;
    }

    _boundViewId = activeViewId;
    _viewBridge->setInteractionHandlers(
        [this](const AnnotationItem& annotation) {
            addAnnotation(annotation);
        },
        [this](const QString& id) {
            selectAnnotation(id);
        },
        [this](const QString& id) {
            removeAnnotation(id);
        });
    logAnnotationPluginEvent(
        QStringLiteral("bind.attach"),
        QJsonObject{
            { QStringLiteral("viewId"), _boundViewId },
            { QStringLiteral("hasOverlayProvider"), _overlayProvider != nullptr }
        });
}

void AnnotationPlugin::_unbindOverlay()
{
    if (!_boundViewId.isEmpty() || _viewBridge || _overlayHost || _coordinateMapper) {
        logAnnotationPluginEvent(
            QStringLiteral("unbind.detach"),
            QJsonObject{
                { QStringLiteral("viewId"), _boundViewId },
                { QStringLiteral("hasViewBridge"), _viewBridge != nullptr },
                { QStringLiteral("hasOverlayHost"), _overlayHost != nullptr },
                { QStringLiteral("hasCoordinateMapper"), _coordinateMapper != nullptr }
            });
    }
    _viewBridge = nullptr;
    _overlayHost = nullptr;
    _coordinateMapper = nullptr;
    _boundViewId.clear();
}

void AnnotationPlugin::_syncToolbarState()
{
    if (!_toolbar) {
        return;
    }

    if (_toolbar->currentTool() != static_cast<AnnotationToolbar::Tool>(_currentTool)) {
        _toolbar->setTool(static_cast<AnnotationToolbar::Tool>(_currentTool));
    }
    if (_toolbar->currentColor() != _currentColor) {
        _toolbar->setColor(_currentColor);
    }
}

void AnnotationPlugin::_syncPanelState()
{
    if (!_panel) {
        return;
    }

    const int frame = _viewBridge ? _viewBridge->currentFrame() : 0;
    _panel->refresh(_annotationManager ? _annotationManager->all() : QVector<AnnotationItem>{}, frame);
    _panel->selectById(_selectedAnnotationId);
}

QString AnnotationPlugin::_activeRuntimeViewId() const
{
    return _activeView ? _activeView->activeViewId() : QString();
}

} // namespace cgplay
