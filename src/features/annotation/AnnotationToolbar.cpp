#include "AnnotationToolbar.h"

#include <QApplication>
#include <QColorDialog>
#include <QPainter>
#include <QPainterPath>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace cgplay {

namespace {

QColor themeColor(const char* property, const QColor& fallback)
{
    const QColor value(qApp ? qApp->property(property).toString() : QString());
    return value.isValid() ? value : fallback;
}

QString cssColor(QColor color, int alpha = -1)
{
    if (alpha >= 0) color.setAlpha(qBound(0, alpha, 255));
    return color.name(QColor::HexArgb);
}

QColor blendColor(const QColor& foreground, const QColor& background, int amount)
{
    amount = qBound(0, amount, 100);
    return QColor((foreground.red() * amount + background.red() * (100 - amount)) / 100,
                  (foreground.green() * amount + background.green() * (100 - amount)) / 100,
                  (foreground.blue() * amount + background.blue() * (100 - amount)) / 100);
}

QIcon makeToolIcon(int tool)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor iconColor = themeColor("cgplay.textColor", QColor(QStringLiteral("#D8DEE7")));
    QPen pen(iconColor);
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (tool) {
    case AnnotationToolbar::Select:
        painter.drawLine(QPointF(7, 6), QPointF(7, 18));
        painter.drawLine(QPointF(7, 6), QPointF(15, 11));
        painter.drawLine(QPointF(7, 18), QPointF(10.5, 14.5));
        painter.drawLine(QPointF(10.5, 14.5), QPointF(14.5, 18));
        break;
    case AnnotationToolbar::Arrow:
        painter.drawLine(QPointF(6, 17), QPointF(18, 7));
        painter.drawLine(QPointF(12, 7), QPointF(18, 7));
        painter.drawLine(QPointF(18, 7), QPointF(18, 13));
        break;
    case AnnotationToolbar::Rectangle:
        painter.drawRoundedRect(QRectF(6.5, 7.0, 11.0, 10.0), 2, 2);
        break;
    case AnnotationToolbar::Circle:
        painter.drawEllipse(QRectF(6.5, 6.5, 11.0, 11.0));
        break;
    case AnnotationToolbar::Text:
        painter.drawLine(QPointF(7, 7), QPointF(17, 7));
        painter.drawLine(QPointF(12, 7), QPointF(12, 18));
        break;
    case AnnotationToolbar::FreeDraw:
    {
        QPainterPath path(QPointF(6, 15));
        path.cubicTo(QPointF(9, 9), QPointF(13, 18), QPointF(18, 8));
        painter.drawPath(path);
        break;
    }
    case AnnotationToolbar::Point:
        painter.setBrush(iconColor);
        painter.drawEllipse(QPointF(12, 12), 2.2, 2.2);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(12, 12), 5.5, 5.5);
        break;
    }

    return QIcon(pixmap);
}

} // namespace

const QVector<QColor> AnnotationToolbar::kColorPresets = {
    QColor(255, 86, 86),
    QColor(245, 196, 66),
    QColor(64, 205, 104),
    QColor(57, 160, 255),
    QColor(95, 107, 255),
    QColor(174, 88, 255),
    QColor(255, 255, 255)
};

AnnotationToolbar::AnnotationToolbar(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    auto makeToolButton = [&](int tool, const QString& tip) {
        auto* button = new QPushButton(this);
        button->setFixedSize(32, 32);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setIcon(makeToolIcon(tool));
        button->setIconSize(QSize(16, 16));
        button->setProperty("cgplay.annotationTool", tool);
        return button;
    };

    const QStringList toolTips = {
        QStringLiteral("选择"),
        QStringLiteral("箭头"),
        QStringLiteral("矩形"),
        QStringLiteral("圆形"),
        QStringLiteral("文字"),
        QStringLiteral("自由绘制"),
        QStringLiteral("点标记")
    };

    auto* toolGrid = new QGridLayout();
    toolGrid->setContentsMargins(0, 0, 0, 0);
    toolGrid->setHorizontalSpacing(6);
    toolGrid->setVerticalSpacing(6);

    for (int i = 0; i <= Point; ++i) {
        auto* button = makeToolButton(i, toolTips[i]);
        connect(button, &QPushButton::clicked, this, [this, i] { _onToolClicked(i); });
        _buttons.append(button);
        toolGrid->addWidget(button, 0, i);
    }
    _buttons[0]->setChecked(true);
    root->addLayout(toolGrid);

    auto* colorRow = new QHBoxLayout();
    colorRow->setContentsMargins(0, 0, 0, 0);
    colorRow->setSpacing(5);

    QVector<QPushButton*> swatches;
    for (const auto& color : kColorPresets) {
        auto* swatch = new QPushButton(this);
        swatch->setFixedSize(24, 24);
        swatch->setToolTip(color.name());
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setProperty("cgplay.annotationSwatch", color);
        connect(swatch, &QPushButton::clicked, this, [this, color] { setColor(color); });
        swatches.append(swatch);
        colorRow->addWidget(swatch);
    }

    auto* customButton = new QPushButton("+", this);
    customButton->setFixedSize(22, 22);
    customButton->setToolTip(QStringLiteral("自定义颜色"));
    customButton->setCursor(Qt::PointingHandCursor);
    connect(customButton, &QPushButton::clicked, this, &AnnotationToolbar::_pickColor);
    colorRow->addWidget(customButton);

    _colorIndicator = new QLabel(this);
    _colorIndicator->setFixedSize(0, 0);
    _colorIndicator->hide();
    colorRow->addWidget(_colorIndicator);
    colorRow->addStretch();
    root->addLayout(colorRow);

    _updateColorIndicator();
    setStyleSheet("background:transparent;");

    const auto applyRuntimeTheme = [this, swatches, customButton] {
        const QColor panel = themeColor("cgplay.panelColor", QColor(QStringLiteral("#111418")));
        const QColor toolbar = themeColor("cgplay.toolbarColor", panel);
        const QColor text = themeColor("cgplay.textColor", QColor(QStringLiteral("#D8DEE7")));
        const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 22));
        const QColor accent = themeColor("cgplay.accentColor", QColor(QStringLiteral("#FF8A3D")));
        const QColor button = blendColor(panel, toolbar, 70);
        const QColor hover = blendColor(accent, button, 18);
        const QColor checked = blendColor(accent, button, 28);
        const QString toolStyle = QStringLiteral(
            "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:8px;}"
            "QPushButton:hover{background:%4;color:%5;border-color:%6;}"
            "QPushButton:checked{background:%7;color:%8;border-color:%8;}")
            .arg(cssColor(button, 170), cssColor(blendColor(text, panel, 68)), cssColor(border),
                 cssColor(hover), cssColor(text), cssColor(blendColor(accent, border, 55)),
                 cssColor(checked), cssColor(accent));
        for (int i = 0; i < _buttons.size(); ++i) {
            _buttons[i]->setIcon(makeToolIcon(i));
            _buttons[i]->setStyleSheet(toolStyle);
        }
        for (auto* swatch : swatches) {
            const QColor swatchColor = swatch->property("cgplay.annotationSwatch").value<QColor>();
            swatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid %2;border-radius:6px;}"
                "QPushButton:hover{border-color:%3;}")
                .arg(cssColor(swatchColor), cssColor(border), cssColor(text)));
        }
        customButton->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:6px;font-size:12px;font-weight:700;}"
            "QPushButton:hover{border-color:%4;color:%4;}")
            .arg(cssColor(panel), cssColor(text), cssColor(border), cssColor(accent)));
        _updateColorIndicator();
    };
    applyRuntimeTheme();
    if (qApp) connect(qApp, &QApplication::paletteChanged, this,
                      [applyRuntimeTheme](const QPalette&) { applyRuntimeTheme(); });
}

void AnnotationToolbar::setTool(Tool tool)
{
    if (tool >= Select && tool <= Point) {
        _onToolClicked(static_cast<int>(tool));
    }
}

void AnnotationToolbar::_onToolClicked(int tool)
{
    for (int i = 0; i < _buttons.size(); ++i) {
        _buttons[i]->setChecked(i == tool);
    }
    _currentTool = static_cast<Tool>(tool);
    Q_EMIT toolChanged(tool, _currentColor);
}

void AnnotationToolbar::setColor(const QColor& color)
{
    _currentColor = color;
    _updateColorIndicator();
    Q_EMIT colorChanged(color);
}

void AnnotationToolbar::_pickColor()
{
    const QColor color = QColorDialog::getColor(_currentColor, this, QStringLiteral("选择批注颜色"));
    if (color.isValid()) {
        setColor(color);
    }
}

void AnnotationToolbar::_updateColorIndicator()
{
    const QColor border = themeColor("cgplay.borderColor", QColor(255, 255, 255, 56));
    _colorIndicator->setStyleSheet(QString(
        "QLabel{background:%1;border:2px solid %2;border-radius:6px;}")
        .arg(cssColor(_currentColor), cssColor(border)));
}

} // namespace cgplay
