#pragma once

#include "plugins/api/HostExtensionPoints.h"

#include <QtPlugin>

namespace cgplay {

class IToolbarProvider
{
public:
    virtual ~IToolbarProvider() = default;

    virtual QVector<ToolbarContribution> toolbarContributions() const = 0;
};

} // namespace cgplay

#define CGPLAY_ITOOLBARPROVIDER_IID "com.cgplay.IToolbarProvider"
Q_DECLARE_INTERFACE(cgplay::IToolbarProvider, CGPLAY_ITOOLBARPROVIDER_IID)
