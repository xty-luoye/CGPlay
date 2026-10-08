#pragma once

#include "plugins/api/HostExtensionPoints.h"

#include <QtPlugin>

namespace cgplay {

class ICommandProvider
{
public:
    virtual ~ICommandProvider() = default;

    virtual QVector<CommandDescriptor> commandDescriptors() const = 0;
};

} // namespace cgplay

#define CGPLAY_ICOMMANDPROVIDER_IID "com.cgplay.ICommandProvider"
Q_DECLARE_INTERFACE(cgplay::ICommandProvider, CGPLAY_ICOMMANDPROVIDER_IID)
