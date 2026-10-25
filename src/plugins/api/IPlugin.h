#pragma once

#include <QString>
#include <QtPlugin>

namespace cgplay {

class IPlugin
{
public:
    virtual ~IPlugin() = default;

    virtual QString id() const = 0;
    // Both lifecycle methods must be idempotent. shutdown() deactivates runtime
    // resources; it does not imply that a dynamically loaded binary was unloaded.
    virtual void initialize() = 0;
    virtual void shutdown() = 0;
};

} // namespace cgplay

// ABI2 fixes the public playback layout and accompanies the matched native SDK.
#define CGPLAY_IPLUGIN_IID "com.cgplay.IPlugin/2.0"
Q_DECLARE_INTERFACE(cgplay::IPlugin, CGPLAY_IPLUGIN_IID)
