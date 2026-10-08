#include "QuickLookKeyboardHook.h"

namespace cgplay::quicklook {

QuickLookKeyboardHook* QuickLookKeyboardHook::s_instance = nullptr;

QuickLookKeyboardHook::QuickLookKeyboardHook(QObject* parent)
    : QObject(parent)
{
}

QuickLookKeyboardHook::~QuickLookKeyboardHook()
{
    stop();
}

bool QuickLookKeyboardHook::start(EventHandler handler)
{
    _handler = std::move(handler);
    if (_hook) {
        return true;
    }

    s_instance = this;
    _hook = SetWindowsHookExW(WH_KEYBOARD_LL, _hookProc, nullptr, 0);
    if (!_hook && s_instance == this) {
        s_instance = nullptr;
    }
    return _hook != nullptr;
}

void QuickLookKeyboardHook::stop()
{
    if (_hook) {
        UnhookWindowsHookEx(_hook);
        _hook = nullptr;
    }
    _handler = {};
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool QuickLookKeyboardHook::isRunning() const
{
    return _hook != nullptr;
}

LRESULT CALLBACK QuickLookKeyboardHook::_hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (s_instance && s_instance->_dispatch(code, wParam, lParam)) {
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool QuickLookKeyboardHook::_dispatch(int code, WPARAM wParam, LPARAM lParam)
{
    if (code < 0 || !_handler) {
        return false;
    }

    const auto* hookData = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    return hookData ? _handler(wParam, hookData) : false;
}

} // namespace cgplay::quicklook
