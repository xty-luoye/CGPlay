#include "TopBar.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWindow>

#include <cmath>

namespace cgplay {

static const char* kTopBarBg = "#0b1016";
static const char* kTopBarGlow = "#121a22";
static const char* kText = "#dce1e8";
static const char* kMuted = "#8d96a2";
static const char* kAccent = "#ff7a1a";

namespace {

QColor themeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

QColor surfaceColor(const char* propertyName, const QColor& fallback, const char* opacityProperty, int fallbackOpacity)
{
    QColor color = themeColor(propertyName, fallback);
    const int opacity = qApp ? qBound(0, qApp->property(opacityProperty).toInt(), 100) : fallbackOpacity;
    color.setAlpha(qRound(opacity * 255.0 / 100.0));
    return color;
}

bool lightTheme()
{
    return qApp && qApp->property("cgplay.themeMode").toString().compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0;
}

class BrandBadge final : public QWidget
{
public:
    explicit BrandBadge(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(29, 20);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath metal;
        metal.moveTo(5.1, 2.2);
        metal.lineTo(8.9, 2.2);
        metal.cubicTo(7.6, 4.9, 7.6, 15.1, 8.9, 17.8);
        metal.lineTo(5.1, 17.8);
        metal.cubicTo(2.6, 17.8, 1.3, 16.4, 1.3, 13.7);
        metal.lineTo(1.3, 6.3);
        metal.cubicTo(1.3, 3.6, 2.6, 2.2, 5.1, 2.2);
        metal.closeSubpath();

        QPainterPath flag;
        flag.moveTo(10.0, 3.0);
        flag.lineTo(22.2, 3.0);
        flag.cubicTo(24.8, 3.0, 26.5, 4.3, 26.5, 6.4);
        flag.lineTo(26.5, 13.6);
        flag.cubicTo(26.5, 15.7, 24.8, 17.0, 22.2, 17.0);
        flag.lineTo(10.0, 17.0);
        flag.cubicTo(7.7, 14.9, 7.9, 11.8, 9.6, 10.0);
        flag.cubicTo(7.9, 8.2, 7.7, 5.1, 10.0, 3.0);
        flag.closeSubpath();

        auto fillShadow = [&](const QPainterPath& path, const QPointF& offset, int alpha) {
            painter.save();
            painter.translate(offset);
            painter.fillPath(path, QColor(0, 0, 0, alpha));
            painter.restore();
        };

        fillShadow(metal, QPointF(0.0, 0.8), 52);
        fillShadow(flag, QPointF(0.0, 0.8), 44);

        QLinearGradient metalGrad(1.3, 2.2, 8.9, 17.8);
        metalGrad.setColorAt(0.0, QColor("#8ea1b4"));
        metalGrad.setColorAt(0.48, QColor("#718395"));
        metalGrad.setColorAt(1.0, QColor("#566574"));
        painter.fillPath(metal, metalGrad);

        QLinearGradient flagGrad(8.8, 2.8, 26.5, 17.0);
        flagGrad.setColorAt(0.0, QColor("#ff9b42"));
        flagGrad.setColorAt(0.52, QColor("#ff7a1a"));
        flagGrad.setColorAt(1.0, QColor("#eb6213"));
        painter.fillPath(flag, flagGrad);

        painter.setPen(QPen(QColor(255, 255, 255, 22), 0.9));
        painter.drawPath(metal);
        painter.setPen(QPen(QColor(255, 214, 180, 36), 0.9));
        painter.drawPath(flag);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 28));
        painter.drawRoundedRect(QRectF(11.4, 4.0, 12.2, 2.2), 1.1, 1.1);

        QPainterPath playShadow;
        playShadow.moveTo(12.7, 7.0);
        playShadow.lineTo(12.7, 13.0);
        playShadow.lineTo(18.2, 10.0);
        playShadow.closeSubpath();
        fillShadow(playShadow, QPointF(0.55, 0.7), 62);

        QPainterPath play;
        play.moveTo(12.2, 6.5);
        play.lineTo(12.2, 12.9);
        play.lineTo(18.0, 9.7);
        play.closeSubpath();
        painter.fillPath(play, QColor("#f4f7fb"));
    }
};

} // namespace

struct TopBar::Private
{
    QLabel* logoLabel = nullptr;
    QPushButton* btnFile = nullptr;
    QPushButton* btnView = nullptr;
    QPushButton* btnWindow = nullptr;
    QPushButton* btnOtio = nullptr;
    QPushButton* btnColor = nullptr;
    QPushButton* btnAudio = nullptr;
    QPushButton* btnPlayback = nullptr;
    QPushButton* btnHelp = nullptr;

    QLabel* lblFPS = nullptr;
    QLabel* lblDecoder = nullptr;
    QLabel* lblCodec = nullptr;
    QLabel* lblResolution = nullptr;
    QPushButton* btnSettings = nullptr;
    QPushButton* btnMenu = nullptr;
    QPushButton* btnMin = nullptr;
    QPushButton* btnMax = nullptr;
    QPushButton* btnClose = nullptr;

    QPoint dragPos;
    bool dragging = false;
};

TopBar::TopBar(QWidget* parent)
    : QWidget(parent)
    , _p(std::make_unique<Private>())
{
    setFixedHeight(56);
    _setupUI();
}

TopBar::~TopBar() = default;

QPushButton* TopBar::fileMenuBtn() const { return _p->btnFile; }
QPushButton* TopBar::viewMenuBtn() const { return _p->btnView; }
QPushButton* TopBar::windowMenuBtn() const { return _p->btnWindow; }
QPushButton* TopBar::otioMenuBtn() const { return _p->btnOtio; }
QPushButton* TopBar::colorMenuBtn() const { return _p->btnColor; }
QPushButton* TopBar::audioMenuBtn() const { return _p->btnAudio; }
QPushButton* TopBar::playMenuBtn() const { return _p->btnPlayback; }
QPushButton* TopBar::helpMenuBtn() const { return _p->btnHelp; }
QLabel* TopBar::fpsLabel() const { return _p->lblFPS; }
QLabel* TopBar::decoderLabel() const { return _p->lblDecoder; }
QLabel* TopBar::codecLabel() const { return _p->lblCodec; }
QLabel* TopBar::resolutionLabel() const { return _p->lblResolution; }
QPushButton* TopBar::settingsButton() const { return _p->btnSettings; }
QPushButton* TopBar::menuButton() const { return _p->btnMenu; }

QPushButton* TopBar::_makeMenuBtn(const QString& text)
{
    auto* btn = new QPushButton(text, this);
    btn->setFlat(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedHeight(56);
    btn->setStyleSheet(QString(
        "QPushButton{color:%1;background:transparent;border:none;padding:0 12px;"
        "font-family:'Microsoft YaHei UI','Segoe UI';font-size:13px;}"
        "QPushButton:hover{color:%2;background:rgba(255,255,255,0.028);}"
        "QPushButton:pressed{color:%3;}")
        .arg(kMuted, kText, kAccent));
    return btn;
}

QPushButton* TopBar::_makeWindowBtn(const QString& text, const QString& tip, bool danger)
{
    auto* btn = new QPushButton(text, this);
    btn->setFlat(true);
    btn->setToolTip(tip);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedSize(34, 30);
    btn->setStyleSheet(QString(
        "QPushButton{color:%1;background:transparent;border:none;border-radius:7px;font-size:0px;}"
        "QPushButton:hover{color:#ffffff;background:%2;}")
        .arg(kMuted, danger ? "#b72525" : "rgba(255,255,255,0.065)"));
    return btn;
}

QLabel* TopBar::_makeStatusLabel(const QString& text)
{
    auto* label = new QLabel(text, this);
    label->setStyleSheet(
        "QLabel{color:#dce1e8;background:transparent;font-family:'Segoe UI';font-size:12px;padding:0 10px;}");
    return label;
}

void TopBar::_setupUI()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(18, 0, 10, 0);
    layout->setSpacing(0);

    auto* badge = new BrandBadge(this);
    layout->addWidget(badge, 0, Qt::AlignVCenter);
    layout->addSpacing(8);

    _p->logoLabel = new QLabel("CGPlay", this);
    _p->logoLabel->setStyleSheet(
        "QLabel{color:#ffffff;background:transparent;font-family:'Segoe UI';font-size:16px;font-weight:700;}");
    layout->addWidget(_p->logoLabel);
    layout->addSpacing(18);

    _p->btnFile = _makeMenuBtn(QStringLiteral("文件"));
    _p->btnView = _makeMenuBtn(QStringLiteral("视图"));
    _p->btnWindow = _makeMenuBtn(QStringLiteral("窗口"));
    _p->btnOtio = _makeMenuBtn(QStringLiteral("OTIO"));
    _p->btnColor = _makeMenuBtn(QStringLiteral("颜色"));
    _p->btnAudio = _makeMenuBtn(QStringLiteral("音频"));
    _p->btnPlayback = _makeMenuBtn(QStringLiteral("播放"));
    _p->btnHelp = _makeMenuBtn(QStringLiteral("帮助"));

    _p->btnFile->setText(QString::fromUtf8("文件"));
    _p->btnView->setText(QString::fromUtf8("视图"));
    _p->btnWindow->setText(QString::fromUtf8("窗口"));
    _p->btnColor->setText(QString::fromUtf8("颜色"));
    _p->btnAudio->setText(QString::fromUtf8("音频"));
    _p->btnPlayback->setText(QString::fromUtf8("播放"));
    _p->btnHelp->setText(QString::fromUtf8("帮助"));

    layout->addWidget(_p->btnFile);
    layout->addWidget(_p->btnView);
    layout->addWidget(_p->btnWindow);
    layout->addWidget(_p->btnOtio);
    layout->addWidget(_p->btnColor);
    layout->addWidget(_p->btnAudio);
    layout->addWidget(_p->btnPlayback);
    layout->addWidget(_p->btnHelp);
    layout->addStretch();

    auto addDivider = [this, layout]() {
        auto* divider = new QFrame(this);
        divider->setObjectName(QStringLiteral("TopBarStatusDivider"));
        divider->setFixedSize(1, 18);
        divider->setStyleSheet("QFrame{background:rgba(255,255,255,0.06);border:none;}");
        layout->addWidget(divider);
        layout->addSpacing(7);
    };

    _p->lblFPS = _makeStatusLabel("-- FPS");
    _p->lblDecoder = _makeStatusLabel("--");
    _p->lblCodec = _makeStatusLabel("--");
    _p->lblResolution = _makeStatusLabel("--");

    layout->addWidget(_p->lblFPS);
    addDivider();
    layout->addWidget(_p->lblDecoder);
    addDivider();
    layout->addWidget(_p->lblCodec);
    addDivider();
    layout->addWidget(_p->lblResolution);
    layout->addSpacing(6);

    _p->btnSettings = _makeWindowBtn(QString(), QStringLiteral("设置"));
    _p->btnSettings->setObjectName(QStringLiteral("TopBarSettings"));
    _p->btnSettings->setAccessibleName(QStringLiteral("设置"));
    _p->btnMenu = _makeWindowBtn(QString(), QStringLiteral("菜单"));
    _p->btnMin = _makeWindowBtn(QString(), QStringLiteral("最小化"));
    _p->btnMax = _makeWindowBtn(QString(), QStringLiteral("最大化"));
    _p->btnClose = _makeWindowBtn(QString(), QStringLiteral("关闭"), true);

    _p->btnSettings->setToolTip(QStringLiteral("设置"));
    _p->btnMenu->setToolTip(QString::fromUtf8("菜单"));
    _p->btnMin->setToolTip(QString::fromUtf8("最小化"));
    _p->btnMax->setToolTip(QString::fromUtf8("最大化 / 还原"));
    _p->btnClose->setToolTip(QString::fromUtf8("关闭"));

    layout->addWidget(_p->btnSettings);
    layout->addSpacing(2);
    layout->addWidget(_p->btnMenu);
    layout->addSpacing(4);
    layout->addWidget(_p->btnMin);
    layout->addWidget(_p->btnMax);
    layout->addWidget(_p->btnClose);

    connect(_p->btnMin, &QPushButton::clicked, this, [this] {
        if (auto* w = window()) {
            w->showMinimized();
        }
    });
    connect(_p->btnMax, &QPushButton::clicked, this, &TopBar::_toggleMaximized);
    connect(_p->btnClose, &QPushButton::clicked, this, [this] {
        if (auto* w = window()) {
            w->close();
        }
    });
}

void TopBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int available = event ? event->size().width() : width();
    const bool hideStatus = available < 1180;
    const bool compactMenus = available < 930;
    const bool narrowMenus = available < 720;

    for (QLabel* label : {_p->lblFPS, _p->lblDecoder, _p->lblCodec, _p->lblResolution}) {
        if (label) label->setVisible(!hideStatus);
    }
    for (QFrame* divider : findChildren<QFrame*>(QStringLiteral("TopBarStatusDivider"))) {
        if (divider) divider->setVisible(!hideStatus);
    }

    for (QPushButton* button : {_p->btnWindow, _p->btnOtio, _p->btnColor, _p->btnAudio}) {
        if (button) button->setVisible(!compactMenus);
    }
    for (QPushButton* button : {_p->btnFile, _p->btnView, _p->btnPlayback, _p->btnHelp}) {
        if (button) button->setVisible(!narrowMenus);
    }
}

void TopBar::_toggleMaximized()
{
    if (auto* w = window()) {
        w->isMaximized() ? w->showNormal() : w->showMaximized();
    }
}

void TopBar::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool light = lightTheme();
    const QColor toolbar = surfaceColor("cgplay.toolbarColor", QColor(kTopBarBg), "cgplay.toolbarOpacity", 92);
    const QColor accent = themeColor("cgplay.accentColor", QColor(kAccent));
    const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 24));

    QLinearGradient base(0, 0, 0, height());
    base.setColorAt(0.0, light ? toolbar.lighter(104) : toolbar.darker(112));
    base.setColorAt(1.0, light ? toolbar.darker(104) : toolbar);
    painter.fillRect(rect(), base);

    if (!light) {
        QRadialGradient glow(width() * 0.64, -8.0, width() * 0.34);
        glow.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 42));
        glow.setColorAt(0.38, QColor(accent.red(), accent.green(), accent.blue(), 14));
        glow.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.fillRect(rect(), glow);
    }

    painter.setPen(Qt::NoPen);
    for (int y = 0; y < height(); y += 6) {
        for (int x = 0; x < width(); x += 6) {
            const quint32 seed =
                static_cast<quint32>((x + 5) * 73856093u ^ (y + 3) * 19349663u);
            const int alpha = light ? 1 + static_cast<int>(seed % 2u) : 2 + static_cast<int>(seed % 4u);
            painter.setBrush(light ? QColor(0, 0, 0, alpha) : QColor(255, 255, 255, alpha));
            painter.drawRect(x, y, 1, 1);
        }
    }

    auto drawWindowGlyph = [&](QPushButton* button, int type, bool danger = false) {
        if (!button) {
            return;
        }

        const QRect r = button->geometry();
        const bool hover = button->underMouse();
        const QColor textColor = themeColor("cgplay.textColor", QColor(kText));
        const QColor color = danger ? textColor : (hover ? textColor.lighter(115) : textColor);
        QPen pen(color);
        pen.setWidthF(1.35);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        const QPointF c = r.center();
        switch (type) {
        case 0:
            painter.drawLine(QPointF(c.x() - 5.0, c.y() + 0.5), QPointF(c.x() + 5.0, c.y() + 0.5));
            break;
        case 1:
            painter.drawRect(QRectF(c.x() - 4.8, c.y() - 4.5, 9.6, 9.0));
            break;
        case 2:
            painter.drawLine(QPointF(c.x() - 4.2, c.y() - 4.2), QPointF(c.x() + 4.2, c.y() + 4.2));
            painter.drawLine(QPointF(c.x() + 4.2, c.y() - 4.2), QPointF(c.x() - 4.2, c.y() + 4.2));
            break;
        case 3:
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRectF(c.x() - 1.25, c.y() - 5.2, 2.5, 2.5));
            painter.drawEllipse(QRectF(c.x() - 1.25, c.y() - 1.25, 2.5, 2.5));
            painter.drawEllipse(QRectF(c.x() - 1.25, c.y() + 2.7, 2.5, 2.5));
            break;
        case 4: {
            painter.drawEllipse(QRectF(c.x() - 3.1, c.y() - 3.1, 6.2, 6.2));
            for (int i = 0; i < 8; ++i) {
                const double angle = i * 3.14159265358979323846 / 4.0;
                const QPointF inner(c.x() + std::cos(angle) * 5.0, c.y() + std::sin(angle) * 5.0);
                const QPointF outer(c.x() + std::cos(angle) * 7.0, c.y() + std::sin(angle) * 7.0);
                painter.drawLine(inner, outer);
            }
            break;
        }
        default:
            break;
        }
    };

    drawWindowGlyph(_p->btnSettings, 4);
    drawWindowGlyph(_p->btnMenu, 3);
    drawWindowGlyph(_p->btnMin, 0);
    drawWindowGlyph(_p->btnMax, 1);
    drawWindowGlyph(_p->btnClose, 2, true);

    painter.setPen(QPen(light ? QColor(border.red(), border.green(), border.blue(), 170) : QColor(255, 255, 255, 14), 1));
    painter.drawLine(0, height() - 1, width(), height() - 1);
    QWidget::paintEvent(event);
}

void TopBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (auto* w = window()) {
            if (auto* handle = w->windowHandle(); handle && handle->startSystemMove()) {
                _p->dragging = false;
                event->accept();
                return;
            }
        }
        _p->dragging = true;
        _p->dragPos = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TopBar::mouseMoveEvent(QMouseEvent* event)
{
    if (_p->dragging && (event->buttons() & Qt::LeftButton)) {
        if (auto* w = window()) {
            if (w->isMaximized()) {
                w->showNormal();
                _p->dragPos = QPoint(width() / 2, height() / 2);
            }
            w->move(event->globalPosition().toPoint() - _p->dragPos);
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TopBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        _p->dragging = false;
    }
    QWidget::mouseReleaseEvent(event);
}

void TopBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        _toggleMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace cgplay
