#include "AgentConsole.h"

#include <QJsonDocument>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace cgplay {

AgentConsole::AgentConsole(AgentHostPolicy policy, QWidget* parent)
    : QWidget(parent)
    , _host(policy)
{
    setObjectName(QStringLiteral("AgentConsole"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* headerLayout = new QHBoxLayout();
    auto* title = new QLabel(tr("Agent Console"), this);
    title->setStyleSheet(QStringLiteral("font-weight:600;"));
    headerLayout->addWidget(title);
    headerLayout->addStretch();

    auto* refreshButton = new QToolButton(this);
    refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshButton->setToolTip(tr("Refresh"));
    connect(refreshButton, &QToolButton::clicked, this, [this] { refresh(); });
    headerLayout->addWidget(refreshButton);
    layout->addLayout(headerLayout);

    _policyLabel = new QLabel(this);
    _policyLabel->setWordWrap(true);
    layout->addWidget(_policyLabel);

    _detailsView = new QPlainTextEdit(this);
    _detailsView->setObjectName(QStringLiteral("AgentConsoleDetails"));
    _detailsView->setReadOnly(true);
    _detailsView->setMinimumHeight(120);
    layout->addWidget(_detailsView, 1);

    refresh();
}

void AgentConsole::refresh()
{
    const AgentHostResult result = _host.inspect(QStringLiteral("console"));
    _policyLabel->setText(result.status);
    _detailsView->setPlainText(QString::fromUtf8(
        QJsonDocument(result.details).toJson(QJsonDocument::Indented)));
}

} // namespace cgplay
