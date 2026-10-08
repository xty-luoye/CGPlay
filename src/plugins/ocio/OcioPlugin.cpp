#include "OcioPlugin.h"

#include "core/session/api/ISessionService.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "plugins/api/HostExtensionPoints.h"
#include "ocio/OcioManager.h"

#include <QAction>
#include <QApplication>
#include <QMetaObject>
#include <QWidget>

namespace cgplay {

QString OcioPlugin::id() const
{
    return QStringLiteral("ocio");
}

void OcioPlugin::initialize()
{
    if (_initialized) {
        return;
    }
    auto* eventBus = ServiceLocator::getService<IEventBus>();
    auto ocioManager = ServiceLocator::getSharedService<OcioManager>();
    if (!eventBus || !ocioManager) {
        return;
    }

    _eventBus = eventBus;
    _ocioObject = ocioManager.get();
    _initialized = true;

    _mediaOpenedSubscriptions.push_back(
        eventBus->subscribe<OcioOptionsChangedEvent>([this](const OcioOptionsChangedEvent&) {
            if (auto* sessionService = ServiceLocator::getService<ISessionService>()) {
                sessionService->markDirty();
            }
        }));

}

void OcioPlugin::shutdown()
{
    if (!_initialized) {
        return;
    }
    _initialized = false;
    for (const auto& connection : _connections) {
        disconnect(connection);
    }
    _connections.clear();

    if (_eventBus) {
        for (const auto subscriptionId : _mediaOpenedSubscriptions) {
            _eventBus->unsubscribe<OcioOptionsChangedEvent>(subscriptionId);
        }
    }
    _mediaOpenedSubscriptions.clear();
    _disposeActions();
    _viewerObject.clear();
    _ocioObject.clear();
    _eventBus = nullptr;
}

QVector<CommandDescriptor> OcioPlugin::commandDescriptors() const
{
    return {
        CommandDescriptor{
            QStringLiteral("ocio.enable"),
            QStringLiteral("启用 OCIO"),
            QString(),
            QString(),
            QString(),
            QString(),
            true,
            0,
            [this](QObject* parent) { return _ensureEnableAction(parent); },
            {},
            {},
            {},
            {}
        },
        CommandDescriptor{
            QStringLiteral("ocio.alpha"),
            QStringLiteral("查看 Alpha 通道"),
            QString(),
            QString(),
            QString(),
            QString(),
            true,
            10,
            [this](QObject* parent) { return _ensureAlphaAction(parent); },
            {},
            {},
            {},
            {}
        },
        CommandDescriptor{
            QStringLiteral("ocio.settings"),
            QStringLiteral("OCIO 设置..."),
            QString(),
            QString(),
            QString(),
            QString(),
            false,
            20,
            [this](QObject* parent) { return _ensureSettingsAction(parent); },
            {},
            {},
            {},
            {}
        }
    };
}

QVector<MenuContribution> OcioPlugin::menuContributions() const
{
    return {
        MenuContribution{QStringLiteral("ocio.menu.enable"), HostExtensionSlots::kMenuColor, QStringLiteral("颜色"), QStringLiteral("ocio.enable"), 0, false, false},
        MenuContribution{QStringLiteral("ocio.menu.alpha"), HostExtensionSlots::kMenuColor, QStringLiteral("颜色"), QStringLiteral("ocio.alpha"), 10, false, false},
        MenuContribution{QStringLiteral("ocio.menu.settings"), HostExtensionSlots::kMenuColor, QStringLiteral("颜色"), QStringLiteral("ocio.settings"), 20, false, false}
    };
}

QVector<ToolbarContribution> OcioPlugin::toolbarContributions() const
{
    return {};
}

QVector<PanelContribution> OcioPlugin::panelContributions() const
{
    return {};
}

QAction* OcioPlugin::_ensureEnableAction(QObject* parent) const
{
    if (_enableAction) {
        if (parent && !_enableAction->parent()) {
            _enableAction->setParent(parent);
        }
        return _enableAction;
    }

    auto* action = new QAction(QStringLiteral("启用 OCIO"), parent);
    action->setCheckable(true);
    action->setChecked(_queryOcioEnabled());

    if (_ocioObject) {
        connect(action, &QAction::toggled, this, [this](bool checked) {
            if (_ocioObject) {
                QMetaObject::invokeMethod(_ocioObject, "setEnabled", Q_ARG(bool, checked));
            }
        });
        connect(_ocioObject, SIGNAL(enabledChanged(bool)), action, SLOT(setChecked(bool)));
    }

    _enableAction = action;
    return action;
}

QAction* OcioPlugin::_ensureAlphaAction(QObject* parent) const
{
    if (_alphaAction) {
        if (parent && !_alphaAction->parent()) {
            _alphaAction->setParent(parent);
        }
        return _alphaAction;
    }

    auto* action = new QAction(QStringLiteral("查看 Alpha 通道"), parent);
    action->setCheckable(true);
    action->setChecked(false);

    connect(action, &QAction::toggled, this, [this](bool checked) {
        // Resolve the current viewer at invocation time.  The main viewer can
        // be rebuilt while this QAction remains owned by the menu.
        if (auto* viewer = _resolveViewerObject()) {
            QMetaObject::invokeMethod(viewer, "setAlphaChannelVisible", Q_ARG(bool, checked));
        }
        _bindAlphaActionToViewer();
    });

    _alphaAction = action;
    _bindAlphaActionToViewer();
    return action;
}

void OcioPlugin::_bindAlphaActionToViewer() const
{
    if (!_alphaAction) {
        return;
    }

    if (_alphaViewerConnection) {
        disconnect(_alphaViewerConnection);
        _alphaViewerConnection = {};
    }

    if (auto* viewer = _resolveViewerObject()) {
        // The connection is owned by the action, so it is disconnected when
        // either the rebuilt viewer or the menu action is destroyed.
        _alphaViewerConnection = connect(
            viewer,
            SIGNAL(alphaChannelChanged(bool)),
            _alphaAction,
            SLOT(setChecked(bool)));
    }
}

QAction* OcioPlugin::_ensureSettingsAction(QObject* parent) const
{
    if (_settingsAction) {
        if (parent && !_settingsAction->parent()) {
            _settingsAction->setParent(parent);
        }
        return _settingsAction;
    }

    auto* action = new QAction(QStringLiteral("OCIO 设置..."), parent);
    connect(action, &QAction::triggered, this, [this, action] {
        if (!_ocioObject) {
            return;
        }
        QWidget* owner = qobject_cast<QWidget*>(action->parent());
        QMetaObject::invokeMethod(_ocioObject, "showSettings", Q_ARG(QWidget*, owner));
    });

    _settingsAction = action;
    return action;
}

void OcioPlugin::_disposeActions()
{
    if (_alphaViewerConnection) {
        disconnect(_alphaViewerConnection);
        _alphaViewerConnection = {};
    }
    if (_enableAction) {
        _enableAction->deleteLater();
        _enableAction.clear();
    }
    if (_alphaAction) {
        _alphaAction->deleteLater();
        _alphaAction.clear();
    }
    if (_settingsAction) {
        _settingsAction->deleteLater();
        _settingsAction.clear();
    }
}

QObject* OcioPlugin::_resolveViewerObject() const
{
    if (_viewerObject) {
        return _viewerObject;
    }

    for (QWidget* widget : qApp->allWidgets()) {
        if (widget->metaObject()->indexOfSignal("alphaChannelChanged(bool)") >= 0 &&
            widget->metaObject()->indexOfMethod("setAlphaChannelVisible(bool)") >= 0) {
            _viewerObject = widget;
            break;
        }
    }
    return _viewerObject;
}

bool OcioPlugin::_queryOcioEnabled() const
{
    if (!_ocioObject) {
        return false;
    }

    bool enabled = false;
    if (QMetaObject::invokeMethod(
            _ocioObject,
            "isEnabled",
            Qt::DirectConnection,
            Q_RETURN_ARG(bool, enabled))) {
        return enabled;
    }

    return _ocioObject->property("enabled").toBool();
}

} // namespace cgplay
