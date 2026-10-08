#pragma once

#include <QString>
#include <QtPlugin>

namespace cgplay {

class IActivePlaybackView
{
public:
    virtual ~IActivePlaybackView() = default;

    virtual QString activeViewId() const = 0;
    virtual bool hasActiveView() const = 0;
};

} // namespace cgplay

#define CGPLAY_IACTIVEPLAYBACKVIEW_IID "com.cgplay.IActivePlaybackView"
Q_DECLARE_INTERFACE(cgplay::IActivePlaybackView, CGPLAY_IACTIVEPLAYBACKVIEW_IID)
