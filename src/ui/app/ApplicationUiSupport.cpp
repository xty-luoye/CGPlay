#include "ApplicationInternal.h"
#include "CommandPresentation.h"

namespace cgplay {
void MainWindow::_refreshCommandPresentation()
{
    QHash<QString, const CommandDescriptor*> commands;
    for (const auto& descriptor : _p->commandDescriptors)
        commands.insert(descriptor.id, &descriptor);

    // Keep this bounded to player chrome; do not traverse the AI workspace or
    // the large settings editor, and do not run a polling timer or repolish.
    QList<QWidget*> roots{_p->playbackBar, _p->compareBar, _p->viewer.data(),
                          _p->annoToolbar.data(), _p->topBar};
    if (auto* custom = findChild<QWidget*>(QStringLiteral("cgplayCustomToolbar"))) roots.push_back(custom);
    // The existing volume popup is a parentless tool window. An owner marker
    // lets us include only this player's popup without walking global widgets.
    for (QWidget* popup : QApplication::topLevelWidgets()) {
        if (popup->property("cgplay.commandPresentation.owner").value<QObject*>() == _p->playbackBar &&
            _p->playbackBar) roots.push_back(popup);
    }
    const QStringList annotationCommands{
        QStringLiteral("annotation.tool.select"), QStringLiteral("annotation.tool.arrow"),
        QStringLiteral("annotation.tool.rectangle"), QStringLiteral("annotation.tool.circle"),
        QStringLiteral("annotation.tool.text"), QStringLiteral("annotation.tool.freeDraw")};
    QSet<QAbstractButton*> visitedButtons;
    for (QWidget* root : roots) {
        if (!root) continue;
        for (QAbstractButton* button : root->findChildren<QAbstractButton*>()) {
            if (visitedButtons.contains(button)) continue;
            visitedButtons.insert(button);
            // Macro IDs are user-owned and may happen to equal a command ID.
            // Their ordered multi-command descriptions are maintained by the
            // custom-toolbar builder, not replaced by a single-command hint.
            if (button->property("cgplay.customButton").toBool()) continue;
            QString id = button->property("commandId").toString();
            if (id.isEmpty() && button->property("cgplay.annotationTool").isValid()) {
                const int tool = button->property("cgplay.annotationTool").toInt();
                if (tool >= 0 && tool < annotationCommands.size()) {
                    id = annotationCommands[tool];
                    button->setProperty("commandId", id);
                }
            }
            if (const auto* descriptor = commands.value(id, nullptr)) {
                applyCommandPresentation(button, descriptor->text, descriptor->shortcut, descriptor->toolTip);
            } else if (button->accessibleName().isEmpty()) {
                // Unregistered controls (for example direct B selection and
                // point annotation) must not advertise a different command's key.
                const QString label = button->toolTip().isEmpty() ? button->text() : button->toolTip();
                button->setAccessibleName(label);
                button->setAccessibleDescription(button->toolTip());
            }
        }
    }

    QList<QMenu*> menus;
    if (_p->viewer) menus.append(_p->viewer->findChildren<QMenu*>());
    if (menuBar()) {
        for (QAction* action : menuBar()->actions()) if (action->menu()) menus.push_back(action->menu());
    }
    QSet<QMenu*> visitedMenus;
    QSet<QAction*> visitedActions;
    while (!menus.isEmpty()) {
        QMenu* menu = menus.takeLast();
        if (!menu || visitedMenus.contains(menu)) continue;
        visitedMenus.insert(menu);
        menu->setToolTipsVisible(true);
        for (QAction* action : menu->actions()) {
            if (action->menu()) menus.push_back(action->menu());
            if (visitedActions.contains(action)) continue;
            visitedActions.insert(action);
            QString id = action->property("commandId").toString();
            if (id.isEmpty()) id = action->property("cgplay.command.id").toString();
            if (id.isEmpty()) id = action->property("cgplay.commandPresentation.id").toString();
            if (const auto* descriptor = commands.value(id, nullptr))
                applyCommandPresentation(action, descriptor->text, descriptor->shortcut, descriptor->toolTip);
        }
    }
}

void applyPlayerWindowOpacity(QWidget* window, int percent)
{
    if (!window) return;
    percent = qBound(40, percent, 100);
    window->setWindowOpacity(percent / 100.0);
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) return;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (percent < 100) {
        if (!(style & WS_EX_LAYERED)) {
            style |= WS_EX_LAYERED;
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
        }
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(qRound(percent * 255.0 / 100.0)), LWA_ALPHA);
    } else if (style & WS_EX_LAYERED) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#endif
}

bool runProcessResponsive(const QString& program, const QStringList& arguments, int timeoutMs)
{
    QProcess process;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;

    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::errorOccurred, &loop,
                     [&process, &loop](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || process.state() == QProcess::NotRunning) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&process, &timedOut]() {
        timedOut = true;
        process.kill();
    });

    process.start(program, arguments);
    timeout.start(timeoutMs);
    loop.exec(QEventLoop::AllEvents);
    timeout.stop();
    return !timedOut && process.state() == QProcess::NotRunning &&
        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
class SettingsScrollArea final : public QScrollArea
{
public:
    explicit SettingsScrollArea(QWidget* parent = nullptr)
        : QScrollArea(parent)
    {}

    ~SettingsScrollArea() override
    {
        if (_grabWidget) {
            _grabWidget->releaseMouse();
            _grabWidget->unsetCursor();
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* watchedWidget = qobject_cast<QWidget*>(watched);
        const bool belongs = watchedWidget &&
            (watchedWidget == this || watchedWidget == viewport() || isAncestorOf(watchedWidget));
        if (!belongs || !event || !verticalScrollBar()) {
            return false;
        }

        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            const int pixelDelta = wheel->pixelDelta().y();
            const int angleDelta = wheel->angleDelta().y();
            int delta = pixelDelta != 0 ? pixelDelta : qRound(angleDelta * 0.5);
            if (delta == 0 && angleDelta != 0) delta = angleDelta > 0 ? 1 : -1;
            if (wheel->inverted()) delta = -delta;
            if (delta != 0) {
                verticalScrollBar()->setValue(verticalScrollBar()->value() - delta);
                wheel->accept();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton && !_dragging) {
                _dragging = true;
                _startGlobalY = mouse->globalPosition().toPoint().y();
                _startValue = verticalScrollBar()->value();
                _grabWidget = viewport();
                _grabWidget->grabMouse();
                _grabWidget->setCursor(Qt::ClosedHandCursor);
                mouse->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && _dragging) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const int deltaY = mouse->globalPosition().toPoint().y() - _startGlobalY;
            verticalScrollBar()->setValue(_startValue - deltaY);
            mouse->accept();
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease && _dragging) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                if (_grabWidget) {
                    _grabWidget->releaseMouse();
                    _grabWidget->unsetCursor();
                }
                _grabWidget.clear();
                _dragging = false;
                mouse->accept();
                return true;
            }
        }
        return false;
    }

private:
    QPointer<QWidget> _grabWidget;
    bool _dragging = false;
    int _startGlobalY = 0;
    int _startValue = 0;
};

QScrollArea* createSettingsScrollArea(QWidget* parent)
{
    return new SettingsScrollArea(parent);
}
void showHelpDocument(QWidget* parent, const QString& title, const QString& html)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(760, 680);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* browser = new QTextBrowser(&dialog);
    browser->setOpenExternalLinks(true);
    browser->setHtml(html);
    layout->addWidget(browser, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QString cgplayAboutHtml()
{
    const QString version = QCoreApplication::applicationVersion().isEmpty()
        ? QStringLiteral("1.0.7.12")
        : QCoreApplication::applicationVersion();
    return QStringLiteral(
        "<h2>CGPlay %1</h2>"
        "<p>面向 VFX、动画、合成与影视审片流程的专业播放器。</p>"
        "<p><b>核心能力</b></p>"
        "<ul>"
        "<li>帧精确播放、0.25x-8x 倍速、图像序列、视频与多音轨检查</li>"
        "<li>A/B 对比、批注、审片报告、完整视频与带批注视频导出</li>"
        "<li>字幕识别与翻译、当前画面识别、HDR/媒体信息分析</li>"
        "<li>LUT/OCIO 色彩管理与 ACES 工作流</li>"
        "<li>播放器内置 RVLite Codex 工作台，可调用无界面的 Codex CLI 运行核心</li>"
        "<li>Windows 资源管理器 QuickLook：选中文件后按 Space 快速预览</li>"
        "</ul>"
        "<p>完整版内置 Codex CLI、FFmpeg、Python、Qt WebEngine 与导出/AI 运行环境，"
        "无需另外安装 Codex 桌面应用。</p>"
        "<p><a href=\"https://cgplay-app.netlify.app/\">CGPlay 官方网站</a>　"
        "<a href=\"https://github.com/xty-luoye/CGPlay/releases\">备用发布页</a></p>")
        .arg(version.toHtmlEscaped());
}

bool isTextInputFocusWidget(QWidget* widget)
{
    while (widget) {
        if (qobject_cast<QLineEdit*>(widget) ||
            qobject_cast<QTextEdit*>(widget) ||
            qobject_cast<QPlainTextEdit*>(widget))
        {
            return true;
        }
        if (auto* comboBox = qobject_cast<QComboBox*>(widget)) {
            if (comboBox->isEditable()) {
                return true;
            }
        }
        widget = widget->parentWidget();
    }
    return false;
}
ReviewPanel* resolveReviewPanelWidget(QWidget* widget)
{
    if (!widget) {
        return nullptr;
    }
    if (auto* panel = qobject_cast<ReviewPanel*>(widget)) {
        return panel;
    }
    const QString className = QString::fromLatin1(widget->metaObject()->className());
    if (widget->objectName() == QStringLiteral("ReviewPanel") || className.endsWith(QStringLiteral("ReviewPanel"))) {
        return static_cast<ReviewPanel*>(widget);
    }
    return nullptr;
}

AnnotationToolbar* resolveAnnotationToolbarWidget(QWidget* widget)
{
    if (!widget) {
        return nullptr;
    }
    if (auto* toolbar = qobject_cast<AnnotationToolbar*>(widget)) {
        return toolbar;
    }
    const QString className = QString::fromLatin1(widget->metaObject()->className());
    if (widget->objectName() == QStringLiteral("AnnotationToolbar") || className.endsWith(QStringLiteral("AnnotationToolbar"))) {
        return static_cast<AnnotationToolbar*>(widget);
    }
    return nullptr;
}

bool isPluginFallbackDisabled()
{
    return qApp && qApp->property("cgplay.disablePluginFallback").toBool();
}

bool shouldAllowAnnotationFallback()
{
    // TODO(Phase13-remove): Transitional host fallback gate kept only for migration compatibility.
    return !isPluginFallbackDisabled();
}

bool hasAnnotationCapability(IAnnotationService* service, AnnotationManager* fallback)
{
    if (service) {
        return true;
    }
    return shouldAllowAnnotationFallback() && fallback;
}

IAnnotationService* resolveAnnotationService(IAnnotationService* service, AnnotationManager* fallback)
{
    if (service) {
        return service;
    }
    if (shouldAllowAnnotationFallback() && fallback) {
        return static_cast<IAnnotationService*>(fallback);
    }
    return nullptr;
}
QColor appColorProperty(const char* name, const QColor& fallback)
{
    if (!qApp) {
        return fallback;
    }
    const QColor value(qApp->property(name).toString());
    return value.isValid() ? value : fallback;
}

QString appStringProperty(const char* name, const QString& fallback)
{
    if (!qApp) {
        return fallback;
    }
    const QString value = qApp->property(name).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

QString safeWorkspaceName(QString name)
{
    name = QFileInfo(name.trimmed()).fileName();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    return name.isEmpty() ? QStringLiteral("\u9ed8\u8ba4\u5ba1\u7247") : name.left(80);
}

int appIntProperty(const char* name, int fallback)
{
    if (!qApp) {
        return fallback;
    }
    const QVariant value = qApp->property(name);
    return value.isValid() && value.canConvert<int>() ? value.toInt() : fallback;
}

QColor compositeOpaque(QColor foreground, const QColor& background)
{
    if (!foreground.isValid()) {
        return background.isValid() ? QColor(background.red(), background.green(), background.blue()) : QColor(kWindow);
    }
    const QColor under = background.isValid() ? background : QColor(kWindow);
    const qreal alpha = qBound(0.0, foreground.alphaF(), 1.0);
    if (alpha >= 0.999) {
        foreground.setAlpha(255);
        return foreground;
    }
    const auto blend = [alpha](int over, int underChannel) {
        return qBound(0, qRound(over * alpha + underChannel * (1.0 - alpha)), 255);
    };
    return QColor(blend(foreground.red(), under.red()),
                  blend(foreground.green(), under.green()),
                  blend(foreground.blue(), under.blue()),
                  255);
}

QColor adjustBackdropColor(QColor color, int brightness, int saturation)
{
    if (!color.isValid()) {
        return color;
    }
    const int alpha = color.alpha();
    QColor hsv = color.toHsv();
    const int hue = hsv.hue() < 0 ? 0 : hsv.hue();
    const int sat = qBound(0, qRound(hsv.saturation() * qBound(0, saturation, 200) / 100.0), 255);
    const int value = qBound(0, qRound(hsv.value() * qBound(0, brightness, 200) / 100.0), 255);
    hsv.setHsv(hue, sat, value, alpha);
    return hsv.toRgb();
}

QColor dialogSurfaceColor()
{
    const QColor fallback(kPanel);
    QColor surface = appColorProperty("cgplay.dialogColor");
    if (!surface.isValid()) {
        surface = appColorProperty("cgplay.panelColor");
    }
    if (!surface.isValid() && qApp) {
        surface = qApp->palette().color(QPalette::Base);
    }
    if (!surface.isValid()) {
        const QString mode = appStringProperty("cgplay.themeMode", QStringLiteral("dark")).toLower();
        if (mode == QStringLiteral("light")) {
            surface = QColor(QStringLiteral("#FFFFFF"));
        } else if (mode == QStringLiteral("highcontrast")) {
            surface = QColor(QStringLiteral("#101010"));
        } else {
            surface = fallback;
        }
    }
    return compositeOpaque(surface, QColor(kWindow));
}

QColor viewerSurfaceColor()
{
    QColor surface = appColorProperty("cgplay.viewerColor");
    if (!surface.isValid()) {
        surface = QColor(QStringLiteral("#050505"));
    }
    return compositeOpaque(surface, QColor(QStringLiteral("#050505")));
}

void drawBackdropPixmap(QPainter& painter, const QPixmap& pixmap, const QRect& target, const QString& fillMode)
{
    if (pixmap.isNull() || target.isEmpty()) {
        return;
    }
    const QString mode = fillMode.trimmed().toLower();
    if (mode == QStringLiteral("tile")) {
        painter.drawTiledPixmap(target, pixmap);
        return;
    }
    const Qt::AspectRatioMode aspect = mode == QStringLiteral("contain")
        ? Qt::KeepAspectRatio
        : Qt::KeepAspectRatioByExpanding;
    const QPixmap scaled = pixmap.scaled(target.size(), aspect, Qt::SmoothTransformation);
    const QPoint topLeft(target.center().x() - scaled.width() / 2,
                         target.center().y() - scaled.height() / 2);
    painter.drawPixmap(topLeft, scaled);
}

void drawBackdropPixmapAligned(QPainter& painter, const QPixmap& pixmap, const QWidget* surface, const QString& fillMode)
{
    if (!surface || !surface->window()) return;
    const QWidget* root = surface->window();
    const QPoint offset = surface->mapTo(root, QPoint(0, 0));
    painter.save();
    painter.translate(-offset);
    drawBackdropPixmap(painter, pixmap, QRect(QPoint(0, 0), root->size()), fillMode);
    painter.restore();
}

class ThemedBackdrop : public QWidget
{
public:
    explicit ThemedBackdrop(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("cgplayAppearanceHost"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        _dynamicTimer = new QTimer(this);
        _dynamicTimer->setInterval(160);
        connect(_dynamicTimer, &QTimer::timeout, this, [this] {
            if (qApp && qApp->property("cgplay.dynamicBackground").toBool()) update();
        });
        _dynamicTimer->start();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor underlay(kWindow);
        const QColor configured = appColorProperty("cgplay.backgroundColor");
        const bool useConfigured = qApp && qApp->property("cgplay.useConfiguredBackdrop").toBool() && configured.isValid();
        QColor primary = useConfigured ? configured : QColor(0x11, 0x18, 0x20);
        const int brightness = qBound(0, appIntProperty("cgplay.backgroundBrightness", 100), 200);
        const int saturation = qBound(0, appIntProperty("cgplay.backgroundSaturation", 100), 200);
        const int blurRadius = qBound(0, appIntProperty("cgplay.backgroundBlurRadius", 0), 64);
        const bool dynamicBackground = qApp && qApp->property("cgplay.dynamicBackground").toBool();
        const int dynamicBoost = dynamicBackground
            ? qRound((std::sin(QDateTime::currentMSecsSinceEpoch() / 1100.0) + 1.0) * 3.0)
            : 0;
        primary = adjustBackdropColor(primary, brightness, saturation);
        int configuredOpacity = appIntProperty("cgplay.backgroundOpacity", -1);
        if (configuredOpacity >= 0 && configuredOpacity <= 100) {
            primary.setAlphaF(primary.alphaF() * configuredOpacity / 100.0);
        }
        primary = compositeOpaque(primary, underlay);

        QColor secondary = appColorProperty("cgplay.backgroundSecondary");
        if (!secondary.isValid()) {
            secondary = primary.darker(useConfigured ? 112 : 118);
        }
        secondary = adjustBackdropColor(secondary, brightness, saturation);
        if (configuredOpacity >= 0 && configuredOpacity <= 100) {
            secondary.setAlphaF(secondary.alphaF() * configuredOpacity / 100.0);
        }
        secondary = compositeOpaque(secondary, underlay);

        const QString backgroundType = appStringProperty("cgplay.backgroundType", QStringLiteral("gradient")).toLower();
        const QString configuredImagePath = appStringProperty("cgplay.backgroundImage");
        const QString imagePath = normalizedBackdropPath(configuredImagePath);
        bool drewImage = false;
        if ((backgroundType == QStringLiteral("image") || backgroundType == QStringLiteral("texture")) && !imagePath.isEmpty()) {
            if (imagePath != _cachedImagePath) {
                _cachedImagePath = imagePath;
                _cachedSourcePixmap = QPixmap(imagePath);
                _cachedPreparedPixmap = {};
                _cachedBrightness = -1;
                _cachedSaturation = -1;
                _cachedBlurRadius = -1;
            }
            if (_cachedSourcePixmap.isNull() && imagePath != configuredImagePath) {
                // Keep a final fallback for unusual Windows paths which Qt's
                // URL/path parser may normalize differently.
                _cachedSourcePixmap = QPixmap(configuredImagePath);
            }
            if (_cachedSourcePixmap.isNull() && QFileInfo::exists(imagePath)) {
                // A profile can be restored before its media mount is ready;
                // retry once the file becomes available without requiring a
                // second theme refresh.
                _cachedSourcePixmap = QPixmap(imagePath);
            }
            const QString fillMode = appStringProperty(
                "cgplay.backgroundFillMode",
                appStringProperty("cgplay.fillMode", QStringLiteral("cover")));
            if (!_cachedSourcePixmap.isNull()) {
                _cachedPreparedPixmap = preparedBackdropPixmap(
                    imagePath, brightness, saturation, blurRadius,
                    window() ? window()->size() : size(), fillMode);
                _cachedBrightness = brightness;
                _cachedSaturation = saturation;
                _cachedBlurRadius = blurRadius;
            }
            const QPixmap& pixmap = _cachedPreparedPixmap;
            if (qApp) qApp->setProperty("cgplay.backgroundImageValid", !pixmap.isNull());
            if (!pixmap.isNull()) {
                // WA_OpaquePaintEvent means the backing store is not cleared for us.
                // Paint an opaque underlay before drawing a possibly translucent image.
                painter.fillRect(rect(), underlay);
                painter.save();
                painter.setOpacity(configuredOpacity >= 0 && configuredOpacity <= 100 ? configuredOpacity / 100.0 : 1.0);
                drawBackdropPixmapAligned(painter, pixmap, this, fillMode);
                painter.restore();
                drewImage = true;
                // Keep text surfaces readable over bright local artwork.
                painter.fillRect(rect(), QColor(0, 0, 0, 28));
            }
        }
        if (qApp && !drewImage) qApp->setProperty("cgplay.backgroundImageValid", false);
        if (!drewImage) {
            if (backgroundType == QStringLiteral("solid")) {
                painter.fillRect(rect(), primary);
            } else {
                QLinearGradient base(0, 0, 0, height());
                base.setColorAt(0.0, primary.lighter(useConfigured ? 108 : 100));
                base.setColorAt(0.32, primary);
                base.setColorAt(0.76, secondary);
                base.setColorAt(1.0, secondary.darker(106));
                painter.fillRect(rect(), base);
            }
        }

        if (backgroundType != QStringLiteral("solid") || drewImage) {
            const QColor wash = primary.lighter(130);
            QLinearGradient topWash(0, 0, 0, height() * 0.42);
            topWash.setColorAt(0.0, QColor(wash.red(), wash.green(), wash.blue(), qBound(0, (useConfigured ? 34 : 48) + dynamicBoost, 255)));
            topWash.setColorAt(0.45, QColor(wash.red(), wash.green(), wash.blue(), qBound(0, (useConfigured ? 14 : 22) + dynamicBoost / 2, 255)));
            topWash.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), topWash);
        }

        painter.setPen(Qt::NoPen);
        for (int y = 0; y < height(); y += 6) {
            for (int x = 0; x < width(); x += 6) {
                const quint32 seed = static_cast<quint32>((x + 11) * 73856093u ^ (y + 17) * 19349663u);
                const int alpha = useConfigured ? 1 + static_cast<int>(seed % 3u) : 2 + static_cast<int>(seed % 5u);
                painter.setBrush(QColor(255, 255, 255, alpha));
                painter.drawRect(x, y, 1, 1);
            }
        }

        const int vignetteStrength = qBound(0, appIntProperty("cgplay.backgroundVignette", useConfigured ? 12 : 36), 100);
        if (vignetteStrength > 0) {
            const int alpha = qBound(0, qRound(vignetteStrength * 0.9), 100);
            QLinearGradient vignette(0, 0, width(), 0);
            vignette.setColorAt(0.0, QColor(0, 0, 0, alpha));
            vignette.setColorAt(0.18, QColor(0, 0, 0, 0));
            vignette.setColorAt(0.82, QColor(0, 0, 0, 0));
            vignette.setColorAt(1.0, QColor(0, 0, 0, alpha));
            painter.fillRect(rect(), vignette);
        }
    }

private:
    QTimer* _dynamicTimer = nullptr;
    QString _cachedImagePath;
    QPixmap _cachedSourcePixmap;
    QPixmap _cachedPreparedPixmap;
    int _cachedBrightness = -1;
    int _cachedSaturation = -1;
    int _cachedBlurRadius = -1;
};

class ThemedDockWidget final : public QDockWidget
{
public:
    using QDockWidget::QDockWidget;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QDockWidget::paintEvent(event);
        QPainter painter(this);
        BackdropRenderOptions options;
        options.underlay = appColorProperty("cgplay.backgroundColor").isValid()
            ? appColorProperty("cgplay.backgroundColor") : QColor(kWindow);
        options.readabilityWashAlpha = 28;
        drawApplicationBackdrop(painter, rect(), this, options);
    }
};

class ThemedDialog : public QDialog
{
public:
    using QDialog::QDialog;

protected:
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override
    {
        Q_UNUSED(eventType);
        auto* msg = static_cast<MSG*>(message);
        if (!msg || !result) return QDialog::nativeEvent(eventType, message, result);

        const auto scrollAt = [this](const QPoint& globalPos) -> QScrollArea* {
            for (QScrollArea* scroll : findChildren<QScrollArea*>()) {
                if (!scroll || !scroll->isVisible() || !scroll->verticalScrollBar()) continue;
                const QRect globalRect(scroll->mapToGlobal(QPoint(0, 0)), scroll->size());
                if (globalRect.contains(globalPos)) return scroll;
            }
            return nullptr;
        };

        if (msg->message == WM_MOUSEWHEEL) {
            const QPoint globalPos(
                static_cast<short>(LOWORD(msg->lParam)),
                static_cast<short>(HIWORD(msg->lParam)));
            if (QScrollArea* scroll = scrollAt(globalPos)) {
                const int wheelDelta = static_cast<short>(HIWORD(msg->wParam));
                int delta = qRound(wheelDelta * 0.5);
                if (delta == 0 && wheelDelta != 0) delta = wheelDelta > 0 ? 1 : -1;
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() - delta);
                *result = 0;
                return true;
            }
        } else if (msg->message == WM_MBUTTONDOWN && !_nativeScrollDragging) {
            const QPoint globalPos = mapToGlobal(QPoint(
                static_cast<short>(LOWORD(msg->lParam)),
                static_cast<short>(HIWORD(msg->lParam))));
            if (QScrollArea* scroll = scrollAt(globalPos)) {
                _nativeScrollDragging = true;
                _nativeScrollArea = scroll;
                _nativeScrollStartGlobalY = globalPos.y();
                _nativeScrollStartValue = scroll->verticalScrollBar()->value();
                SetCapture(reinterpret_cast<HWND>(winId()));
                setCursor(Qt::ClosedHandCursor);
                *result = 0;
                return true;
            }
        } else if (msg->message == WM_MOUSEMOVE && _nativeScrollDragging) {
            const QPoint globalPos = mapToGlobal(QPoint(
                static_cast<short>(LOWORD(msg->lParam)),
                static_cast<short>(HIWORD(msg->lParam))));
            if (_nativeScrollArea && _nativeScrollArea->verticalScrollBar()) {
                _nativeScrollArea->verticalScrollBar()->setValue(
                    _nativeScrollStartValue - (globalPos.y() - _nativeScrollStartGlobalY));
            }
            *result = 0;
            return true;
        } else if (msg->message == WM_MBUTTONUP && _nativeScrollDragging) {
            ReleaseCapture();
            unsetCursor();
            _nativeScrollDragging = false;
            _nativeScrollArea.clear();
            *result = 0;
            return true;
        }
        return QDialog::nativeEvent(eventType, message, result);
    }
#endif

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect(), dialogSurfaceColor());
        QDialog::paintEvent(event);
    }

private:
#ifdef Q_OS_WIN
    QPointer<QScrollArea> _nativeScrollArea;
    bool _nativeScrollDragging = false;
    int _nativeScrollStartGlobalY = 0;
    int _nativeScrollStartValue = 0;
#endif
};

QWidget* createThemedBackdrop(QWidget* parent)
{
    return new ThemedBackdrop(parent);
}

QDockWidget* createThemedDockWidget(const QString& title, QWidget* parent)
{
    return new ThemedDockWidget(title, parent);
}

QDialog* createThemedDialog(QWidget* parent)
{
    return new ThemedDialog(parent);
}

} // namespace cgplay
