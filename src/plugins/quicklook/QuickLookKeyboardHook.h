#pragma once

#include <QObject>
#include <functional>

#include <windows.h>

namespace cgplay::quicklook {

class QuickLookKeyboardHook : public QObject
{
    Q_OBJECT
public:
    using EventHandler = std::function<bool(WPARAM, const KBDLLHOOKSTRUCT*)>;

    explicit QuickLookKeyboardHook(QObject* parent = nullptr);
    ~QuickLookKeyboardHook() override;

    bool start(EventHandler handler);
    void stop();
    bool isRunning() const;

private:
    static LRESULT CALLBACK _hookProc(int code, WPARAM wParam, LPARAM lParam);
    bool _dispatch(int code, WPARAM wParam, LPARAM lParam);

    HHOOK _hook = nullptr;
    EventHandler _handler;

    static QuickLookKeyboardHook* s_instance;
};

} // namespace cgplay::quicklook
