#include "NavigationRail.h"

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QToolButton>
#include <QVBoxLayout>

namespace cgplay {

static const char* kBg = "#111418";
static const char* kBorder = "#252B33";
static const char* kAccent = "#FF8A3D";
static const char* kIcon = "#E8EDF4";

static QPixmap renderNavIcon(const QString& path, const QColor& color)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const QString lower = path.toLower();
    if (lower.contains(QStringLiteral("playlist"))) {
        painter.drawEllipse(QPointF(7, 7), 1.2, 1.2);
        painter.drawLine(QPointF(10, 7), QPointF(18, 7));
        painter.drawEllipse(QPointF(7, 12), 1.2, 1.2);
        painter.drawLine(QPointF(10, 12), QPointF(18, 12));
        painter.drawEllipse(QPointF(7, 17), 1.2, 1.2);
        painter.drawLine(QPointF(10, 17), QPointF(18, 17));
    } else if (lower.contains(QStringLiteral("review"))) {
        painter.drawRoundedRect(QRectF(5, 5, 14, 14), 3, 3);
        painter.drawLine(QPointF(8, 9), QPointF(16, 9));
        painter.drawLine(QPointF(8, 12), QPointF(14, 12));
        painter.drawLine(QPointF(8, 15), QPointF(12, 15));
        painter.drawLine(QPointF(14, 16), QPointF(16, 18));
        painter.drawLine(QPointF(16, 18), QPointF(19, 15));
    } else if (lower.contains(QStringLiteral("compare"))) {
        painter.drawRoundedRect(QRectF(4, 6, 7, 12), 2, 2);
        painter.drawRoundedRect(QRectF(13, 6, 7, 12), 2, 2);
        painter.drawLine(QPointF(12, 8), QPointF(10, 12));
        painter.drawLine(QPointF(10, 12), QPointF(12, 16));
        painter.drawLine(QPointF(12, 8), QPointF(14, 12));
        painter.drawLine(QPointF(14, 12), QPointF(12, 16));
    } else if (lower.contains(QStringLiteral("versions"))) {
        painter.drawRoundedRect(QRectF(6, 6, 10, 10), 2, 2);
        painter.drawRoundedRect(QRectF(8, 8, 10, 10), 2, 2);
        painter.drawLine(QPointF(15, 4), QPointF(19, 4));
        painter.drawLine(QPointF(19, 4), QPointF(19, 8));
        painter.drawLine(QPointF(19, 4), QPointF(14, 9));
    } else if (lower.contains(QStringLiteral("timeline"))) {
        painter.drawLine(QPointF(5, 12), QPointF(19, 12));
        painter.drawLine(QPointF(8, 8), QPointF(8, 16));
        painter.drawLine(QPointF(12, 6), QPointF(12, 18));
        painter.drawLine(QPointF(16, 8), QPointF(16, 16));
        painter.drawEllipse(QPointF(8, 12), 1.2, 1.2);
        painter.drawEllipse(QPointF(12, 12), 1.2, 1.2);
        painter.drawEllipse(QPointF(16, 12), 1.2, 1.2);
    } else if (lower.contains(QStringLiteral("settings"))) {
        painter.drawEllipse(QPointF(12, 12), 3.4, 3.4);
        painter.drawLine(QPointF(12, 4), QPointF(12, 7));
        painter.drawLine(QPointF(12, 17), QPointF(12, 20));
        painter.drawLine(QPointF(4, 12), QPointF(7, 12));
        painter.drawLine(QPointF(17, 12), QPointF(20, 12));
        painter.drawLine(QPointF(6.3, 6.3), QPointF(8.4, 8.4));
        painter.drawLine(QPointF(15.6, 6.3), QPointF(13.6, 8.4));
        painter.drawLine(QPointF(6.3, 15.6), QPointF(8.4, 13.6));
        painter.drawLine(QPointF(15.6, 15.6), QPointF(13.6, 13.6));
    } else {
        painter.drawRoundedRect(QRectF(4, 4, 16, 16), 3, 3);
    }

    return pixmap;
}

static QIcon makeNavIcon(const QString& path)
{
    const QIcon resourceIcon(path);
    if (!resourceIcon.isNull()) {
        return resourceIcon;
    }
    QIcon icon;
    icon.addPixmap(renderNavIcon(path, QColor(kIcon)), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderNavIcon(path, QColor(kAccent)), QIcon::Normal, QIcon::On);
    icon.addPixmap(renderNavIcon(path, QColor(kAccent)), QIcon::Selected, QIcon::On);
    icon.addPixmap(renderNavIcon(path, QColor("#FFFFFF")), QIcon::Active, QIcon::Off);
    return icon;
}

NavigationRail::NavigationRail(QWidget* parent)
    : QWidget(parent)
{
    setMinimumWidth(56);
    setMaximumWidth(120);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 14, 0, 14);
    layout->setSpacing(12);
    layout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    auto makeButton = [this](const QString& iconPath, const QString& tip) {
        auto* button = new QToolButton(this);
        button->setText(QString());
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(false);
        button->setIcon(makeNavIcon(iconPath));
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(42, 42);
        button->setStyleSheet(QString(
            "QToolButton{background:transparent;border:1px solid transparent;border-radius:8px;padding:0;color:transparent;}"
            "QToolButton:hover{background:rgba(32,40,50,0.64);border-color:rgba(255,140,50,0.18);}"
            "QToolButton:checked{background:rgba(255,140,50,0.10);border-color:%1;}")
            .arg(kAccent));
        return button;
    };

    const struct Entry {
        const char* icon;
        const char* tip;
    } entries[] = {
        {":/cgplay/icons/nav_playlist.svg", "播放列表"},
        {":/cgplay/icons/nav_review.svg",   "批注记录"},
        {":/cgplay/icons/nav_compare.svg",  "对比"},
        {":/cgplay/icons/nav_versions.svg", "版本"},
        {":/cgplay/icons/nav_timeline.svg", "时间轴"},
        {":/cgplay/icons/nav_settings.svg", "设置"},
    };

    const QStringList navTips{
        QString::fromUtf8(u8"播放列表"), QString::fromUtf8(u8"批注记录"),
        QString::fromUtf8(u8"对比"), QString::fromUtf8(u8"版本"),
        QString::fromUtf8(u8"时间线"), QString::fromUtf8(u8"设置")};
    for (int i = 0; i < 6; ++i) {
        if (i == Settings) {
            layout->addStretch();
        }
        _btn[i] = makeButton(QString::fromLatin1(entries[i].icon), QString::fromUtf8(entries[i].tip));
        _btn[i]->setToolTip(navTips.at(i));
        connect(_btn[i], &QToolButton::clicked, this, [this, i] { _select(i); });
        layout->addWidget(_btn[i], 0, Qt::AlignHCenter);
    }

    _page = Review;
    _btn[Review]->setChecked(true);
}

QToolButton* NavigationRail::btn(int i) const
{
    return (i >= 0 && i < 6) ? _btn[i] : nullptr;
}

void NavigationRail::_select(int i)
{
    _page = static_cast<Page>(i);
    for (int j = 0; j < 6; ++j) {
        if (_btn[j]) {
            _btn[j]->setChecked(j == i);
        }
    }
    Q_EMIT pageChanged(i);
}

void NavigationRail::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QLinearGradient glass(0, 0, 0, height());
    glass.setColorAt(0.0, QColor(17, 23, 30, 214));
    glass.setColorAt(0.48, QColor(12, 18, 24, 204));
    glass.setColorAt(1.0, QColor(8, 13, 18, 222));
    painter.fillRect(rect(), glass);

    QLinearGradient topEdge(0, 0, 0, 42);
    topEdge.setColorAt(0.0, QColor(255, 255, 255, 18));
    topEdge.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(QRect(0, 0, width(), 42), topEdge);

    painter.setPen(QPen(QColor(255, 255, 255, 16), 1));
    painter.drawLine(width() - 1, 0, width() - 1, height());
}

} // namespace cgplay
