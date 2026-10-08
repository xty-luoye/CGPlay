#pragma once

#include "annotation/api/IAnnotationService.h"
#include "annotation/api/IAnnotationViewBridge.h"
#include "common/events/api/IEventBus.h"
#include "core/session/api/ISessionContributor.h"
#include "plugins/api/ICommandProvider.h"
#include "plugins/api/IMenuProvider.h"
#include "plugins/api/IPanelProvider.h"
#include "plugins/api/IPlugin.h"
#include "plugins/api/IToolbarProvider.h"
#include "viewer/api/IActivePlaybackView.h"
#include "viewer/api/IOverlayHost.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QColor>
#include <QPointer>
#include <QObject>
#include <QWidget>

#include <memory>
#include <vector>

namespace cgplay {

class AnnotationToolbar;
class ReviewPanel;
class AnnotationManager;
class AnnotationOverlayProvider;

class AnnotationPlugin : public QObject,
                         public IPlugin,
                         public IAnnotationService,
                         public ISessionContributor,
                         public ICommandProvider,
                         public IMenuProvider,
                         public IToolbarProvider,
                         public IPanelProvider
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IPlugin cgplay::ICommandProvider cgplay::IMenuProvider cgplay::IToolbarProvider cgplay::IPanelProvider)
#ifdef CGPLAY_BUILD_QT_PLUGIN
    Q_PLUGIN_METADATA(IID CGPLAY_IPLUGIN_IID)
#endif
public:
    QString id() const override;
    void initialize() override;
    void shutdown() override;

    int count() const override;
    QVector<AnnotationItem> annotations() const override;
    void replaceAnnotations(const QVector<AnnotationItem>& annotations) override;
    void clearAnnotations() override;
    bool undo() override;
    bool redo() override;
    QString addAnnotation(const AnnotationItem& annotation) override;
    bool removeAnnotation(const QString& annotationId) override;
    bool removeSelectedAnnotation() override;
    bool updateAnnotationComment(const QString& annotationId, const QString& text) override;
    QString createNote(const QString& text) override;
    void selectAnnotation(const QString& annotationId) override;
    QString selectedAnnotationId() const override;
    void setTool(int tool) override;
    int currentTool() const override;
    void setToolColor(const QColor& color) override;
    QColor currentToolColor() const override;

    QString sessionKey() const override;
    void serializeInto(QJsonObject& root) const override;
    void deserializeFrom(const QJsonObject& root) override;

    QVector<CommandDescriptor> commandDescriptors() const override;
    QVector<MenuContribution> menuContributions() const override;
    QVector<ToolbarContribution> toolbarContributions() const override;
    QVector<PanelContribution> panelContributions() const override;

    Q_INVOKABLE QString runtimeBoundViewId() const;
    Q_INVOKABLE bool runtimeHasBridgeBinding() const;
    Q_INVOKABLE bool runtimeHasOverlayHostBinding() const;
    Q_INVOKABLE bool runtimeHasCoordinateMapperBinding() const;
    Q_INVOKABLE int runtimeSubscriptionCount() const;

private:
    QString _activeRuntimeViewId() const;
    QWidget* _createToolbarWidget(QObject* parent) const;
    QWidget* _createPanelWidget(QObject* parent) const;
    void _connectToolbarSignals(AnnotationToolbar* toolbar);
    void _connectPanelSignals(ReviewPanel* panel);
    void _bindOverlay();
    void _unbindOverlay();
    void _syncToolbarState();
    void _syncPanelState();

private:
    IEventBus* _eventBus = nullptr;
    std::vector<IEventBus::SubscriptionId> _subscriptions;
    mutable IAnnotationViewBridge* _viewBridge = nullptr;
    mutable IActivePlaybackView* _activeView = nullptr;
    mutable IOverlayHost* _overlayHost = nullptr;
    mutable IViewerCoordinateMapper* _coordinateMapper = nullptr;
    mutable QPointer<AnnotationToolbar> _toolbar;
    mutable QPointer<ReviewPanel> _panel;
    mutable QPointer<AnnotationManager> _annotationManager;
    std::unique_ptr<AnnotationOverlayProvider> _overlayProvider;
    mutable int _currentTool = 0;
    mutable QColor _currentColor = QColor(255, 0, 0);
    mutable QString _selectedAnnotationId;
    mutable QString _boundViewId;
    bool _initialized = false;
};

} // namespace cgplay
