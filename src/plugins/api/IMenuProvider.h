#pragma once

#include "plugins/api/HostExtensionPoints.h"

#include <QtPlugin>

namespace cgplay {

class IMenuProvider
{
public:
    virtual ~IMenuProvider() = default;

    virtual QVector<MenuContribution> menuContributions() const = 0;
};

} // namespace cgplay

#define CGPLAY_IMENUPROVIDER_IID "com.cgplay.IMenuProvider"
Q_DECLARE_INTERFACE(cgplay::IMenuProvider, CGPLAY_IMENUPROVIDER_IID)
