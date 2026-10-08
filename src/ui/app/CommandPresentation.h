#pragma once

#include <QAbstractButton>
#include <QAction>
#include <QEvent>
#include <QKeySequence>
#include <QObject>
#include <QStringList>
#include <QVariant>

namespace cgplay {

// Presentation only: never installs a shortcut, changes focus, checks a button,
// enables an action, or dispatches a command.
inline QString commandPresentationText(const QString& title, const QString& shortcut,
                                       const QString& detail = {}, bool enabled = true,
                                       const QString& state = {})
{
    QStringList lines{title};
    if (!detail.isEmpty() && detail != title) lines.push_back(detail);
    const QString key = QKeySequence(shortcut).toString(QKeySequence::NativeText);
    lines.push_back(key.isEmpty() ? QStringLiteral("快捷键：未设置")
                                 : QStringLiteral("快捷键：%1").arg(key));
    if (!state.isEmpty()) lines.push_back(state);
    if (!enabled) lines.push_back(QStringLiteral("当前不可用"));
    return lines.join(QLatin1Char('\n'));
}

inline void refreshCommandPresentation(QAbstractButton* button)
{
    if (!button || !button->property("cgplay.commandPresentation.ready").toBool() ||
        button->property("cgplay.commandPresentation.updating").toBool()) return;
    button->setProperty("cgplay.commandPresentation.updating", true);
    const QString stateTitle = button->property("cgplay.commandPresentation.stateTitle").toString();
    const QString title = stateTitle.isEmpty()
        ? button->property("cgplay.commandPresentation.title").toString() : stateTitle;
    QString detail = button->property("cgplay.commandPresentation.stateDetail").toString();
    if (detail.isEmpty()) detail = button->property("cgplay.commandPresentation.detail").toString();
    const QString state = button->isCheckable()
        ? (button->isChecked() ? QStringLiteral("当前：已选中") : QStringLiteral("当前：未选中"))
        : QString{};
    const QString hint = commandPresentationText(title,
        button->property("cgplay.commandPresentation.shortcut").toString(), detail,
        button->isEnabled(), state);
    button->setToolTip(hint);
    button->setStatusTip(hint);
    button->setAccessibleName(title);
    button->setAccessibleDescription(hint);
    button->setProperty("cgplay.commandPresentation.updating", false);
}

namespace command_presentation_detail {
class ButtonObserver final : public QObject
{
public:
    explicit ButtonObserver(QAbstractButton* button) : QObject(button), _button(button)
    {
        button->installEventFilter(this);
        connect(button, &QAbstractButton::toggled, this,
                [button](bool) { refreshCommandPresentation(button); });
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == _button && event->type() == QEvent::EnabledChange)
            refreshCommandPresentation(_button);
        return QObject::eventFilter(object, event);
    }
private:
    QAbstractButton* _button = nullptr;
};
}

inline void applyCommandPresentation(QAbstractButton* button, const QString& title,
                                     const QString& effectiveShortcut,
                                     const QString& detail = {})
{
    if (!button) return;
    if (!button->property("cgplay.commandPresentation.ready").toBool())
        new command_presentation_detail::ButtonObserver(button);
    button->setProperty("cgplay.commandPresentation.title", title);
    button->setProperty("cgplay.commandPresentation.shortcut", effectiveShortcut);
    button->setProperty("cgplay.commandPresentation.detail", detail);
    button->setProperty("cgplay.commandPresentation.ready", true);
    refreshCommandPresentation(button);
}

inline void updateCommandPresentationState(QAbstractButton* button, const QString& title,
                                           const QString& detail = {})
{
    if (!button) return;
    button->setProperty("cgplay.commandPresentation.stateTitle", title);
    button->setProperty("cgplay.commandPresentation.stateDetail", detail);
    if (button->property("cgplay.commandPresentation.ready").toBool()) {
        refreshCommandPresentation(button);
    } else {
        // Standalone/secondary controls may not have a resolved command binding.
        // Do not claim either a default shortcut or an unbound shortcut there.
        const QString hint = detail.isEmpty() ? title : title + QLatin1Char('\n') + detail;
        button->setToolTip(hint);
        button->setAccessibleName(title);
        button->setAccessibleDescription(hint);
    }
}

inline void refreshCommandPresentation(QAction* action)
{
    if (!action || !action->property("cgplay.commandPresentation.ready").toBool() ||
        action->property("cgplay.commandPresentation.updating").toBool()) return;
    action->setProperty("cgplay.commandPresentation.updating", true);
    const QString state = action->isCheckable()
        ? (action->isChecked() ? QStringLiteral("当前：已选中") : QStringLiteral("当前：未选中"))
        : QString{};
    const QString hint = commandPresentationText(
        action->property("cgplay.commandPresentation.title").toString(),
        action->property("cgplay.commandPresentation.shortcut").toString(),
        action->property("cgplay.commandPresentation.detail").toString(),
        action->isEnabled(), state);
    action->setToolTip(hint);
    action->setStatusTip(hint);
    action->setProperty("cgplay.commandPresentation.updating", false);
}

inline void applyCommandPresentation(QAction* action, const QString& title,
                                     const QString& effectiveShortcut,
                                     const QString& detail = {})
{
    if (!action) return;
    if (!action->property("cgplay.commandPresentation.ready").toBool())
        QObject::connect(action, &QAction::changed, action,
                         [action] { refreshCommandPresentation(action); });
    action->setProperty("cgplay.commandPresentation.title", title);
    action->setProperty("cgplay.commandPresentation.shortcut", effectiveShortcut);
    action->setProperty("cgplay.commandPresentation.detail", detail);
    action->setProperty("cgplay.commandPresentation.ready", true);
    refreshCommandPresentation(action);
}

} // namespace cgplay
