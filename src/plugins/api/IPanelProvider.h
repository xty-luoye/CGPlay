#pragma once

#include "plugins/api/HostExtensionPoints.h"

#include <QtPlugin>

namespace cgplay {

class IPanelProvider
{
public:
    virtual ~IPanelProvider() = default;

    virtual QVector<PanelContribution> panelContributions() const = 0;
};

} // namespace cgplay

#define CGPLAY_IPANELPROVIDER_IID "com.cgplay.IPanelProvider"
Q_DECLARE_INTERFACE(cgplay::IPanelProvider, CGPLAY_IPANELPROVIDER_IID)
