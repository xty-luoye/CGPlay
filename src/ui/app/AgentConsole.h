#pragma once

#include "services/agent/AgentHost.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace cgplay {

class AgentConsole final : public QWidget
{
public:
    explicit AgentConsole(
        AgentHostPolicy policy = AgentHost::defaultPolicy(),
        QWidget* parent = nullptr);

    void refresh();

private:
    AgentHost _host;
    QLabel* _policyLabel = nullptr;
    QPlainTextEdit* _detailsView = nullptr;
};

} // namespace cgplay
