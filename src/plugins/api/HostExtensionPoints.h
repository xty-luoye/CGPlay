#pragma once

#include <QAction>
#include <QSize>
#include <QString>
#include <QWidget>
#include <QVector>

#include <functional>

namespace cgplay {

namespace HostExtensionSlots {

inline const QString kMenuFile = QStringLiteral("menu.file");
inline const QString kMenuView = QStringLiteral("menu.view");
inline const QString kMenuWindow = QStringLiteral("menu.window");
inline const QString kMenuOtio = QStringLiteral("menu.otio");
inline const QString kMenuColor = QStringLiteral("menu.color");
inline const QString kMenuAudio = QStringLiteral("menu.audio");
inline const QString kMenuPlayback = QStringLiteral("menu.playback");
inline const QString kMenuHelp = QStringLiteral("menu.help");

inline const QString kToolbarViewerPrimary = QStringLiteral("toolbar.viewer.primary");
inline const QString kToolbarViewerSecondary = QStringLiteral("toolbar.viewer.secondary");
inline const QString kToolbarReviewPrimary = QStringLiteral("toolbar.review.primary");
inline const QString kToolbarTopPrimary = QStringLiteral("toolbar.top.primary");

inline const QString kPanelLeft = QStringLiteral("panel.left");
inline const QString kPanelRight = QStringLiteral("panel.right");
inline const QString kPanelBottom = QStringLiteral("panel.bottom");
inline const QString kPanelCenterAux = QStringLiteral("panel.center.aux");

} // namespace HostExtensionSlots

struct CommandDescriptor
{
    QString id;
    QString text;
    QString toolTip;
    QString statusTip;
    QString iconName;
    QString shortcut;
    bool checkable = false;
    int order = 0;
    std::function<QAction*(QObject* parent)> createAction;
    std::function<void(bool checked)> trigger;
    std::function<bool()> isEnabled;
    std::function<bool()> isVisible;
    std::function<bool()> isChecked;
};

struct MenuContribution
{
    QString id;
    QString menuSlot;
    QString menuTitle;
    QString commandId;
    int order = 0;
    bool separatorBefore = false;
    bool separatorAfter = false;
};

struct ToolbarContribution
{
    QString id;
    QString toolbarSlot;
    QString commandId;
    QString labelOverride;
    QString toolTipOverride;
    int order = 0;
    bool separatorBefore = false;
    std::function<QWidget*(QObject* parent)> createWidget;
};

struct PanelContribution
{
    QString id;
    QString panelSlot;
    QString title;
    QString contentToken;
    QSize preferredSize;
    bool defaultVisible = true;
    int order = 0;
    std::function<QWidget*(QObject* parent)> createWidget;
};

struct ProviderCountSummary
{
    QString plugin;
    int count = 0;
};

} // namespace cgplay
