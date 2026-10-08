#include "CodexAgentWorkspace.h"
#include "common/theme/BackdropRenderer.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QAbstractItemView>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageReader>
#include <QInputDialog>
#include <QLineEdit>
#include <QPalette>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTextDocument>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineDownloadRequest>
#include <QStandardPaths>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>

#include <utility>

namespace cgplay {
namespace {

constexpr auto kReady = "#48D597";
constexpr auto kWarning = "#F2A45F";
constexpr auto kError = "#FF6B6B";

QColor mixColors(const QColor& base, const QColor& tint, qreal tintAmount)
{
    const qreal amount = qBound(0.0, tintAmount, 1.0);
    const auto blend = [amount](int baseChannel, int tintChannel) {
        return qRound(baseChannel * (1.0 - amount) + tintChannel * amount);
    };
    return QColor(
        blend(base.red(), tint.red()),
        blend(base.green(), tint.green()),
        blend(base.blue(), tint.blue()));
}

QString colorName(const QColor& color)
{
    return color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb).toUpper();
}

QColor appThemeColor(const char* propertyName, const QColor& fallback)
{
    if (qApp) {
        const QColor configured(qApp->property(propertyName).toString());
        if (configured.isValid()) return configured;
    }
    return fallback;
}

QString rgbaColor(const QColor& color, qreal alpha)
{
    const QColor opaque = color.isValid() ? color : QColor(Qt::black);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(opaque.red())
        .arg(opaque.green())
        .arg(opaque.blue())
        .arg(qBound(0.0, alpha, 1.0), 0, 'f', 3);
}

QString contrastingText(const QColor& color)
{
    const QColor c = color.isValid() ? color : QColor(Qt::black);
    const int luminance = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
    return luminance >= 150 ? QStringLiteral("#111111") : QStringLiteral("#FFFFFF");
}

struct WorkspaceTheme
{
    QString window;
    QString surface;
    QString raised;
    QString hover;
    QString border;
    QString text;
    QString muted;
    QString accent;
    QString accentText;
    QString input;
    QString error;
    QString accentSoft;
    QString accentHoverSoft;
    QString accentPressedSoft;
};

WorkspaceTheme currentWorkspaceTheme()
{
    const QPalette palette = QApplication::palette();
    QColor panel = appThemeColor("cgplay.panelColor", palette.color(QPalette::Base));
    QColor toolbar = appThemeColor("cgplay.toolbarColor", palette.color(QPalette::Button));
    QColor timeline = appThemeColor("cgplay.timelineColor", palette.color(QPalette::AlternateBase));
    const int panelOpacity = qApp ? qBound(0, qApp->property("cgplay.panelOpacity").toInt(), 100) : 96;
    const int toolbarOpacity = qApp ? qBound(0, qApp->property("cgplay.toolbarOpacity").toInt(), 100) : 92;
    const int timelineOpacity = qApp ? qBound(0, qApp->property("cgplay.timelineOpacity").toInt(), 100) : 96;
    panel.setAlpha(qRound(panelOpacity * 255.0 / 100.0));
    toolbar.setAlpha(qRound(toolbarOpacity * 255.0 / 100.0));
    timeline.setAlpha(qRound(timelineOpacity * 255.0 / 100.0));
    const QColor text = appThemeColor("cgplay.textColor", palette.color(QPalette::Text));
    const QColor border = appThemeColor("cgplay.borderColor", palette.color(QPalette::Mid));
    const QColor accent = appThemeColor("cgplay.accentColor", palette.color(QPalette::Highlight));
    const QColor raised = toolbar.isValid() ? toolbar : mixColors(panel, text, 0.055);
    const QColor hover = mixColors(raised, text, 0.12);
    const QColor muted = mixColors(text, panel, 0.28);
    return {
        colorName(panel),
        colorName(panel),
        colorName(raised),
        colorName(hover),
        colorName(border),
        colorName(text),
        colorName(muted),
        colorName(accent),
        contrastingText(accent),
        colorName(timeline),
        QString::fromLatin1(kError),
        rgbaColor(accent, 0.12),
        rgbaColor(accent, 0.22),
        rgbaColor(accent, 0.34)
    };
}

QString codexThemeOverride(const WorkspaceTheme& theme)
{
    QString css = QStringLiteral(
        "#CodexAgentWorkspace{background:transparent;color:%6;}"
        "#CodexContent{background:transparent;color:%6;}"
        "#CodexRail{background:%2;border-right:1px solid %5;}"
        "#CodexHeader{background:%3;border-bottom:1px solid %5;border-top:1px solid %11;}"
        "#CodexMark{background:%4;border:1px solid %5;color:%6;}"
        "#CodexSourceSummary{background:transparent;border-bottom:1px solid %5;color:%7;}"
        "#CodexConversation{background:transparent;border:0;}"
        "#CodexConversationViewport{background:transparent;border:0;}"
        "#CodexConversationHost{background:transparent;}"
        "#CodexComposerWrap{background:transparent;border-top:1px solid %5;}"
        "#CodexComposer{background:%3;border:1px solid %5;border-top:2px solid %11;}"
        "#CodexPrompt{background:%10;color:%6;border-color:%5;}"
        "#CodexDiagnostics{background:transparent;border-top:1px solid %5;}"
        "#CodexLog{background:%2;color:%7;border-color:%5;}"
        "#CodexUserBubble{background:%3;border-color:%5;border-left-color:%8;}"
        "#CodexUserImage,#CodexGeneratedImage,#CodexFileArtifact,#CodexTaskCard{background:%3;border-color:%5;}"
        "#CodexImageImport,#CodexFileOpen,#CodexFileSaveAs{background:%4;border-color:%5;color:%6;}"
        "#CodexImageSave{background:%3;border-color:%5;color:%6;}"
        "#CodexAvatar{background:%4;border-color:%5;color:%6;}"
        "#CodexRetry{color:%8;border-color:%11;background:%11;}"
        "#CodexProject,#CodexNewConversation,#CodexApprovalMode,#CodexReasoning{background:%3;border:1px solid %5;color:%6;}"
        "#CodexApprovalMode{color:%6;border-color:%5;background:%3;}"
        "#CodexReasoning{color:%6;}"
        "#CodexSend{background:%3;color:%6;border:1px solid %5;min-width:31px;max-width:31px;min-height:31px;max-height:31px;}"
        "#CodexSend:hover{background:%4;color:%6;}"
        "#CodexSend:pressed{background:%8;color:%9;}"
        "#CodexSend:disabled{background:%3;color:%7;border:1px solid %5;}"
        "QToolButton{color:%6;}"
        "#CodexHeader QToolButton{color:%6;}"
        "#CodexHeaderNewConversation,#CodexSessions,#CodexOverflow,#CodexClose{background:%3;color:%6;border:1px solid %5;border-radius:6px;padding:4px;}"
        "#CodexHeader QToolButton:hover{background:%4;border-color:%5;color:%6;}"
        "QToolButton:hover{background:%4;border-color:%5;color:%6;}"
        "QToolButton:disabled{color:%7;background:transparent;border-color:transparent;}"
        "#CodexHeaderNewConversation:disabled,#CodexSessions:disabled,#CodexOverflow:disabled,#CodexClose:disabled,#CodexProject:disabled,#CodexNewConversation:disabled,#CodexApprovalMode:disabled,#CodexReasoning:disabled,#CodexSend:disabled{background:%3;color:%6;border:1px solid %5;}"
        "QComboBox{background:%3;color:%6;border-color:%5;}"
        "QComboBox QAbstractItemView{background:%2;color:%6;border-color:%5;selection-background-color:%4;selection-color:%6;}"
        "QComboBox#CodexModel QAbstractItemView{background:%2;color:%6;selection-background-color:%4;selection-color:%6;}"
        "QMenu{background:%3;color:%6;border-color:%5;}"
        "QMenu::item:selected{background:%4;color:%6;}"
        "QScrollBar::handle:vertical{background:%5;}"
    ).arg(theme.window, theme.surface, theme.raised, theme.hover, theme.border, theme.text,
          theme.muted, theme.accent, theme.accentText, theme.input,
          theme.accentSoft, theme.accentHoverSoft, theme.accentPressedSoft);
    return css;
}

QString chooseSavePath(QWidget* parent, const QString& title, const QString& suggestedPath, const QString& filter)
{
    QWidget* owner = parent ? parent->window() : nullptr;
    if (owner) {
        owner->raise();
        owner->activateWindow();
    }

    const QFileInfo suggested(suggestedPath);
    QFileDialog dialog(owner, title, suggested.absolutePath(), filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.selectFile(suggested.fileName());
    QTimer::singleShot(0, &dialog, [&dialog] {
        dialog.raise();
        dialog.activateWindow();
    });
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    return QFileInfo(dialog.selectedFiles().constFirst()).absoluteFilePath();
}

void configureBoundedTextView(QPlainTextEdit* view, int maximumBlocks)
{
    view->setReadOnly(true);
    view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view->document()->setMaximumBlockCount(maximumBlocks);
}

QString displayModelName(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    if (normalized.contains(QStringLiteral("5.6-sol"))) return QStringLiteral("5.6 Sol");
    if (normalized.contains(QStringLiteral("5.6-terra"))) return QStringLiteral("5.6 Terra");
    if (normalized.contains(QStringLiteral("5.6-luna"))) return QStringLiteral("5.6 Luna");
    if (normalized == QStringLiteral("gpt-5.5") || normalized == QStringLiteral("5.5")) return QStringLiteral("5.5");
    if (normalized == QStringLiteral("gpt-5.4") || normalized == QStringLiteral("5.4")) return QStringLiteral("5.4");
    return model.trimmed();
}

QString compactSourceStatus(QString status)
{
    status.replace(QRegularExpression(QStringLiteral("\\s*\\n\\s*")), QStringLiteral(" · "));
    status.replace(QStringLiteral("服务商："), QStringLiteral(""));
    status.replace(QStringLiteral("API："), QStringLiteral(""));
    status.replace(QStringLiteral("模型："), QStringLiteral(""));
    status.replace(QStringLiteral("密钥：安全凭据（不显示）"), QStringLiteral("密钥安全托管"));
    return status.simplified();
}

QString turnStatusText(const QString& status)
{
    if (status == QStringLiteral("completed")) return QObject::tr("已完成");
    if (status == QStringLiteral("failed")) return QObject::tr("失败");
    if (status == QStringLiteral("interrupted")) return QObject::tr("已中断");
    return QObject::tr("已结束");
}

} // namespace

CodexAgentWorkspace::CodexAgentWorkspace(QWidget* parent)
    : QWidget(parent)
{
    const QPalette hostPalette = QApplication::palette();
    const QColor windowColor = hostPalette.color(QPalette::Window);
    const QColor surfaceColor = hostPalette.color(QPalette::Base);
    const QColor textColor = hostPalette.color(QPalette::Text);
    const QString window = colorName(windowColor);
    const QString surface = colorName(surfaceColor);
    const QString raised = colorName(mixColors(surfaceColor, textColor, 0.055));
    const QString hover = colorName(mixColors(surfaceColor, textColor, 0.095));
    const QString border = colorName(mixColors(surfaceColor, textColor, 0.13));
    const QString text = colorName(textColor);
    const QString muted = colorName(hostPalette.color(QPalette::Disabled, QPalette::Text));
    const QString accent = colorName(hostPalette.color(QPalette::Highlight));

    setObjectName(QStringLiteral("CodexAgentWorkspace"));
    setMinimumWidth(420);
    setStyleSheet(QStringLiteral(
        "#CodexAgentWorkspace{background:%1;color:%6;}"
        "#CodexRail{background:%2;border-right:1px solid %5;}"
        "#CodexRail QToolButton{background:transparent;border:1px solid transparent;border-radius:5px;color:%6;font-size:15px;padding:4px;}"
        "#CodexRail QToolButton:hover{background:%4;border-color:%5;color:%6;}"
        "#CodexRail QToolButton:checked{background:%4;border-color:%8;color:%8;}"
        "#CodexRail QLabel{color:%7;font-size:9px;font-weight:600;}"
        "QLabel{color:%6;}"
        "#CodexHeader{background:%3;border-bottom:1px solid %5;border-top:1px solid rgba(255,138,61,0.22);}" 
        "#CodexMark{background:%4;border:1px solid %5;border-radius:6px;color:%6;font-size:13px;font-weight:700;}"
        "#CodexTitle{font-size:14px;font-weight:600;color:%6;}"
        "#CodexConnectionStatus{font-size:10px;}"
        "#CodexSourceSummary{background:%1;border-bottom:1px solid %5;color:%7;padding:3px 14px;font-size:9px;}"
        "#CodexConversation{background:%2;border:0;}"
        "#CodexConversationHost{background:%2;}"
        "#CodexEmpty{color:%6;font-size:12px;padding:24px;}"
        "#CodexUserBubble{background:%3;border:1px solid %5;border-left:2px solid %8;border-radius:5px;}"
        "#CodexUserText{color:%6;font-size:13px;padding:10px 12px;}"
        "#CodexUserImage{background:%1;border:1px solid %5;border-radius:4px;padding:2px;}"
        "#CodexAvatar{background:%4;border:1px solid %5;border-radius:5px;color:%6;font-size:10px;font-weight:700;}"
        "#CodexWho{color:%6;font-size:10px;}"
        "#CodexAssistantText{color:%6;font-size:13px;}"
        "#CodexGeneratedImage{background:%1;border:1px solid %5;border-radius:6px;padding:3px;}"
        "#CodexImageCaption{color:%7;font-size:10px;}"
        "#CodexImageImport{background:%4;border:1px solid %5;color:%6;padding:5px 9px;}"
        "#CodexImageSave{background:%3;border:1px solid %5;color:%6;padding:5px 9px;}"
        "#CodexFileArtifact{background:%3;border:1px solid %5;border-radius:6px;}"
        "#CodexFileName{color:%6;font-size:12px;font-weight:600;}"
        "#CodexFileMeta{color:%7;font-size:9px;}"
        "#CodexFileOpen,#CodexFileSaveAs{background:%4;border:1px solid %5;color:%6;padding:5px 8px;}"
        "#CodexTaskCard{background:%3;border:1px solid %5;border-radius:7px;}"
        "#CodexTaskHeader{border:0;border-bottom:1px solid %5;border-radius:0;color:%6;padding:7px 9px;text-align:left;font-size:11px;}"
        "#CodexTaskHeader:hover{background:%4;}"
        "#CodexTaskStatus{color:%6;font-size:10px;padding-right:8px;}"
        "#CodexTaskDetails{background:transparent;}"
        "#CodexTaskStep{color:%6;font-size:10px;padding:3px 9px;}"
        "#CodexError{color:%6;background:%3;border:1px solid %5;border-left:2px solid %8;border-radius:6px;padding:7px 9px;}"
        "#CodexDiagnostics{background:%1;border-top:1px solid %5;}"
        "#CodexRuntimeSource,#CodexAiWorkspaceSource{color:%7;font-size:10px;}"
        "#CodexLog{background:%2;color:%7;border:1px solid %5;border-radius:6px;padding:7px;font-family:Consolas,monospace;font-size:10px;}"
        "#CodexComposerWrap{background:%1;border-top:1px solid %5;padding-top:1px;}"
        "#CodexComposer{background:%3;border:1px solid %5;border-top:2px solid rgba(255,138,61,0.34);border-radius:6px;}"
        "#CodexPrompt{background:%4;color:%6;border:0;border-radius:5px;padding:9px 10px;font-size:13px;}"
        "QToolButton{border:1px solid transparent;border-radius:6px;color:%6;padding:4px;}"
        "#CodexHeader QToolButton{color:%6;}"
        "#CodexHeaderNewConversation,#CodexSessions,#CodexOverflow,#CodexClose{background:%3;color:%6;border:1px solid %5;border-radius:6px;padding:4px;}"
        "#CodexHeader QToolButton:hover{background:%4;border-color:%5;color:%6;}"
        "QToolButton:hover{background:%4;border-color:%5;color:%6;}"
        "QToolButton:disabled{color:#5E6874;background:transparent;border-color:transparent;}"
        "#CodexHeaderNewConversation:disabled,#CodexSessions:disabled,#CodexOverflow:disabled,#CodexClose:disabled,#CodexProject:disabled,#CodexNewConversation:disabled,#CodexApprovalMode:disabled,#CodexReasoning:disabled,#CodexSend:disabled{background:%3;color:%6;border:1px solid %5;}"
        "#CodexRetry{color:%8;border-color:rgba(255,138,61,0.45);background:rgba(255,138,61,0.10);padding:4px 8px;}"
        "#CodexProject,#CodexNewConversation,#CodexApprovalMode,#CodexReasoning{background:%3;border:1px solid %5;color:%6;padding:4px 7px;}"
        "#CodexApprovalMode{color:%6;border-color:%5;background:%3;}"
        "#CodexReasoning{color:%6;}"
        "#CodexSend{background:%3;color:%6;border:1px solid %5;border-radius:7px;padding:5px;min-width:31px;max-width:31px;min-height:31px;max-height:31px;}"
        "#CodexSend:hover{background:%4;color:%6;}"
        "#CodexSend:disabled{background:%3;color:%7;border:1px solid %5;}"
        "QComboBox{background:%3;color:%6;border:1px solid %5;border-radius:6px;padding:4px 22px 4px 8px;min-height:19px;}"
        "QComboBox:hover{border-color:rgba(255,138,61,0.58);}"
        "QComboBox QAbstractItemView{background:%2;color:%6;border:1px solid %5;selection-background-color:%4;selection-color:%6;}"
        "QComboBox#CodexModel QAbstractItemView{background:%2;color:%6;border:1px solid %5;selection-background-color:%4;selection-color:%6;}"
        "QComboBox#CodexModel:disabled{background:%3;color:%6;border:1px solid %5;}"
        "QMenu{background:%3;color:%6;border:1px solid %5;border-radius:7px;padding:6px;}"
        "QMenu::item{padding:8px 26px 8px 10px;border-radius:5px;}"
        "QMenu::item:selected{background:%4;}"
        "QMenu::item:checked{color:%8;}"
        "QMenu::separator{height:1px;background:%5;margin:4px 8px;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:2px;}"
        "QScrollBar::handle:vertical{background:%5;border-radius:4px;min-height:24px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
    ).arg(window, surface, raised, hover, border, text, muted, accent, QString::fromLatin1(kError)));
    // Keep the constructor stylesheet as a stable base.  Runtime refreshes
    // append theme-owned surface rules without accumulating old overrides.
    setProperty("cgplay.baseCodexStyleSheet", styleSheet());

    auto* shellLayout = new QHBoxLayout(this);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    auto* rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("CodexRail"));
    rail->setFixedWidth(68);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(8, 10, 8, 10);
    railLayout->setSpacing(8);
    auto railButton = [rail](const QString& glyph, const QString& tip) {
        auto* button = new QToolButton(rail);
        button->setText(glyph);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setFixedSize(36, 34);
        return button;
    };
    auto* railNew = railButton(QStringLiteral("+"), tr("新建会话"));
    railLayout->addWidget(railNew);
    auto* railSessions = railButton(QStringLiteral("≡"), tr("会话列表"));
    railSessions->setChecked(true);
    railLayout->addWidget(railSessions);
    auto* railCurrent = railButton(QStringLiteral("›"), tr("当前会话"));
    railCurrent->setEnabled(false);
    railLayout->addWidget(railCurrent);
    auto* railArchive = railButton(QStringLiteral("□"), tr("归档会话"));
    railLayout->addWidget(railArchive);
    railLayout->addStretch(1);
    auto* railBrand = new QLabel(QStringLiteral("CG\nAI"), rail);
    railBrand->setAlignment(Qt::AlignCenter);
    railBrand->setToolTip(tr("CGPlay Codex 工作台"));
    railLayout->addWidget(railBrand);
    // The top header already exposes session/new/overflow actions; keep the
    // legacy rail out of the visual layout so the conversation gets the width.
    rail->setVisible(false);
    shellLayout->addWidget(rail);

    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("CodexContent"));
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    shellLayout->addWidget(content, 1);

    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("CodexHeader"));
    header->setFixedHeight(52);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(17, 0, 10, 0);
    headerLayout->setSpacing(7);

    auto* mark = new QLabel(QStringLiteral("C"), header);
    mark->setObjectName(QStringLiteral("CodexMark"));
    mark->setAlignment(Qt::AlignCenter);
    mark->setFixedSize(28, 28);
    headerLayout->addWidget(mark);

    auto* titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(1);
    _titleLabel = new QLabel(tr("RVLite 工作台"), header);
    _titleLabel->setObjectName(QStringLiteral("CodexTitle"));
    _titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleBlock->addWidget(_titleLabel);
    _statusLabel = new QLabel(header);
    _statusLabel->setObjectName(QStringLiteral("CodexConnectionStatus"));
    titleBlock->addWidget(_statusLabel);
    headerLayout->addLayout(titleBlock, 1);

    _retryButton = new QToolButton(header);
    _retryButton->setObjectName(QStringLiteral("CodexRetry"));
    _retryButton->setText(tr("重试"));
    _retryButton->setToolTip(tr("重新连接 Codex"));
    _retryButton->hide();
    headerLayout->addWidget(_retryButton);

    _diagnosticsButton = new QToolButton(header);
    _diagnosticsButton->setObjectName(QStringLiteral("CodexDiagnosticsButton"));
    _diagnosticsButton->setText(QStringLiteral(">_"));
    _diagnosticsButton->setToolTip(tr("运行诊断"));
    _diagnosticsButton->setFixedSize(30, 30);
    _diagnosticsButton->hide();
    headerLayout->addWidget(_diagnosticsButton);

    _headerNewConversationButton = new QToolButton(header);
    _headerNewConversationButton->setObjectName(QStringLiteral("CodexHeaderNewConversation"));
    _headerNewConversationButton->setText(QStringLiteral("+"));
    _headerNewConversationButton->setToolTip(tr("新会话"));
    _headerNewConversationButton->setFixedSize(30, 30);
    headerLayout->addWidget(_headerNewConversationButton);

    _sessionsButton = new QToolButton(header);
    _sessionsButton->setObjectName(QStringLiteral("CodexSessions"));
    _sessionsButton->setText(QStringLiteral("≡"));
    _sessionsButton->setToolTip(tr("会话"));
    _sessionsButton->setFixedSize(30, 30);
    _sessionsButton->setPopupMode(QToolButton::InstantPopup);
    _sessionsMenu = new QMenu(_sessionsButton);
    _sessionsButton->setMenu(_sessionsMenu);
    headerLayout->addWidget(_sessionsButton);

    _overflowButton = new QToolButton(header);
    _overflowButton->setObjectName(QStringLiteral("CodexOverflow"));
    _overflowButton->setText(QStringLiteral("..."));
    _overflowButton->setToolTip(tr("更多"));
    _overflowButton->setFixedSize(30, 30);
    _overflowButton->setPopupMode(QToolButton::InstantPopup);
    headerLayout->addWidget(_overflowButton);

    _closeButton = new QToolButton(header);
    _closeButton->setObjectName(QStringLiteral("CodexClose"));
    _closeButton->setText(QStringLiteral("×"));
    _closeButton->setToolTip(tr("关闭工作台"));
    _closeButton->setFixedSize(30, 30);
    headerLayout->addWidget(_closeButton);
    rootLayout->addWidget(header);

    _overflowMenu = new QMenu(_overflowButton);
    _stopAction = _overflowMenu->addAction(tr("停止 Codex"));
    _stopAction->setEnabled(false);
    _overflowMenu->addSeparator();
    _workbenchAction = _overflowMenu->addAction(tr("打开 Codex 控制中心"));
    _overflowButton->setMenu(_overflowMenu);

    _sourceSummaryLabel = new QLabel(tr("正在读取 AI 工作台配置..."), this);
    _sourceSummaryLabel->setObjectName(QStringLiteral("CodexSourceSummary"));
    _sourceSummaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    _sourceSummaryLabel->setFixedHeight(22);
    rootLayout->addWidget(_sourceSummaryLabel);
    _sourceSummaryLabel->hide();

    _imageCapabilityLabel = new QLabel(tr("图片生成能力：正在探测"), this);
    _imageCapabilityLabel->setObjectName(QStringLiteral("CodexImageCapability"));
    _imageCapabilityLabel->setStyleSheet(QStringLiteral("padding:3px 18px;font-size:10px;color:%1;").arg(muted));
    rootLayout->addWidget(_imageCapabilityLabel);
    _imageCapabilityLabel->hide();

    _errorLabel = new QLabel(this);
    _errorLabel->setObjectName(QStringLiteral("CodexError"));
    _errorLabel->setWordWrap(true);
    _errorLabel->hide();
    rootLayout->addWidget(_errorLabel);

    _conversationScroll = new QScrollArea(this);
    _conversationScroll->setObjectName(QStringLiteral("CodexConversation"));
    _conversationScroll->setWidgetResizable(true);
    _conversationScroll->setFrameShape(QFrame::NoFrame);
    _conversationScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The viewport is a separate child and can otherwise retain the host's
    // default (white) palette around the conversation widget.
    _conversationScroll->viewport()->setObjectName(QStringLiteral("CodexConversationViewport"));
    _conversationScroll->viewport()->setStyleSheet(QStringLiteral("background:%1;border:0;").arg(surface));
    _conversationHost = new QWidget(_conversationScroll);
    _conversationHost->setObjectName(QStringLiteral("CodexConversationHost"));
    _conversationLayout = new QVBoxLayout(_conversationHost);
    _conversationLayout->setContentsMargins(14, 12, 14, 10);
    _conversationLayout->setSpacing(10);
    _conversationScroll->setWidget(_conversationHost);
    rootLayout->addWidget(_conversationScroll, 1);

    _emptyStateLabel = new QLabel(_conversationHost);
    _emptyStateLabel->setObjectName(QStringLiteral("CodexEmpty"));
    _emptyStateLabel->setAlignment(Qt::AlignCenter);
    _emptyStateLabel->setWordWrap(true);
    _conversationLayout->addWidget(_emptyStateLabel, 1);
    _conversationLayout->addStretch(1);

    _diagnosticsFrame = new QFrame(this);
    _diagnosticsFrame->setObjectName(QStringLiteral("CodexDiagnostics"));
    auto* diagnosticsLayout = new QVBoxLayout(_diagnosticsFrame);
    diagnosticsLayout->setContentsMargins(12, 9, 12, 10);
    diagnosticsLayout->setSpacing(5);
    _runtimeSourceLabel = new QLabel(_diagnosticsFrame);
    _runtimeSourceLabel->setObjectName(QStringLiteral("CodexRuntimeSource"));
    _runtimeSourceLabel->setWordWrap(true);
    _runtimeSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    diagnosticsLayout->addWidget(_runtimeSourceLabel);
    _aiWorkspaceSourceLabel = new QLabel(_diagnosticsFrame);
    _aiWorkspaceSourceLabel->setObjectName(QStringLiteral("CodexAiWorkspaceSource"));
    _aiWorkspaceSourceLabel->setWordWrap(true);
    _aiWorkspaceSourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    diagnosticsLayout->addWidget(_aiWorkspaceSourceLabel);
    _logView = new QPlainTextEdit(_diagnosticsFrame);
    _logView->setObjectName(QStringLiteral("CodexLog"));
    _logView->setMaximumHeight(105);
    configureBoundedTextView(_logView, 800);
    diagnosticsLayout->addWidget(_logView);
    _diagnosticsFrame->hide();
    _sourceSummaryLabel->setToolTip(tr("AI 工作台配置与安全凭据状态"));
    rootLayout->addWidget(_diagnosticsFrame);

    auto* composerWrap = new QFrame(this);
    composerWrap->setObjectName(QStringLiteral("CodexComposerWrap"));
    auto* composerWrapLayout = new QVBoxLayout(composerWrap);
    composerWrapLayout->setContentsMargins(14, 9, 14, 12);
    composerWrapLayout->setSpacing(0);
    auto* composer = new QFrame(composerWrap);
    composer->setObjectName(QStringLiteral("CodexComposer"));
    composer->setMinimumHeight(96);
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(8, 7, 8, 8);
    composerLayout->setSpacing(4);
    auto* attachmentHost = new QWidget(composer);
    attachmentHost->setObjectName(QStringLiteral("CodexAttachments"));
    _attachmentLayout = new QHBoxLayout(attachmentHost);
    _attachmentLayout->setContentsMargins(1, 0, 1, 0);
    _attachmentLayout->setSpacing(6);
    attachmentHost->hide();
    composerLayout->addWidget(attachmentHost);
    _promptEdit = new QPlainTextEdit(composer);
    _promptEdit->setObjectName(QStringLiteral("CodexPrompt"));
    _promptEdit->setPlaceholderText(tr("向 Codex 发送消息"));
    _promptEdit->setMinimumHeight(42);
    _promptEdit->setMaximumHeight(58);
    _promptEdit->setAcceptDrops(true);
    _promptEdit->installEventFilter(this);
    _promptEdit->viewport()->setAcceptDrops(true);
    _promptEdit->viewport()->installEventFilter(this);
    composerLayout->addWidget(_promptEdit, 1);

    auto* composerControls = new QHBoxLayout();
    composerControls->setContentsMargins(1, 0, 0, 0);
    composerControls->setSpacing(6);
    _newConversationButton = new QToolButton(composer);
    _newConversationButton->setObjectName(QStringLiteral("CodexNewConversation"));
    _newConversationButton->setText(QStringLiteral("+"));
    _newConversationButton->setToolTip(tr("新会话"));
    _newConversationButton->setFixedSize(29, 29);
    composerControls->addWidget(_newConversationButton);

    _projectButton = new QToolButton(composer);
    _projectButton->setObjectName(QStringLiteral("CodexProject"));
    _projectButton->setText(tr("当前项目"));
    _projectButton->setToolTip(tr("打开当前项目目录"));
    _projectButton->setFixedHeight(29);
    composerControls->addWidget(_projectButton);
    _projectButton->hide();

    _approvalButton = new QToolButton(composer);
    _approvalButton->setObjectName(QStringLiteral("CodexApprovalMode"));
    _approvalButton->setFixedHeight(29);
    _approvalButton->setPopupMode(QToolButton::InstantPopup);
    composerControls->addWidget(_approvalButton);

    _approvalMenu = new QMenu(_approvalButton);
    _approvalMenu->setTitle(tr("Codex 操作权限"));
    _approvalGroup = new QActionGroup(this);
    _approvalGroup->setExclusive(true);
    const struct PermissionOption {
        const char* mode;
        QString title;
        QString description;
    } permissionOptions[] = {
        {"request", tr("请求批准"), tr("编辑文件或访问网络时始终询问")},
        {"auto", tr("替我审批"), tr("仅对检测到的风险操作请求批准")},
        {"full", tr("完全访问权限"), tr("可访问互联网和电脑上的任何文件")}
    };
    for (const PermissionOption& option : permissionOptions) {
        QAction* action = _approvalMenu->addAction(option.title + QStringLiteral("\n") + option.description);
        action->setCheckable(true);
        action->setData(QString::fromLatin1(option.mode));
        action->setToolTip(option.description);
        _approvalGroup->addAction(action);
        _approvalActions.insert(QString::fromLatin1(option.mode), action);
        connect(action, &QAction::triggered, this, [this, mode = QString::fromLatin1(option.mode)] {
            chooseApprovalMode(mode);
        });
        if (QString::fromLatin1(option.mode) == QStringLiteral("auto")) {
            _approvalMenu->addSeparator();
        }
    }
    _approvalButton->setMenu(_approvalMenu);
    // Keep the permission selector visible in the narrow dock.  Ignored lets
    // the layout collapse this control to zero before the model/send controls.
    _approvalButton->setMinimumWidth(112);
    _approvalButton->setMaximumWidth(156);
    _approvalButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    _modelCombo = new QComboBox(composer);
    _modelCombo->setObjectName(QStringLiteral("CodexModel"));
    _modelCombo->setMinimumWidth(82);
    _modelCombo->setMaximumWidth(138);
    _modelCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    _modelCombo->setFixedHeight(29);
    composerControls->addWidget(_modelCombo);

    _reasoningButton = new QToolButton(composer);
    _reasoningButton->setObjectName(QStringLiteral("CodexReasoning"));
    _reasoningButton->setToolTip(tr("推理强度"));
    _reasoningButton->setMinimumWidth(48);
    _reasoningButton->setMaximumWidth(72);
    _reasoningButton->setFixedHeight(29);
    _reasoningButton->setPopupMode(QToolButton::InstantPopup);
    _reasoningButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    composerControls->addWidget(_reasoningButton);

    composerControls->addStretch(1);

    _reasoningMenu = new QMenu(_reasoningButton);
    _reasoningGroup = new QActionGroup(this);
    _reasoningGroup->setExclusive(true);
    _reasoningButton->setMenu(_reasoningMenu);

    _sendButton = new QToolButton(composer);
    _sendButton->setObjectName(QStringLiteral("CodexSend"));
    _sendButton->setText(QStringLiteral("↑"));
    _sendButton->setToolTip(tr("发送"));
    _sendButton->setFixedSize(31, 31);
    _sendButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    _sendButton->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    composerControls->addWidget(_sendButton);
    composerLayout->addLayout(composerControls);
    composerWrapLayout->addWidget(composer);
    rootLayout->addWidget(composerWrap);

    connect(_retryButton, &QToolButton::clicked, this, &CodexAgentWorkspace::retryRequested);
    connect(railNew, &QToolButton::clicked, this, &CodexAgentWorkspace::newConversationRequested);
    connect(railSessions, &QToolButton::clicked, this, [this] { emit sessionListRequested(); });
    connect(railArchive, &QToolButton::clicked, this, [this] { emit sessionListRequested(); });
    connect(_headerNewConversationButton, &QToolButton::clicked, this, &CodexAgentWorkspace::newConversationRequested);
    connect(_sessionsMenu, &QMenu::aboutToShow, this, [this] { emit sessionListRequested(); });
    connect(_newConversationButton, &QToolButton::clicked, this, &CodexAgentWorkspace::newConversationRequested);
    connect(_projectButton, &QToolButton::clicked, this, &CodexAgentWorkspace::openProjectRequested);
    connect(_closeButton, &QToolButton::clicked, this, &CodexAgentWorkspace::closeRequested);
    connect(_stopAction, &QAction::triggered, this, &CodexAgentWorkspace::stopRequested);
    connect(_workbenchAction, &QAction::triggered, this, [this] {
        showWorkbench();
        emit workbenchRequested();
    });
    connect(_diagnosticsButton, &QToolButton::clicked, this, [this] {
        setDiagnosticsVisible(!_diagnosticsFrame->isVisible());
    });
    connect(_sendButton, &QToolButton::clicked, this, [this] {
        if (_turnInProgress) emit stopRequested();
        else submitPrompt();
    });
    connect(_promptEdit, &QPlainTextEdit::textChanged, this, &CodexAgentWorkspace::refreshActionState);
    connect(_modelCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        _modelCombo->setToolTip(selectedModel());
        emit modelSelectionChanged(selectedModel());
        refreshActionState();
    });

    setRuntimeSourceStatus(tr("正在自动发现 Codex 运行环境。"), false);
    setAiWorkspaceSourceStatus(tr("正在读取 AI 工作台配置。"), false);
    setApprovalMode(QStringLiteral("full"));
    setReasoningOptions({}, {});
    setConnectionStatus(tr("正在连接"), false);
    setPromptEnabled(false);
    refreshActionState();
    refreshTheme();
}

void CodexAgentWorkspace::refreshTheme()
{
    if (_applyingTheme) return;
    _applyingTheme = true;

    const WorkspaceTheme theme = currentWorkspaceTheme();
    QString baseStyle = property("cgplay.baseCodexStyleSheet").toString();
    if (baseStyle.isEmpty()) {
        baseStyle = styleSheet();
        setProperty("cgplay.baseCodexStyleSheet", baseStyle);
    }

    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(baseStyle + codexThemeOverride(theme));

    // QAbstractScrollArea paints its viewport independently from the scroll
    // frame.  Keep both the frame and viewport on the same runtime surface.
    if (_conversationScroll) {
        _conversationScroll->setAttribute(Qt::WA_StyledBackground, true);
        _conversationScroll->setPalette(QApplication::palette());
        _conversationScroll->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        _conversationScroll->viewport()->setPalette(QApplication::palette());
        _conversationScroll->viewport()->setStyleSheet(
            QStringLiteral("QWidget#CodexConversationViewport{background:%1;border:0;}")
                .arg(theme.surface));
    }
    if (_conversationHost) {
        _conversationHost->setAttribute(Qt::WA_StyledBackground, true);
        _conversationHost->setStyleSheet(
            QStringLiteral("QWidget#CodexConversationHost{background:%1;color:%2;}")
                .arg(theme.surface, theme.text));
    }

    if (_sourceSummaryLabel) {
        _sourceSummaryLabel->setStyleSheet(
            QStringLiteral("QLabel#CodexSourceSummary{background:%1;color:%2;border-bottom:1px solid %3;padding:3px 14px;font-size:9px;}")
                .arg(theme.window, _aiConfigured ? QString::fromLatin1(kReady) : QString::fromLatin1(kWarning), theme.border));
    }
    if (_imageCapabilityLabel) {
        const bool available = property("codexImageGenerationAvailable").toBool();
        _imageCapabilityLabel->setStyleSheet(
            QStringLiteral("QLabel#CodexImageCapability{background:%1;color:%2;padding:3px 18px;font-size:10px;}")
                .arg(theme.window, available ? QString::fromLatin1(kReady) : QString::fromLatin1(kWarning)));
    }
    if (_statusLabel) {
        _statusLabel->setStyleSheet(
            QStringLiteral("QLabel#CodexConnectionStatus{background:transparent;color:%1;font-size:10px;}")
                .arg(_connected ? QString::fromLatin1(kReady) : QString::fromLatin1(kWarning)));
    }
    if (_modelCombo) {
        const QString comboText = QStringLiteral("#F4F7FB");
        _modelCombo->setStyleSheet(QStringLiteral(
            "QComboBox#CodexModel{background:%1;color:%2;border:1px solid %3;}"
            "QComboBox#CodexModel QAbstractItemView{background:%4;color:%2;border:1px solid %3;"
            "selection-background-color:%5;selection-color:%2;}"
        ).arg(theme.input, comboText, theme.border, theme.surface, theme.accent));
        if (auto* popup = _modelCombo->view()) {
            popup->setStyleSheet(QStringLiteral(
                "QAbstractItemView{background:%1;color:%2;border:1px solid %3;font-size:12px;font-weight:600;}"
                "QAbstractItemView::item{color:%2;background:%1;padding:6px 8px;}"
                "QAbstractItemView::item:selected{color:#FFFFFF;background:%4;}"
            ).arg(theme.surface, comboText, theme.border, theme.accent));
            QPalette popupPalette = popup->palette();
            popupPalette.setColor(QPalette::Base, QColor(theme.surface));
            popupPalette.setColor(QPalette::Text, QColor(comboText));
            popupPalette.setColor(QPalette::Highlight, QColor(theme.accent));
            popupPalette.setColor(QPalette::HighlightedText, QColor(Qt::white));
            popup->setPalette(popupPalette);
            popup->setAttribute(Qt::WA_StyledBackground, true);
        }
    }
    const QString controlStyle = QStringLiteral(
        "QToolButton{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:4px;}"
        "QToolButton:hover{background:%4;color:%2;border-color:%3;}"
        "QToolButton:pressed{background:%5;color:%6;border-color:%5;}"
        "QToolButton:disabled{background:%1;color:%2;border:1px solid %3;}"
    ).arg(theme.raised, theme.text, theme.border, theme.hover, theme.accent, theme.accentText);
    const QList<QToolButton*> materialButtons = {
        _headerNewConversationButton, _sessionsButton, _overflowButton, _closeButton,
        _newConversationButton, _projectButton, _approvalButton, _reasoningButton, _sendButton
    };
    for (QToolButton* button : materialButtons) {
        if (!button) continue;
        button->setStyleSheet(controlStyle);
        QPalette buttonPalette = button->palette();
        buttonPalette.setColor(QPalette::Button, theme.raised);
        buttonPalette.setColor(QPalette::ButtonText, theme.text);
        buttonPalette.setColor(QPalette::Disabled, QPalette::Button, theme.raised);
        buttonPalette.setColor(QPalette::Disabled, QPalette::ButtonText, theme.text);
        button->setPalette(buttonPalette);
    }

    if (_workbenchDialog) {
        _workbenchDialog->setAttribute(Qt::WA_StyledBackground, true);
        _workbenchDialog->setPalette(QApplication::palette());
        _workbenchDialog->setStyleSheet(QStringLiteral(
            "QDialog#CodexControlCenter{background:%1;color:%6;}"
            "QWidget{color:%6;}"
            "QTabWidget,QTabWidget::pane{background:%2;color:%6;border:1px solid %5;}"
            "QTabBar{background:%2;}"
            "QTabBar::tab{background:%3;color:%7;padding:7px 11px;border:0;}"
            "QTabBar::tab:selected,QTabBar::tab:hover{background:%4;color:%6;}"
            "QToolButton{background:%3;color:%6;border:1px solid %5;border-radius:5px;padding:5px 9px;}"
            "QToolButton:hover{background:%4;border-color:%8;}"
            "QToolButton:pressed{background:%14;color:%9;}"
            "QLineEdit,QPlainTextEdit,QTextEdit{background:%10;color:%6;border:1px solid %5;border-radius:5px;padding:5px 7px;}"
            "QLineEdit:focus,QPlainTextEdit:focus,QTextEdit:focus{border-color:%8;}"
            "QScrollBar:vertical{background:transparent;width:8px;}"
            "QScrollBar::handle:vertical{background:%5;border-radius:4px;min-height:24px;}"
        ).arg(theme.window, theme.surface, theme.raised, theme.hover, theme.border, theme.text,
              theme.muted, theme.accent, theme.accentText, theme.input, theme.error,
              theme.accentSoft, theme.accentHoverSoft, theme.accentPressedSoft));
        if (_workbenchTabs) {
            _workbenchTabs->setAttribute(Qt::WA_StyledBackground, true);
            _workbenchTabs->setPalette(QApplication::palette());
        }
        for (QPlainTextEdit* view : std::as_const(_workbenchViews)) {
            if (!view) continue;
            view->setStyleSheet(QStringLiteral(
                "QPlainTextEdit{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:6px;}")
                .arg(theme.input, theme.text, theme.border));
        }
        for (QLineEdit* input : std::as_const(_workbenchInputs)) {
            if (!input) continue;
            input->setStyleSheet(QStringLiteral(
                "QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:5px;padding:5px 7px;}")
                .arg(theme.input, theme.text, theme.border));
        }
    }

    _applyingTheme = false;
    update();
}

void CodexAgentWorkspace::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!qApp) return;
    const QString type = qApp->property("cgplay.backgroundType").toString().toLower();
    const QString path = qApp->property("cgplay.backgroundImage").toString().trimmed();
    if ((type != QStringLiteral("image") && type != QStringLiteral("texture")) || path.isEmpty()) return;

    QPainter painter(this);
    BackdropRenderOptions backdropOptions;
    backdropOptions.drawFallback = false;
    backdropOptions.readabilityWashAlpha = 0;
    if (!drawApplicationBackdrop(painter, rect(), this, backdropOptions)) return;

    const int opacity = qBound(0, qApp->property("cgplay.panelOpacity").toInt(), 100);
    QColor panel = appThemeColor("cgplay.panelColor", QColor(QStringLiteral("#171B20")));
    QColor secondary = appThemeColor("cgplay.backgroundSecondary", panel.darker(112));
    panel.setAlpha(qRound(opacity * 255.0 / 100.0));
    secondary.setAlpha(qRound(opacity * 255.0 / 100.0));
    const QColor border = appThemeColor("cgplay.borderColor", QColor(255, 255, 255, 24));
    const QColor accent = appThemeColor("cgplay.accentColor", QColor(0xFF, 0x8A, 0x3D));
    const bool light = qApp->property("cgplay.themeMode").toString().compare(
        QStringLiteral("light"), Qt::CaseInsensitive) == 0;

    QLinearGradient glass(0, 0, 0, height());
    glass.setColorAt(0.0, panel.lighter(light ? 104 : 108));
    glass.setColorAt(0.42, panel);
    glass.setColorAt(1.0, secondary);
    painter.fillRect(rect(), glass);

    QLinearGradient topEdge(0, 0, 0, 46);
    topEdge.setColorAt(0.0, light ? QColor(255, 255, 255, 32) : QColor(255, 255, 255, 13));
    topEdge.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(QRect(0, 0, width(), 46), topEdge);
    if (!light) {
        QRadialGradient warm(width() * 0.82, height() * 0.08, width() * 0.56);
        warm.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 12));
        warm.setColorAt(0.52, QColor(accent.red(), accent.green(), accent.blue(), 3));
        warm.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.fillRect(rect(), warm);
    }
    painter.setPen(QPen(light ? border : QColor(255, 255, 255, 17), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void CodexAgentWorkspace::setRuntimeSourceStatus(const QString& status, bool configured)
{
    _runtimeConfigured = configured;
    _runtimeSourceLabel->setText(status);
    refreshActionState();
}

void CodexAgentWorkspace::setAiWorkspaceSourceStatus(const QString& status, bool configured)
{
    _aiConfigured = configured;
    _aiWorkspaceSourceLabel->setText(status);
    const QString compact = compactSourceStatus(status);
    _sourceSummaryLabel->setText(configured
        ? tr("AI 工作台 · %1").arg(compact)
        : tr("AI 工作台 · %1").arg(status.simplified()));
    _sourceSummaryLabel->setToolTip(status);
    _sourceSummaryLabel->setStyleSheet(QStringLiteral("color:%1;")
        .arg(configured ? QString::fromLatin1(kReady) : QString::fromLatin1(kWarning)));
    refreshActionState();
}

void CodexAgentWorkspace::setImageGenerationCapability(bool available, const QString& reason)
{
    const QString detail = reason.trimmed();
    _imageCapabilityLabel->setText(available
        ? tr("图片生成能力：可用")
        : (detail.isEmpty() ? tr("图片生成能力：当前服务商未提供") : tr("图片生成能力：不可用 · %1").arg(detail)));
    _imageCapabilityLabel->setToolTip(detail);
    _imageCapabilityLabel->setStyleSheet(QStringLiteral("padding:3px 18px;font-size:10px;color:%1;")
        .arg(available ? QString::fromLatin1(kReady) : QString::fromLatin1(kWarning)));
    _imageCapabilityLabel->setVisible(!available);
    setProperty("codexImageGenerationAvailable", available);
    setProperty("codexImageGenerationReason", detail);
}

void CodexAgentWorkspace::setProjectPath(const QString& path)
{
    _projectPath = QFileInfo(path).absoluteFilePath();
    _projectButton->setToolTip(_projectPath.isEmpty() ? tr("当前项目") : _projectPath);
    refreshActionState();
}

void CodexAgentWorkspace::setModelOptions(const QStringList& models, const QString& selectedModel)
{
    QSignalBlocker blocker(_modelCombo);
    _modelCombo->clear();
    for (const QString& model : models) {
        const QString trimmed = model.trimmed();
        if (trimmed.isEmpty()) continue;
        bool present = false;
        for (int index = 0; index < _modelCombo->count(); ++index) {
            if (_modelCombo->itemData(index).toString().compare(trimmed, Qt::CaseInsensitive) == 0) {
                present = true;
                break;
            }
        }
        if (!present) _modelCombo->addItem(displayModelName(trimmed), trimmed);
    }
    int selectedIndex = -1;
    for (int index = 0; index < _modelCombo->count(); ++index) {
        if (_modelCombo->itemData(index).toString().compare(selectedModel.trimmed(), Qt::CaseInsensitive) == 0) {
            selectedIndex = index;
            break;
        }
    }
    if (selectedIndex < 0 && !selectedModel.trimmed().isEmpty()) {
        _modelCombo->addItem(displayModelName(selectedModel), selectedModel.trimmed());
        selectedIndex = _modelCombo->count() - 1;
    }
    if (selectedIndex >= 0) _modelCombo->setCurrentIndex(selectedIndex);
    else if (_modelCombo->count() > 0) _modelCombo->setCurrentIndex(0);
    _modelCombo->setToolTip(this->selectedModel());
    refreshActionState();
}

void CodexAgentWorkspace::setReasoningOptions(
    const QStringList& efforts,
    const QString& selectedEffort)
{
    _reasoningMenu->clear();
    _reasoningActions.clear();
    _reasoningOptionsAvailable = false;

    QStringList normalizedEfforts;
    for (const QString& effort : efforts) {
        const QString normalized = effort.trimmed().toLower();
        if (!normalized.isEmpty() && !normalizedEfforts.contains(normalized, Qt::CaseInsensitive)) {
            normalizedEfforts.push_back(normalized);
        }
    }

    for (const QString& effort : normalizedEfforts) {
        QAction* action = _reasoningMenu->addAction(reasoningEffortDisplayName(effort));
        action->setCheckable(true);
        action->setData(effort);
        _reasoningGroup->addAction(action);
        _reasoningActions.insert(effort, action);
        connect(action, &QAction::triggered, this, [this, effort] {
            chooseReasoningEffort(effort);
        });
    }

    QString target = selectedEffort.trimmed().toLower();
    if (!normalizedEfforts.contains(target, Qt::CaseInsensitive)) {
        target = normalizedEfforts.isEmpty() ? QString() : normalizedEfforts.front();
    }
    _reasoningOptionsAvailable = !normalizedEfforts.isEmpty();
    for (auto it = _reasoningActions.cbegin(); it != _reasoningActions.cend(); ++it) {
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(it.key().compare(target, Qt::CaseInsensitive) == 0);
    }
    _reasoningButton->setText(target.isEmpty() ? tr("默认") : reasoningEffortDisplayName(target));
    _reasoningButton->setToolTip(target.isEmpty()
        ? tr("当前模型未返回可选推理强度")
        : tr("推理强度：%1").arg(reasoningEffortDisplayName(target)));
    refreshActionState();
}

void CodexAgentWorkspace::setApprovalMode(const QString& approvalMode)
{
    const QString mode = _approvalActions.contains(approvalMode) ? approvalMode : QStringLiteral("full");
    _approvalMode = mode;
    for (auto it = _approvalActions.cbegin(); it != _approvalActions.cend(); ++it) {
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(it.key() == mode);
    }
    _approvalButton->setText(QStringLiteral("◉ ") + approvalDisplayName(mode));
    _approvalButton->setToolTip(_approvalActions.value(mode)->toolTip());
}

void CodexAgentWorkspace::setConnectionStatus(const QString& status, bool connected)
{
    _connected = connected;
    if (connected) {
        _retryAvailable = false;
        _retryButton->hide();
        _errorLabel->hide();
    }
    setStatusText(status, connected, false);
    updateEmptyState();
    refreshActionState();
}

void CodexAgentWorkspace::setPromptEnabled(bool enabled)
{
    _promptEnabled = enabled;
    refreshActionState();
}

void CodexAgentWorkspace::setRetryAvailable(bool available)
{
    _retryAvailable = available;
    _retryButton->setVisible(available);
    if (available) setStatusText(tr("连接失败"), false, true);
    updateEmptyState();
    refreshActionState();
}

void CodexAgentWorkspace::appendStreamText(const QString& text)
{
    if (text.isEmpty()) return;
    ensureAssistantMessage();
    _assistantTextLabel->setVisible(true);
    _assistantTextLabel->setText(_assistantTextLabel->text() + text);
    const QString assistantText = _assistantTextLabel->text().toLower();
    const bool nativeImageToolUnavailable =
        (assistantText.contains(QStringLiteral("image-generation tool")) ||
         assistantText.contains(QStringLiteral("image_gen tool")) ||
         assistantText.contains(QStringLiteral("`image_gen` tool"))) &&
        assistantText.contains(QStringLiteral("not available"));
    // The native Codex image tool and CGPlay's independently configured image
    // API are separate capabilities. Do not let a native-tool message hide a
    // working CGPlay image API after a successful generation.
    if (nativeImageToolUnavailable &&
        !property("codexImageGenerationAvailable").toBool()) {
        setImageGenerationCapability(false, tr("当前会话未暴露原生图片生成工具"));
    }
    scrollToBottom();
}

void CodexAgentWorkspace::appendLogLine(const QString& text)
{
    if (text.isEmpty()) return;
    _logView->appendPlainText(text);
    appendWorkbenchEvent(QStringLiteral("terminal"), text);
}

void CodexAgentWorkspace::appendWorkbenchEvent(const QString& category, const QString& text)
{
    if (text.trimmed().isEmpty()) return;
    addWorkbenchRow(category, text.trimmed());
}

void CodexAgentWorkspace::submitWorkbenchContext(const QString& prompt)
{
    const QString normalized = prompt.trimmed();
    if (normalized.isEmpty()) return;
    appendUserMessage(normalized);
    emit promptSubmitted(normalized);
}

bool CodexAgentWorkspace::browserNavigate(const QString& url, QString* error)
{
    if (!_browserView) { if (error) *error = tr("Browser is not open."); return false; }
    const QUrl target = QUrl::fromUserInput(url.trimmed());
    if (!target.isValid() || target.scheme().isEmpty()) { if (error) *error = tr("Invalid browser URL."); return false; }
    _browserView->setUrl(target);
    return true;
}

namespace {
QString browserJsString(const QString& value)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}
}

bool CodexAgentWorkspace::browserClick(const QString& selector, QString* error)
{
    if (!_browserView) { if (error) *error = tr("Browser is not open."); return false; }
    if (selector.trimmed().isEmpty()) { if (error) *error = tr("CSS selector is required."); return false; }
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool clicked = false; QString message;
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    _browserView->page()->runJavaScript(QStringLiteral("(() => { const e=document.querySelector(%1); if(!e) return 'not_found'; e.click(); return 'clicked'; })()").arg(browserJsString(selector)), [&](const QVariant& value) { message = value.toString(); clicked = message == QStringLiteral("clicked"); loop.quit(); });
    timer.start(10000); loop.exec();
    if (!clicked && error) *error = message == QStringLiteral("not_found") ? tr("Element not found.") : tr("Browser click timed out.");
    return clicked;
}

bool CodexAgentWorkspace::browserType(const QString& selector, const QString& text, QString* error)
{
    if (!_browserView) { if (error) *error = tr("Browser is not open."); return false; }
    if (selector.trimmed().isEmpty()) { if (error) *error = tr("CSS selector is required."); return false; }
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool typed = false; QString message;
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QString script = QStringLiteral("(() => { const e=document.querySelector(%1); if(!e) return 'not_found'; e.focus(); e.value=%2; e.dispatchEvent(new Event('input',{bubbles:true})); e.dispatchEvent(new Event('change',{bubbles:true})); return 'typed'; })()").arg(browserJsString(selector), browserJsString(text));
    _browserView->page()->runJavaScript(script, [&](const QVariant& value) { message = value.toString(); typed = message == QStringLiteral("typed"); loop.quit(); });
    timer.start(10000); loop.exec();
    if (!typed && error) *error = message == QStringLiteral("not_found") ? tr("Element not found.") : tr("Browser input timed out.");
    return typed;
}

QString CodexAgentWorkspace::browserExtract(const QString& selector, QString* error)
{
    if (!_browserView) { if (error) *error = tr("Browser is not open."); return {}; }
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); QString result; bool completed = false;
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QString script = selector.trimmed().isEmpty()
        ? QStringLiteral("document.body ? document.body.innerText : ''")
        : QStringLiteral("(() => { const e=document.querySelector(%1); return e ? e.innerText : ''; })()").arg(browserJsString(selector));
    _browserView->page()->runJavaScript(script, [&](const QVariant& value) { result = value.toString().left(24000); completed = true; loop.quit(); });
    timer.start(10000); loop.exec();
    if (!completed && error) *error = tr("Browser extraction timed out.");
    return result;
}

bool CodexAgentWorkspace::captureBrowserSnapshot()
{
    if (!_browserView) showWorkbench();
    if (!_browserView) return false;

    const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("browser_snapshots"));
    QDir().mkpath(directory);
    const QString imagePath = QDir(directory).filePath(
        QStringLiteral("page_%1.png").arg(
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"))));
    _browserView->grab().save(imagePath, "PNG");
    _browserView->page()->runJavaScript(
        QStringLiteral("({text:document.body?document.body.innerText:'',html:document.documentElement?document.documentElement.outerHTML:'',selection:window.getSelection().toString()})"),
        [this, imagePath](const QVariant& result) {
            if (!_browserView) return;
            const QVariantMap page = result.toMap();
            const QString prompt = tr("Browser context snapshot.\nURL: %1\nTitle: %2\nSelected text:\n%3\nPage text:\n%4\nDOM excerpt:\n%5")
                .arg(_browserView->url().toDisplayString(), _browserView->title(),
                     page.value(QStringLiteral("selection")).toString().left(12000),
                     page.value(QStringLiteral("text")).toString().left(24000),
                     page.value(QStringLiteral("html")).toString().left(12000));
            emit browserSnapshotSubmitted(QFileInfo(imagePath).isFile() ? QStringList{imagePath} : QStringList{}, prompt);
            appendWorkbenchEvent(QStringLiteral("browser"), tr("Page snapshot sent: %1").arg(_browserView->url().toDisplayString()));
        });
    return true;
}

void CodexAgentWorkspace::showWorkbench()
{
    if (!_workbenchDialog) {
        _workbenchDialog = new QDialog(this);
        _workbenchDialog->setObjectName(QStringLiteral("CodexControlCenter"));
        _workbenchDialog->setWindowTitle(tr("Codex 控制中心"));
        _workbenchDialog->resize(760, 540);
        auto* layout = new QVBoxLayout(_workbenchDialog);
        layout->setContentsMargins(10, 10, 10, 10);
        _workbenchTabs = new QTabWidget(_workbenchDialog);
        const QList<QPair<QString, QString>> tabs{
            {QStringLiteral("browser"), tr("网页上下文")},
            {QStringLiteral("agents"), tr("任务与代理")},
            {QStringLiteral("extensions"), tr("MCP / Skills / 插件 / Apps")},
            {QStringLiteral("changes"), tr("变更审查")},
            {QStringLiteral("terminal"), tr("终端")},
            {QStringLiteral("rules"), tr("规则与配置")},
            {QStringLiteral("media"), tr("媒体资产与片段")}
        };
        for (const auto& tab : tabs) {
            auto* page = new QWidget(_workbenchTabs);
            auto* pageLayout = new QVBoxLayout(page);
            pageLayout->setContentsMargins(8, 8, 8, 8);
            auto* controls = new QHBoxLayout();
            if (tab.first == QStringLiteral("browser")) {
                auto* back = new QToolButton(page); back->setText(tr("后退")); back->setToolTip(tr("后退")); controls->addWidget(back);
                auto* forward = new QToolButton(page); forward->setText(tr("前进")); forward->setToolTip(tr("前进")); controls->addWidget(forward);
                auto* reload = new QToolButton(page); reload->setText(tr("刷新")); reload->setToolTip(tr("刷新页面")); controls->addWidget(reload);
                auto* address = new QLineEdit(page); address->setPlaceholderText(tr("输入网址")); controls->addWidget(address, 1);
                auto* open = new QToolButton(page); open->setText(tr("打开")); open->setToolTip(tr("打开网页")); controls->addWidget(open);
                pageLayout->addLayout(controls);
                auto* browser = new QWebEngineView(page);
                _browserView = browser;
                browser->setUrl(QUrl(QStringLiteral("https://www.google.com")));
                connect(back, &QToolButton::clicked, browser, &QWebEngineView::back);
                connect(forward, &QToolButton::clicked, browser, &QWebEngineView::forward);
                connect(reload, &QToolButton::clicked, browser, &QWebEngineView::reload);
                const auto navigate = [browser, address] {
                    QUrl url = QUrl::fromUserInput(address->text().trimmed());
                    if (url.isValid() && !url.isEmpty()) browser->setUrl(url);
                };
                connect(open, &QToolButton::clicked, this, navigate);
                connect(address, &QLineEdit::returnPressed, this, navigate);
                connect(browser, &QWebEngineView::urlChanged, address, [address](const QUrl& url) { address->setText(url.toDisplayString()); });
                connect(browser, &QWebEngineView::loadStarted, this, [this, browser] { appendWorkbenchEvent(QStringLiteral("browser"), tr("Loading: %1").arg(browser->url().toDisplayString())); });
                connect(browser, &QWebEngineView::loadFinished, this, [this, browser](bool ok) { appendWorkbenchEvent(QStringLiteral("browser"), ok ? tr("Loaded: %1").arg(browser->title()) : tr("Load failed: %1").arg(browser->url().toDisplayString())); });
                connect(browser->page()->profile(), &QWebEngineProfile::downloadRequested, this, [this](QWebEngineDownloadRequest* request) {
                    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("browser_downloads"));
                    QDir().mkpath(dir); request->setDownloadDirectory(dir); request->accept();
                    appendWorkbenchEvent(QStringLiteral("browser"), tr("Download started: %1").arg(QDir(dir).filePath(request->downloadFileName())));
                });
                auto* sendPage = new QToolButton(page); sendPage->setText(tr("发送页面")); sendPage->setToolTip(tr("将当前网址、标题和选中文本发送给 Codex")); controls->addWidget(sendPage);
                connect(sendPage, &QToolButton::clicked, this, [this, browser] {
                    browser->page()->runJavaScript(QStringLiteral("window.getSelection().toString()"),
                        [this, browser](const QVariant& selection) {
                            emit browserContextSubmitted(browser->url().toDisplayString(), browser->title(), selection.toString());
                        });
                });
                auto* snapshot = new QToolButton(page); snapshot->setText(tr("页面快照")); snapshot->setToolTip(tr("捕获页面截图、正文、DOM 与选区并发送给 Codex")); controls->addWidget(snapshot);
                connect(snapshot, &QToolButton::clicked, this, [this] { captureBrowserSnapshot(); });
                pageLayout->addWidget(browser, 1);
                _workbenchTabs->addTab(page, tab.second);
                continue;
            } else if (tab.first == QStringLiteral("extensions")) {
                auto* refresh = new QToolButton(page); refresh->setText(tr("刷新目录")); refresh->setToolTip(tr("刷新 MCP、Skills、插件和 Apps"));
                connect(refresh, &QToolButton::clicked, this, &CodexAgentWorkspace::refreshNativeCatalogRequested); controls->addWidget(refresh);
                auto* mcp = new QToolButton(page); mcp->setText(tr("重载 MCP")); mcp->setToolTip(tr("重新加载 MCP 配置"));
                connect(mcp, &QToolButton::clicked, this, &CodexAgentWorkspace::mcpReloadRequested); controls->addWidget(mcp);
                auto* oauth = new QToolButton(page); oauth->setText(tr("MCP 授权")); oauth->setToolTip(tr("对 MCP 服务器启动 OAuth 授权")); controls->addWidget(oauth);
                connect(oauth, &QToolButton::clicked, this, [this] { bool ok = false; const QString server = QInputDialog::getText(this, tr("MCP 授权"), tr("服务器名称"), QLineEdit::Normal, {}, &ok); if (ok && !server.trimmed().isEmpty()) emit mcpLoginRequested(server.trimmed()); });
                auto* resource = new QToolButton(page); resource->setText(tr("读取资源")); resource->setToolTip(tr("读取 MCP 资源 URI")); controls->addWidget(resource);
                connect(resource, &QToolButton::clicked, this, [this] { bool ok = false; const QString server = QInputDialog::getText(this, tr("MCP 资源"), tr("服务器名称"), QLineEdit::Normal, {}, &ok).trimmed(); if (!ok || server.isEmpty()) return; const QString uri = QInputDialog::getText(this, tr("MCP 资源"), tr("URI"), QLineEdit::Normal, {}, &ok).trimmed(); if (ok && !uri.isEmpty()) emit mcpResourceReadRequested(server, uri); });
                auto* plugin = new QToolButton(page); plugin->setText(tr("管理插件")); plugin->setToolTip(tr("读取、安装或卸载 Codex 插件")); controls->addWidget(plugin);
                connect(plugin, &QToolButton::clicked, this, &CodexAgentWorkspace::managePluginRequested);
                auto* market = new QToolButton(page); market->setText(tr("添加市场")); market->setToolTip(tr("添加 Codex 插件市场源")); controls->addWidget(market);
                connect(market, &QToolButton::clicked, this, &CodexAgentWorkspace::addMarketplaceRequested);
                auto* skills = new QToolButton(page); skills->setText(tr("Skills")); skills->setToolTip(tr("刷新并显示 Codex Skills 目录")); controls->addWidget(skills);
                connect(skills, &QToolButton::clicked, this, &CodexAgentWorkspace::skillsRefreshRequested);
                auto* localSkills = new QToolButton(page); localSkills->setText(tr("管理 Skills")); localSkills->setToolTip(tr("本地安装、禁用、启用或卸载 Skill")); controls->addWidget(localSkills);
                connect(localSkills, &QToolButton::clicked, this, &CodexAgentWorkspace::localSkillManageRequested);
                auto* apps = new QToolButton(page); apps->setText(tr("管理 Apps")); apps->setToolTip(tr("授权或启动目录中真实返回的 App")); controls->addWidget(apps);
                connect(apps, &QToolButton::clicked, this, &CodexAgentWorkspace::appManageRequested);
                auto* tool = new QToolButton(page); tool->setText(tr("MCP 工具")); tool->setToolTip(tr("调用已配置的 MCP 工具")); controls->addWidget(tool);
                connect(tool, &QToolButton::clicked, this, &CodexAgentWorkspace::mcpToolDialogRequested);
            } else if (tab.first == QStringLiteral("changes")) {
                auto* review = new QToolButton(page); review->setText(tr("审查未提交变更")); review->setToolTip(tr("让 Codex 创建真实的未提交变更审查任务"));
                connect(review, &QToolButton::clicked, this, &CodexAgentWorkspace::reviewRequested); controls->addWidget(review);
                auto* diff = new QToolButton(page); diff->setText(tr("完整 Diff")); diff->setToolTip(tr("读取当前工作区完整 Git diff"));
                connect(diff, &QToolButton::clicked, this, &CodexAgentWorkspace::diffRequested); controls->addWidget(diff);
                auto* fileAction = new QToolButton(page); fileAction->setText(tr("文件操作")); fileAction->setToolTip(tr("暂存、取消暂存或丢弃指定文件的未提交变更")); controls->addWidget(fileAction);
                connect(fileAction, &QToolButton::clicked, this, [this] { bool ok = false; const QString path = QInputDialog::getText(this, tr("Git 文件操作"), tr("相对项目路径"), QLineEdit::Normal, {}, &ok).trimmed(); if (!ok || path.isEmpty()) return; const QString action = QInputDialog::getItem(this, tr("Git 文件操作"), tr("操作"), {tr("暂存"), tr("取消暂存"), tr("丢弃未提交变更")}, 0, false, &ok); if (ok) emit diffFileActionRequested(path, action); });
                auto* hunk = new QToolButton(page); hunk->setText(tr("块级应用")); hunk->setToolTip(tr("粘贴完整 diff 中的一个或多个 hunk，应用到暂存区或撤销已暂存 hunk")); controls->addWidget(hunk);
                connect(hunk, &QToolButton::clicked, this, [this] { bool ok = false; const QString patch = QInputDialog::getMultiLineText(this, tr("Git hunk"), tr("完整 patch（含 diff --git 文件头）"), {}, &ok); if (!ok || patch.trimmed().isEmpty()) return; const QString action = QInputDialog::getItem(this, tr("Git hunk"), tr("操作"), {tr("接受到暂存区"), tr("拒绝已暂存块")}, 0, false, &ok); if (ok) emit diffPatchActionRequested(patch, action == tr("拒绝已暂存块")); });
                auto* conflicts = new QToolButton(page); conflicts->setText(tr("冲突")); conflicts->setToolTip(tr("显示 Git 未合并文件和冲突标记")); controls->addWidget(conflicts);
                connect(conflicts, &QToolButton::clicked, this, &CodexAgentWorkspace::diffConflictRequested);
                auto* exportPatch = new QToolButton(page); exportPatch->setText(tr("导出补丁")); exportPatch->setToolTip(tr("导出当前完整 Git Diff")); controls->addWidget(exportPatch);
                connect(exportPatch, &QToolButton::clicked, this, &CodexAgentWorkspace::diffExportRequested);
            } else if (tab.first == QStringLiteral("terminal")) {
                auto* command = new QLineEdit(page); command->setPlaceholderText(tr("PowerShell 命令")); controls->addWidget(command, 1); _workbenchInputs.insert(tab.first, command);
                auto* run = new QToolButton(page); run->setText(tr("运行")); run->setToolTip(tr("在当前项目目录执行 PowerShell 命令"));
                const auto execute = [this, command] { const QString text = command->text().trimmed(); if (!text.isEmpty()) { emit terminalCommandRequested(text); command->clear(); } };
                connect(run, &QToolButton::clicked, this, execute); connect(command, &QLineEdit::returnPressed, this, execute); controls->addWidget(run);
                auto* stop = new QToolButton(page); stop->setText(tr("停止")); stop->setToolTip(tr("停止当前终端命令"));
                connect(stop, &QToolButton::clicked, this, &CodexAgentWorkspace::terminalStopRequested); controls->addWidget(stop);
                auto* restart = new QToolButton(page); restart->setText(tr("重启")); restart->setToolTip(tr("结束并重新建立 PowerShell 会话")); controls->addWidget(restart);
                connect(restart, &QToolButton::clicked, this, &CodexAgentWorkspace::terminalRestartRequested);
                auto* history = new QToolButton(page); history->setText(tr("历史")); history->setToolTip(tr("显示已保存的终端命令历史")); controls->addWidget(history);
                connect(history, &QToolButton::clicked, this, &CodexAgentWorkspace::terminalHistoryRequested);
                auto* processStatus = new QToolButton(page); processStatus->setText(tr("进程")); processStatus->setToolTip(tr("显示终端及其子进程状态")); controls->addWidget(processStatus);
                connect(processStatus, &QToolButton::clicked, this, &CodexAgentWorkspace::terminalStatusRequested);
            } else if (tab.first == QStringLiteral("rules")) {
                auto* refresh = new QToolButton(page); refresh->setText(tr("刷新规则")); refresh->setToolTip(tr("读取 AGENTS.md、Codex 配置与运行时特性状态")); controls->addWidget(refresh);
                connect(refresh, &QToolButton::clicked, this, &CodexAgentWorkspace::rulesAndConfigRequested);
                auto* nativeRpc = new QToolButton(page); nativeRpc->setText(tr("官方协议")); nativeRpc->setToolTip(tr("调用当前 Codex app-server 暴露的官方协议方法，参数必须是 JSON 对象")); controls->addWidget(nativeRpc);
                connect(nativeRpc, &QToolButton::clicked, this, &CodexAgentWorkspace::nativeRpcConsoleRequested);
                auto* status = new QToolButton(page); status->setText(tr("运行时状态")); status->setToolTip(tr("读取账户、权限、特性与 hooks 状态")); controls->addWidget(status);
                connect(status, &QToolButton::clicked, this, &CodexAgentWorkspace::refreshRuntimeStatusRequested);
                auto* exportDiagnostics = new QToolButton(page); exportDiagnostics->setText(tr("导出诊断")); exportDiagnostics->setToolTip(tr("导出不含密钥的本地 Codex 诊断包")); controls->addWidget(exportDiagnostics);
                connect(exportDiagnostics, &QToolButton::clicked, this, &CodexAgentWorkspace::exportDiagnosticsRequested);
                auto* login = new QToolButton(page); login->setText(tr("登录")); login->setToolTip(tr("启动 Codex ChatGPT 登录流程，不显示或保存令牌")); controls->addWidget(login);
                connect(login, &QToolButton::clicked, this, &CodexAgentWorkspace::accountLoginRequested);
                auto* logout = new QToolButton(page); logout->setText(tr("退出登录")); logout->setToolTip(tr("通过 Codex app-server 清除账户凭据")); controls->addWidget(logout);
                connect(logout, &QToolButton::clicked, this, &CodexAgentWorkspace::accountLogoutRequested);
                auto* feature = new QToolButton(page); feature->setText(tr("Feature flags")); feature->setToolTip(tr("通过 app-server 设置真实 feature flags")); controls->addWidget(feature);
                connect(feature, &QToolButton::clicked, this, &CodexAgentWorkspace::featureFlagRequested);
                auto* rollbackTurn = new QToolButton(page); rollbackTurn->setText(tr("回滚会话")); rollbackTurn->setToolTip(tr("回滚当前会话最近若干轮，不回退文件")); controls->addWidget(rollbackTurn);
                connect(rollbackTurn, &QToolButton::clicked, this, &CodexAgentWorkspace::threadRollbackRequested);
                auto* edit = new QToolButton(page); edit->setText(tr("编辑配置")); edit->setToolTip(tr("编辑项目或用户 Codex config.toml，密钥字段不允许保存")); controls->addWidget(edit);
                connect(edit, &QToolButton::clicked, this, [this] { bool ok = false; const QString choice = QInputDialog::getItem(this, tr("Codex 配置"), tr("配置范围"), {tr("项目"), tr("用户")}, 0, false, &ok); if (ok) emit configEditRequested(choice == tr("用户") ? QStringLiteral("user") : QStringLiteral("project")); });
                auto* rollback = new QToolButton(page); rollback->setText(tr("回滚配置")); rollback->setToolTip(tr("恢复最近一次非密钥配置备份")); controls->addWidget(rollback);
                connect(rollback, &QToolButton::clicked, this, [this] { bool ok = false; const QString choice = QInputDialog::getItem(this, tr("回滚 Codex 配置"), tr("配置范围"), {tr("项目"), tr("用户")}, 0, false, &ok); if (ok) emit configRollbackRequested(choice == tr("用户") ? QStringLiteral("user") : QStringLiteral("project")); });
            } else if (tab.first == QStringLiteral("agents")) {
                auto* task = new QToolButton(page); task->setText(tr("创建任务")); task->setToolTip(tr("提交一个受当前 Codex 线程跟踪的任务"));
                connect(task, &QToolButton::clicked, this, [this] {
                    bool accepted = false;
                    const QString objective = QInputDialog::getText(this, tr("创建 Codex 任务"), tr("任务目标"), QLineEdit::Normal, {}, &accepted);
                    if (accepted && !objective.trimmed().isEmpty()) emit agentTaskRequested(objective.trimmed());
                }); controls->addWidget(task);
                auto* localTask = new QToolButton(page); localTask->setText(tr("后台任务")); localTask->setToolTip(tr("创建 CGPlay 本地持久化后台任务，不冒充 Codex 云任务")); controls->addWidget(localTask);
                connect(localTask, &QToolButton::clicked, this, [this] { bool ok = false; const QString objective = QInputDialog::getText(this, tr("CGPlay 后台任务"), tr("任务目标"), QLineEdit::Normal, {}, &ok); if (ok && !objective.trimmed().isEmpty()) emit localTaskRequested(objective.trimmed()); });
                auto* manageTasks = new QToolButton(page); manageTasks->setText(tr("管理队列")); manageTasks->setToolTip(tr("查看、取消或重试 CGPlay 本地后台任务")); controls->addWidget(manageTasks);
                connect(manageTasks, &QToolButton::clicked, this, &CodexAgentWorkspace::localTaskManageRequested);
                auto* orchestrate = new QToolButton(page); orchestrate->setText(tr("本地多代理")); orchestrate->setToolTip(tr("创建 cgplay.orchestrator.v1 本地任务：独立 app-server 与 Git worktree")); controls->addWidget(orchestrate);
                connect(orchestrate, &QToolButton::clicked, this, [this] { bool ok = false; const QString objective = QInputDialog::getText(this, tr("CGPlay 本地多代理"), tr("任务目标"), QLineEdit::Normal, {}, &ok); if (!ok || objective.trimmed().isEmpty()) return; const int count = QInputDialog::getInt(this, tr("CGPlay 本地多代理"), tr("代理数量"), 2, 1, 4, 1, &ok); if (ok) emit orchestratorCreateRequested(objective.trimmed(), count); });
                auto* orchManage = new QToolButton(page); orchManage->setText(tr("编排状态")); orchManage->setToolTip(tr("查看、取消、重试或合并本地多代理任务结果")); controls->addWidget(orchManage);
                connect(orchManage, &QToolButton::clicked, this, &CodexAgentWorkspace::orchestratorManageRequested);
                auto* search = new QLineEdit(page); search->setPlaceholderText(tr("搜索历史会话")); controls->addWidget(search, 1);
                connect(search, &QLineEdit::returnPressed, this, [this, search] { emit sessionSearchRequested(search->text().trimmed()); });
                auto* exportSessions = new QToolButton(page); exportSessions->setText(tr("导出会话")); exportSessions->setToolTip(tr("导出已加载会话的非密码元数据")); controls->addWidget(exportSessions);
                connect(exportSessions, &QToolButton::clicked, this, &CodexAgentWorkspace::sessionExportRequested);
                auto* archive = new QToolButton(page); archive->setText(tr("批量归档")); archive->setToolTip(tr("按已加载会话 ID 批量归档，当前会话会自动排除")); controls->addWidget(archive);
                connect(archive, &QToolButton::clicked, this, &CodexAgentWorkspace::sessionBulkArchiveRequested);
                auto* importSessions = new QToolButton(page); importSessions->setText(tr("导入会话")); importSessions->setToolTip(tr("导入完整 Codex JSONL 会话文件")); controls->addWidget(importSessions);
                connect(importSessions, &QToolButton::clicked, this, &CodexAgentWorkspace::sessionImportRequested);
                auto* deleteSessions = new QToolButton(page); deleteSessions->setText(tr("删除会话")); deleteSessions->setToolTip(tr("将非当前会话移动到可恢复的本地回收目录")); controls->addWidget(deleteSessions);
                connect(deleteSessions, &QToolButton::clicked, this, &CodexAgentWorkspace::sessionDeleteRequested);
                auto* restoreSessions = new QToolButton(page); restoreSessions->setText(tr("批量恢复")); restoreSessions->setToolTip(tr("通过 Codex app-server 批量取消归档")); controls->addWidget(restoreSessions);
                connect(restoreSessions, &QToolButton::clicked, this, &CodexAgentWorkspace::sessionBulkRestoreRequested);
            }
            controls->addStretch(1);
            pageLayout->addLayout(controls);
            auto* view = new QPlainTextEdit(page);
            view->setReadOnly(true);
            configureBoundedTextView(view, 1000);
            view->setPlaceholderText(tr("等待真实 Codex 事件..."));
            pageLayout->addWidget(view, 1);
            _workbenchViews.insert(tab.first, view);
            _workbenchTabs->addTab(page, tab.second);
        }
        layout->addWidget(_workbenchTabs);
    }
    refreshWorkbench();
    _workbenchDialog->show();
    _workbenchDialog->raise();
    _workbenchDialog->activateWindow();
}

void CodexAgentWorkspace::refreshWorkbench()
{
    for (auto it = _workbenchViews.cbegin(); it != _workbenchViews.cend(); ++it) {
        if (!it.value()) continue;
        const QString prefix = QStringLiteral("[%1]").arg(it.key());
        QStringList rows;
        for (const QString& row : _workbenchEvents) {
            if (row.startsWith(prefix) || (it.key() == QStringLiteral("agents") && row.startsWith(QStringLiteral("[task]")))) {
                rows.push_back(row);
            }
        }
        it.value()->setPlainText(rows.join(QLatin1Char('\n')));
    }
}

void CodexAgentWorkspace::addWorkbenchRow(const QString& category, const QString& text)
{
    const QString row = QStringLiteral("[%1] %2").arg(category, text);
    _workbenchEvents.push_back(row);
    while (_workbenchEvents.size() > 1000) _workbenchEvents.removeFirst();
    if (QPlainTextEdit* view = _workbenchViews.value(category)) view->appendPlainText(row);
    if (category != QStringLiteral("terminal")) {
        if (QPlainTextEdit* all = _workbenchViews.value(QStringLiteral("agents"))) {
            if (category == QStringLiteral("task")) all->appendPlainText(row);
        }
    }
}

void CodexAgentWorkspace::setError(const QString& error)
{
    _errorLabel->setText(error);
    _errorLabel->setVisible(!error.trimmed().isEmpty());
    if (!error.trimmed().isEmpty()) {
        setStatusText(tr("失败"), false, true);
        if (_taskStatusLabel) _taskStatusLabel->setText(tr("失败"));
    }
}

void CodexAgentWorkspace::beginNewConversation()
{
    clearConversationWidgets();
    _titleLabel->setText(tr("新会话"));
    _hasUserMessage = false;
    updateEmptyState();
}

void CodexAgentWorkspace::markPromptAccepted()
{
    ensureAssistantMessage();
    ensureTaskCard();
    QLabel* step = ensureTaskStep(QStringLiteral("request"));
    step->setText(tr("✓ 请求已发送"));
    _taskStatusLabel->setText(tr("已发送"));
    scrollToBottom();
}

void CodexAgentWorkspace::markTurnStarted()
{
    ensureAssistantMessage();
    ensureTaskCard();
    QLabel* step = ensureTaskStep(QStringLiteral("turn"));
    step->setText(tr("• Codex 正在处理"));
    _taskStatusLabel->setText(tr("处理中"));
    scrollToBottom();
}

void CodexAgentWorkspace::updateTaskPlan(const QJsonArray& plan, const QString& explanation)
{
    if (plan.isEmpty() && explanation.trimmed().isEmpty()) return;
    ensureAssistantMessage();
    ensureTaskCard();
    if (!explanation.trimmed().isEmpty()) _taskToggle->setToolTip(explanation.trimmed());
    for (qsizetype index = 0; index < plan.size(); ++index) {
        const QJsonObject item = plan.at(index).toObject();
        const QString text = item.value(QStringLiteral("step")).toString().trimmed();
        if (text.isEmpty()) continue;
        const QString status = item.value(QStringLiteral("status")).toString();
        const QString prefix = status == QStringLiteral("completed")
            ? QStringLiteral("✓ ")
            : (status == QStringLiteral("inProgress") ? QStringLiteral("• ") : QStringLiteral("○ "));
        ensureTaskStep(QStringLiteral("plan:%1").arg(index))->setText(prefix + text);
    }
    _taskStatusLabel->setText(tr("计划已更新"));
    scrollToBottom();
}

void CodexAgentWorkspace::markTaskActivity(const QString& activityId, const QString& label, bool completed)
{
    if (activityId.trimmed().isEmpty() || label.trimmed().isEmpty()) return;
    ensureAssistantMessage();
    ensureTaskCard();
    ensureTaskStep(QStringLiteral("activity:%1").arg(activityId))->setText(
        (completed ? QStringLiteral("✓ ") : QStringLiteral("• ")) + label.trimmed());
    _taskStatusLabel->setText(completed ? tr("步骤完成") : tr("执行中"));
    scrollToBottom();
}

void CodexAgentWorkspace::completeTurn(const QString& status)
{
    if (_assistantTextLabel) appendDetectedFileArtifacts(_assistantTextLabel->text());
    if (_assistantMetricsLabel) {
        QJsonObject last = _latestTokenUsage.value(QStringLiteral("last")).toObject();
        if (last.isEmpty()) {
            last = _latestTokenUsage.value(QStringLiteral("lastTokenUsage")).toObject();
        }
        const auto value = [&last](const char* key) { return last.contains(QString::fromLatin1(key)) ? QString::number(last.value(QString::fromLatin1(key)).toInteger()) : QObject::tr("未提供"); };
        const qint64 cached = last.value(QStringLiteral("cachedInputTokens")).toInteger(-1);
        const qint64 input = last.value(QStringLiteral("inputTokens")).toInteger(-1);
        const QString cacheRate = cached >= 0 && input > 0 ? QString::number(100.0 * cached / input, 'f', 1) + QLatin1Char('%') : tr("未提供");
        const qint64 limit = _latestTokenUsage.value(QStringLiteral("modelContextWindow")).toInteger(-1);
        _assistantMetricsLabel->setText(tr("输入 %1  ·  输出 %2  ·  总计 %3  ·  缓存 %4 (%5)  ·  上下文 %6 / %7")
            .arg(value("inputTokens"), value("outputTokens"), value("totalTokens"), value("cachedInputTokens"), cacheRate,
                 input >= 0 ? QString::number(input) : tr("未提供"), limit > 0 ? QString::number(limit) : tr("未提供")));
        _assistantMetricsLabel->show();
    }
    if (_taskStatusLabel) _taskStatusLabel->setText(turnStatusText(status));
    if (QLabel* step = _taskSteps.value(QStringLiteral("turn"))) {
        step->setText(status == QStringLiteral("completed")
            ? tr("✓ Codex 处理完成")
            : tr("! Codex 本轮%1").arg(turnStatusText(status)));
    }
    _assistantTextLabel = nullptr;
    _assistantMetricsLabel = nullptr;
    _assistantBodyLayout = nullptr;
    _taskCard = nullptr;
    _taskToggle = nullptr;
    _taskStatusLabel = nullptr;
    _taskDetails = nullptr;
    _taskDetailsLayout = nullptr;
    _taskSteps.clear();
    _turnArtifactPaths.clear();
    scrollToBottom();
}

void CodexAgentWorkspace::appendDetectedFileArtifacts(const QString& text)
{
    static const QRegularExpression filePathPattern(
        QStringLiteral("([A-Za-z]:[\\/][^\\r\\n`\"<>|]+?\\.[A-Za-z0-9]{1,12})"));
    const QString projectRoot = _projectPath.trimmed().isEmpty()
        ? QString()
        : QDir::fromNativeSeparators(QDir(_projectPath).absolutePath());
    QRegularExpressionMatchIterator matches = filePathPattern.globalMatch(text);
    while (matches.hasNext()) {
        const QFileInfo file(QDir::fromNativeSeparators(matches.next().captured(1).trimmed()));
        if (!file.isFile() || QImageReader::supportedImageFormats().contains(file.suffix().toLower().toLatin1())) continue;
        const QString absolutePath = QDir::fromNativeSeparators(file.absoluteFilePath());
        if (!projectRoot.isEmpty() && absolutePath != projectRoot &&
            !absolutePath.startsWith(projectRoot + QLatin1Char('/'), Qt::CaseInsensitive)) continue;
        appendFileArtifact(absolutePath);
    }
}

void CodexAgentWorkspace::setTurnInProgress(bool active)
{
    _turnInProgress = active;
    setProperty("codexTurnInProgress", active);
    _promptEdit->setPlaceholderText(active
        ? tr("补充要求或引导当前任务")
        : tr("向 Codex 发送消息"));
    refreshActionState();
}

void CodexAgentWorkspace::setTokenUsage(const QJsonObject& tokenUsage)
{
    _latestTokenUsage = tokenUsage;
}

void CodexAgentWorkspace::updateImageGeneration(const QJsonObject& item, bool completed)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QString status = item.value(QStringLiteral("status")).toString();
    if (!completed) {
        markTaskActivity(id, tr("Generating image"), false);
        return;
    }

    ensureAssistantMessage();
    const QString savedPath = QFileInfo(item.value(QStringLiteral("savedPath")).toString()).absoluteFilePath();
    QImageReader reader(savedPath);
    const QImage image = reader.read();
    if (status != QStringLiteral("completed") || savedPath.isEmpty() || image.isNull()) {
        const QString reason = item.value(QStringLiteral("result")).toString().trimmed();
        markTaskActivity(id, reason.isEmpty() ? tr("Image generation failed") : tr("Image generation failed: %1").arg(reason), true);
        return;
    }

    auto* preview = new QLabel(_conversationHost);
    preview->setObjectName(QStringLiteral("CodexGeneratedImage"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setPixmap(QPixmap::fromImage(image).scaled(420, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    preview->setToolTip(savedPath);
    preview->setCursor(Qt::PointingHandCursor);
    preview->setProperty("codexGeneratedImagePath", savedPath);
    preview->installEventFilter(this);
    _assistantBodyLayout->addWidget(preview);
    setProperty("codexLatestGeneratedImage", savedPath);

    const QString prompt = item.value(QStringLiteral("revisedPrompt")).toString().trimmed();
    if (!prompt.isEmpty()) {
        auto* caption = new QLabel(prompt, _conversationHost);
        caption->setObjectName(QStringLiteral("CodexImageCaption"));
        caption->setWordWrap(true);
        caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _assistantBodyLayout->addWidget(caption);
    }
    auto* importButton = new QToolButton(_conversationHost);
    importButton->setObjectName(QStringLiteral("CodexImageImport"));
    importButton->setText(tr("Import into CGPlay"));
    importButton->setToolTip(savedPath);
    connect(importButton, &QToolButton::clicked, this, [this, savedPath] {
        emit importGeneratedImageRequested(savedPath);
    });
    _assistantBodyLayout->addWidget(importButton, 0, Qt::AlignLeft);
    auto* saveButton = new QToolButton(_conversationHost);
    saveButton->setObjectName(QStringLiteral("CodexImageSave"));
    saveButton->setText(tr("另存为"));
    saveButton->setToolTip(tr("将原始图片保存到指定位置"));
    connect(saveButton, &QToolButton::clicked, this, [this, savedPath] {
        const QFileInfo source(savedPath);
        const QString suggested = QDir::home().filePath(source.fileName());
        const QString destination = chooseSavePath(
            this,
            tr("另存生成图片"),
            suggested,
            tr("PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg);;WebP 图片 (*.webp);;所有文件 (*.*)"));
        if (destination.isEmpty()) return;
        if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
            setError(tr("无法覆盖目标文件：%1").arg(destination));
            return;
        }
        if (!QFile::copy(savedPath, destination)) setError(tr("另存图片失败：%1").arg(destination));
    });
    _assistantBodyLayout->addWidget(saveButton, 0, Qt::AlignLeft);
    markTaskActivity(id, tr("Image generated"), true);
    scrollToBottom();
}

void CodexAgentWorkspace::appendFileArtifact(const QString& absolutePath, const QString& label)
{
    const QFileInfo source(absolutePath);
    const QString canonical = source.canonicalFilePath();
    if (canonical.isEmpty() || !source.isFile() || _turnArtifactPaths.contains(canonical)) return;
    _turnArtifactPaths.insert(canonical);
    ensureAssistantMessage();

    auto* artifact = new QFrame(_conversationHost);
    artifact->setObjectName(QStringLiteral("CodexFileArtifact"));
    artifact->setProperty("codexFileArtifactPath", canonical);
    auto* layout = new QHBoxLayout(artifact);
    layout->setContentsMargins(9, 7, 7, 7);
    layout->setSpacing(8);

    auto* icon = new QLabel(artifact);
    icon->setPixmap(style()->standardIcon(QStyle::SP_FileIcon).pixmap(22, 22));
    icon->setFixedSize(24, 24);
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto* textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(1);
    auto* name = new QLabel(label.trimmed().isEmpty() ? source.fileName() : label.trimmed(), artifact);
    name->setObjectName(QStringLiteral("CodexFileName"));
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    name->setToolTip(canonical);
    textLayout->addWidget(name);
    auto* meta = new QLabel(tr("%1 KB · %2").arg(qMax<qint64>(1, (source.size() + 1023) / 1024)).arg(source.suffix().toUpper()), artifact);
    meta->setObjectName(QStringLiteral("CodexFileMeta"));
    textLayout->addWidget(meta);
    layout->addLayout(textLayout, 1);

    auto* openButton = new QToolButton(artifact);
    openButton->setObjectName(QStringLiteral("CodexFileOpen"));
    openButton->setText(tr("打开"));
    openButton->setToolTip(canonical);
    connect(openButton, &QToolButton::clicked, this, [canonical] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(canonical));
    });
    layout->addWidget(openButton);

    auto* saveButton = new QToolButton(artifact);
    saveButton->setObjectName(QStringLiteral("CodexFileSaveAs"));
    saveButton->setText(tr("另存为"));
    connect(saveButton, &QToolButton::clicked, this, [this, canonical] {
        const QFileInfo info(canonical);
        QString destination = property("codexFileSaveAsTestPath").toString();
        if (destination.isEmpty()) {
            destination = chooseSavePath(
                this,
                tr("另存生成文件"),
                QDir::home().filePath(info.fileName()),
                tr("所有文件 (*.*)"));
        }
        if (destination.isEmpty() || QFileInfo(destination).absoluteFilePath() == canonical) return;
        if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
            setError(tr("无法覆盖目标文件：%1").arg(destination));
            return;
        }
        if (!QFile::copy(canonical, destination)) {
            setError(tr("另存文件失败：%1").arg(destination));
        }
    });
    layout->addWidget(saveButton);

    _assistantBodyLayout->addWidget(artifact);
    setProperty("codexLatestFileArtifact", canonical);
    scrollToBottom();
}

void CodexAgentWorkspace::setSessionList(const QJsonArray& threads, const QString& currentThreadId)
{
    _sessionSnapshot = threads;
    _sessionsMenu->clear();
    QAction* refresh = _sessionsMenu->addAction(tr("刷新会话"));
    connect(refresh, &QAction::triggered, this, &CodexAgentWorkspace::sessionListRequested);
    _sessionsMenu->addSeparator();
    if (threads.isEmpty()) {
        QAction* empty = _sessionsMenu->addAction(tr("没有可恢复的会话"));
        empty->setEnabled(false);
    }
    for (const QJsonValue& value : threads) {
        const QJsonObject thread = value.toObject();
        const QString id = thread.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        QString title = thread.value(QStringLiteral("name")).toString().trimmed();
        if (title.isEmpty()) title = thread.value(QStringLiteral("preview")).toString().simplified();
        if (title.isEmpty()) title = id.left(12);
        if (title.size() > 36) title = title.left(35) + QChar(0x2026);
        QMenu* menu = _sessionsMenu->addMenu((id == currentThreadId ? QStringLiteral("● ") : QString()) + title);
        QAction* read = menu->addAction(tr("查看历史"));
        QAction* resume = menu->addAction(tr("恢复"));
        QAction* fork = menu->addAction(tr("从此分叉"));
        QAction* archive = menu->addAction(tr("归档"));
        const bool mutableAction = _sessionActionsEnabled && id != currentThreadId;
        read->setEnabled(_sessionActionsEnabled);
        resume->setEnabled(mutableAction);
        fork->setEnabled(_sessionActionsEnabled);
        archive->setEnabled(mutableAction);
        const QString reason = _sessionActionsEnabled ? tr("当前会话无需重复操作") : _sessionDisabledReason;
        resume->setToolTip(reason);
        archive->setToolTip(reason);
        connect(read, &QAction::triggered, this, [this, id] { emit sessionReadRequested(id); });
        connect(resume, &QAction::triggered, this, [this, id] { emit sessionResumeRequested(id); });
        connect(fork, &QAction::triggered, this, [this, id] { emit sessionForkRequested(id); });
        connect(archive, &QAction::triggered, this, [this, id] { emit sessionArchiveRequested(id); });
    }
}

void CodexAgentWorkspace::showThreadHistory(const QJsonObject& thread)
{
    clearConversationWidgets();
    _hasUserMessage = false;
    const QString name = thread.value(QStringLiteral("name")).toString().trimmed();
    _titleLabel->setText(name.isEmpty() ? tr("会话历史") : name);
    const auto itemText = [](const QJsonObject& item) {
        QString text = item.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) text = item.value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) return text;
        QStringList parts;
        for (const QJsonValue& value : item.value(QStringLiteral("content")).toArray()) {
            const QJsonObject part = value.toObject();
            QString partText = part.value(QStringLiteral("text")).toString();
            if (partText.isEmpty()) partText = part.value(QStringLiteral("content")).toString();
            if (!partText.trimmed().isEmpty()) parts.push_back(partText);
        }
        return parts.join(QLatin1Char('\n'));
    };
    const auto compactValue = [](const QJsonValue& value) {
        QByteArray json;
        if (value.isObject()) json = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
        else if (value.isArray()) json = QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
        else if (value.isString()) return value.toString();
        QString text = QString::fromUtf8(json);
        if (text.size() > 1200) text = text.left(1200) + QChar(0x2026);
        return text;
    };
    const QJsonArray turns = thread.value(QStringLiteral("turns")).toArray();
    for (const QJsonValue& turnValue : turns) {
        const QJsonArray items = turnValue.toObject().value(QStringLiteral("items")).toArray();
        for (const QJsonValue& itemValue : items) {
            const QJsonObject item = itemValue.toObject();
            const QString type = item.value(QStringLiteral("type")).toString();
            const QString text = itemText(item);
            if (type == QStringLiteral("userMessage")) {
                QStringList imagePaths;
                for (const QJsonValue& value : item.value(QStringLiteral("localImages")).toArray()) {
                    const QString path = value.toString();
                    if (QFileInfo(path).isFile()) imagePaths.push_back(path);
                }
                appendUserMessage(text, imagePaths);
            }
            else if (type == QStringLiteral("agentMessage")) { ensureAssistantMessage(); appendStreamText(text); }
            else if (type == QStringLiteral("imageGeneration")) updateImageGeneration(item, true);
            else if (type == QStringLiteral("fileArtifact")) appendFileArtifact(item.value(QStringLiteral("path")).toString());
            else if (type == QStringLiteral("reasoning")) {
                markTaskActivity(item.value(QStringLiteral("id")).toString(), tr("推理已完成"), true);
            } else {
                QString label = type;
                if (type == QStringLiteral("dynamicToolCall") || type == QStringLiteral("toolCall")) {
                    label = tr("工具调用：%1").arg(item.value(QStringLiteral("tool")).toString(
                        item.value(QStringLiteral("name")).toString()));
                } else if (type == QStringLiteral("commandExecution")) label = tr("命令执行");
                else if (type == QStringLiteral("fileChange")) label = tr("文件更改");
                else if (type == QStringLiteral("webSearch")) label = tr("网页搜索");
                QString detail = text.trimmed();
                if (detail.isEmpty()) detail = compactValue(item.value(QStringLiteral("result")));
                if (detail.isEmpty()) detail = compactValue(item.value(QStringLiteral("output")));
                if (detail.isEmpty()) detail = compactValue(item.value(QStringLiteral("arguments")));
                appendStreamText(detail.isEmpty() ? label : label + QStringLiteral("\n") + detail);
            }
        }
        if (_assistantBodyLayout) completeTurn(QStringLiteral("completed"));
    }
    updateEmptyState();
    scrollToBottom();
}

void CodexAgentWorkspace::setSessionActionsEnabled(bool enabled, const QString& reason)
{
    _sessionActionsEnabled = enabled;
    _sessionDisabledReason = reason;
    _sessionsButton->setEnabled(_connected);
    _sessionsButton->setToolTip(enabled ? tr("会话") : reason);
}

bool CodexAgentWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    const bool promptDropTarget = watched == _promptEdit || watched == _promptEdit->viewport();
    if (promptDropTarget && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)) {
        auto* dragEvent = static_cast<QDropEvent*>(event);
        const QMimeData* mime = dragEvent->mimeData();
        bool hasImage = false;
        for (const QUrl& url : mime->urls()) {
            const QFileInfo file(url.toLocalFile());
            if (file.isFile() && QImageReader::supportedImageFormats().contains(file.suffix().toLower().toLatin1())) {
                hasImage = true;
                break;
            }
        }
        if (hasImage) {
            dragEvent->acceptProposedAction();
            return true;
        }
    }
    if (promptDropTarget && event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const QUrl& url : dropEvent->mimeData()->urls()) {
            const QFileInfo file(url.toLocalFile());
            if (file.isFile() && QImageReader::supportedImageFormats().contains(file.suffix().toLower().toLatin1())) {
                paths.push_back(file.absoluteFilePath());
            }
        }
        if (!paths.isEmpty()) {
            dropEvent->acceptProposedAction();
            addImageAttachments(paths);
            _promptEdit->setPlainText(_promptEdit->toPlainText().replace(
                QRegularExpression(QStringLiteral("(?:^|\\n)?file:///[^\\r\\n]+")), QString()).trimmed());
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto* preview = qobject_cast<QLabel*>(watched)) {
            const QString userPath = preview->property("codexUserImagePath").toString();
            if (!userPath.isEmpty()) {
                showImagePreview(userPath);
                return true;
            }
            const QString path = preview->property("codexGeneratedImagePath").toString();
            if (!path.isEmpty()) {
                emit importGeneratedImageRequested(path);
                return true;
            }
        }
    }
    if (watched == _promptEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->matches(QKeySequence::Paste) && pasteClipboardImages()) {
            return true;
        }
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
            !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            submitPrompt();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CodexAgentWorkspace::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || _applyingTheme) return;
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        refreshTheme();
    }
}

void CodexAgentWorkspace::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] { emit activationRequested(); });
}

void CodexAgentWorkspace::submitPrompt()
{
    const QString prompt = _promptEdit->toPlainText().trimmed();
    if ((prompt.isEmpty() && _attachedImagePaths.isEmpty()) || !_promptEnabled) return;
    if (_turnInProgress) {
        _assistantTextLabel = nullptr;
        _assistantMetricsLabel = nullptr;
        _assistantBodyLayout = nullptr;
        _taskCard = nullptr;
        _taskToggle = nullptr;
        _taskStatusLabel = nullptr;
        _taskDetails = nullptr;
        _taskDetailsLayout = nullptr;
        _taskSteps.clear();
    }
    if (!_attachedImagePaths.isEmpty()) {
        const QString displayPrompt = prompt.isEmpty() ? tr("图片") : prompt;
        updateConversationTitle(displayPrompt);
        const QStringList paths = _attachedImagePaths;
        appendUserMessage(displayPrompt, paths);
        _attachedImagePaths.clear();
        refreshImageAttachments();
        _promptEdit->clear();
        emit imagesSubmitted(paths, prompt);
        return;
    }
    updateConversationTitle(prompt);
    appendUserMessage(prompt);
    _promptEdit->clear();
    emit promptSubmitted(prompt);
}

void CodexAgentWorkspace::refreshActionState()
{
    const bool modelReady = !selectedModel().isEmpty();
    _promptEdit->setEnabled(_promptEnabled);
    _modelCombo->setEnabled(!_turnInProgress && _modelCombo->count() > 0 && (_promptEnabled || !_connected));
    _reasoningButton->setEnabled(
        !_turnInProgress && _reasoningOptionsAvailable && (_promptEnabled || !_connected));
    _approvalButton->setEnabled(!_turnInProgress && _aiConfigured && _runtimeConfigured && (_promptEnabled || !_connected));
    _newConversationButton->setEnabled(_connected && _promptEnabled && !_turnInProgress);
    _headerNewConversationButton->setEnabled(_connected && _promptEnabled && !_turnInProgress);
    _sessionsButton->setEnabled(_connected);
    _projectButton->setEnabled(!_projectPath.isEmpty());
    if (_turnInProgress) {
        _sendButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        _sendButton->setText(QString());
        _sendButton->setToolTip(tr("停止当前任务"));
        _sendButton->setEnabled(true);
    } else {
        _sendButton->setIcon(QIcon());
        _sendButton->setText(QStringLiteral("↑"));
        _sendButton->setToolTip(tr("发送"));
        _sendButton->setEnabled(_promptEnabled && modelReady &&
            (!_promptEdit->toPlainText().trimmed().isEmpty() || !_attachedImagePaths.isEmpty()));
    }
    _retryButton->setEnabled(_retryAvailable);
    _stopAction->setEnabled(_turnInProgress);
    const WorkspaceTheme theme = currentWorkspaceTheme();
    const QString chromeText = QStringLiteral("#F4F7FB");
    const QString secondaryText = QStringLiteral("#AEB8C5");
    const QString headerStyle = QStringLiteral(
        "QToolButton{background:transparent;color:%1;border:0;padding:4px;font-size:14px;font-weight:600;}"
        "QToolButton:hover{background:%2;color:%1;border:0;}"
        "QToolButton:pressed{background:%3;color:#FFFFFF;border:0;}"
        "QToolButton:disabled{background:transparent;color:%1;border:0;}"
    ).arg(chromeText, theme.hover, theme.accent);
    for (QToolButton* button : {_headerNewConversationButton, _sessionsButton, _overflowButton, _closeButton}) {
        if (button) button->setStyleSheet(headerStyle);
    }
    const QString composerStyle = QStringLiteral(
        "QToolButton{background:%1;color:%2;border:1px solid %3;border-radius:6px;}"
        "QToolButton:hover{background:%4;color:%2;}"
        "QToolButton:pressed{background:%5;color:%6;}"
        "QToolButton:disabled{background:%1;color:%2;border:1px solid %3;}"
    ).arg(theme.raised, theme.text, theme.border, theme.hover, theme.accent, theme.accentText);
    for (QToolButton* button : {_approvalButton, _reasoningButton, _sendButton, _projectButton, _newConversationButton}) {
        if (!button) continue;
        button->setStyleSheet(composerStyle);
        QPalette palette = button->palette();
        palette.setColor(QPalette::Button, theme.raised);
        palette.setColor(QPalette::ButtonText, theme.text);
        palette.setColor(QPalette::Disabled, QPalette::Button, theme.raised);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, theme.text);
        button->setPalette(palette);
    }
    const QString menuStyle = QStringLiteral(
        "QMenu{background:%1;color:%2;border:1px solid %3;padding:5px;font-size:12px;font-weight:600;}"
        "QMenu::item{color:%2;background:transparent;padding:8px 12px;border-radius:4px;}"
        "QMenu::item:selected{color:#FFFFFF;background:%4;}"
        "QMenu::item:checked{color:#FFFFFF;background:%4;}"
        "QMenu::item:disabled{color:%5;background:transparent;}"
        "QMenu::separator{height:1px;background:%3;margin:4px 8px;}"
    ).arg(theme.surface, chromeText, theme.border, theme.accent, secondaryText);
    for (QMenu* menu : {_approvalMenu, _reasoningMenu, _sessionsMenu, _overflowMenu}) {
        if (!menu) continue;
        menu->setStyleSheet(menuStyle);
        QPalette palette = menu->palette();
        palette.setColor(QPalette::Window, QColor(theme.surface));
        palette.setColor(QPalette::Text, QColor(chromeText));
        palette.setColor(QPalette::WindowText, QColor(chromeText));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(secondaryText));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(secondaryText));
        palette.setColor(QPalette::Highlight, QColor(theme.accent));
        palette.setColor(QPalette::HighlightedText, QColor(Qt::white));
        menu->setPalette(palette);
    }
}

void CodexAgentWorkspace::addImageAttachments(const QStringList& absolutePaths)
{
    for (const QString& path : absolutePaths) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && !_attachedImagePaths.contains(canonical)) _attachedImagePaths.push_back(canonical);
    }
    refreshImageAttachments();
    refreshActionState();
}

bool CodexAgentWorkspace::pasteClipboardImages()
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime) return false;

    QStringList paths;
    for (const QUrl& url : mime->urls()) {
        const QFileInfo file(url.toLocalFile());
        if (file.isFile() && QImageReader::supportedImageFormats().contains(file.suffix().toLower().toLatin1())) {
            paths.push_back(file.absoluteFilePath());
        }
    }

    if (paths.isEmpty() && mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                          .filePath(QStringLiteral("codex_attachments"));
            QDir().mkpath(directory);
            const QString path = QDir(directory).filePath(QStringLiteral("clipboard_%1.png").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (!image.save(path, "PNG")) {
                setError(tr("无法保存剪贴板图片。"));
                return true;
            }
            paths.push_back(path);
        }
    }

    if (paths.isEmpty()) return false;
    addImageAttachments(paths);
    return true;
}

void CodexAgentWorkspace::refreshImageAttachments()
{
    if (!_attachmentLayout) return;
    while (QLayoutItem* item = _attachmentLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QWidget* host = _attachmentLayout->parentWidget();
    for (const QString& path : _attachedImagePaths) {
        auto* button = new QToolButton(host);
        button->setObjectName(QStringLiteral("CodexImageAttachment"));
        button->setFixedSize(54, 54);
        button->setToolTip(QFileInfo(path).fileName());
        QPixmap image(path);
        button->setIcon(QIcon(image.scaled(48, 48, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
        button->setIconSize(QSize(48, 48));
        connect(button, &QToolButton::clicked, this, [this, path] {
            _attachedImagePaths.removeAll(path);
            refreshImageAttachments();
            refreshActionState();
        });
        _attachmentLayout->addWidget(button);
    }
    _attachmentLayout->addStretch(1);
    host->setVisible(!_attachedImagePaths.isEmpty());
}

void CodexAgentWorkspace::showImagePreview(const QString& absolutePath)
{
    QImageReader reader(absolutePath);
    const QImage image = reader.read();
    if (image.isNull()) return;
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QFileInfo(absolutePath).fileName());
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    auto* preview = new QLabel(dialog);
    preview->setAlignment(Qt::AlignCenter);
    const QSize maxSize(qMax(480, width() - 80), qMax(360, height() - 100));
    preview->setPixmap(QPixmap::fromImage(image).scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(preview);
    dialog->resize(preview->pixmap(Qt::ReturnByValue).size() + QSize(24, 24));
    dialog->exec();
}

QString CodexAgentWorkspace::selectedModel() const
{
    return _modelCombo ? _modelCombo->currentData().toString().trimmed() : QString();
}

QString CodexAgentWorkspace::selectedReasoningEffort() const
{
    for (auto it = _reasoningActions.cbegin(); it != _reasoningActions.cend(); ++it) {
        if (it.value() && it.value()->isChecked()) return it.key();
    }
    return {};
}

QString CodexAgentWorkspace::selectedApprovalMode() const
{
    return _approvalMode;
}

QString CodexAgentWorkspace::approvalDisplayName(const QString& mode) const
{
    if (mode == QStringLiteral("request")) return tr("请求批准");
    if (mode == QStringLiteral("auto")) return tr("替我审批");
    return tr("完全访问权限");
}

QString CodexAgentWorkspace::reasoningEffortDisplayName(const QString& effort) const
{
    const QString normalized = effort.trimmed().toLower();
    if (normalized == QStringLiteral("low")) return tr("轻度");
    if (normalized == QStringLiteral("medium")) return tr("中");
    if (normalized == QStringLiteral("high")) return tr("高");
    if (normalized == QStringLiteral("xhigh")) return tr("极高");
    if (normalized == QStringLiteral("max")) return tr("最高");
    if (normalized == QStringLiteral("ultra")) return tr("超强");
    return effort.trimmed();
}

void CodexAgentWorkspace::chooseApprovalMode(const QString& mode)
{
    if (!_approvalActions.contains(mode)) return;
    const bool changed = _approvalMode != mode;
    setApprovalMode(mode);
    if (changed) emit approvalModeChanged(mode);
}

void CodexAgentWorkspace::chooseReasoningEffort(const QString& effort)
{
    if (!_reasoningActions.contains(effort)) return;
    for (auto it = _reasoningActions.cbegin(); it != _reasoningActions.cend(); ++it) {
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(it.key() == effort);
    }
    _reasoningButton->setText(reasoningEffortDisplayName(effort));
    _reasoningButton->setToolTip(tr("推理强度：%1").arg(reasoningEffortDisplayName(effort)));
    emit reasoningEffortChanged(effort);
}

void CodexAgentWorkspace::setStatusText(const QString& status, bool connected, bool failed)
{
    // Keep the connection indicator readable without turning the whole right
    // workbench red.  Detailed failures remain in the dedicated CodexError
    // label, which is the only intentionally red surface.
    const QString color = connected ? QString::fromLatin1(kReady)
        : appThemeColor("cgplay.textColor", QColor(QStringLiteral("#D8DEE7"))).name(QColor::HexArgb);
    _statusLabel->setText(QStringLiteral("● %1 · %2").arg(status, tr("当前项目")));
    _statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:10px;").arg(color));
}

void CodexAgentWorkspace::setDiagnosticsVisible(bool visible)
{
    if (_diagnosticsFrame->isVisible() == visible) return;
    _diagnosticsFrame->setVisible(visible);
    _diagnosticsButton->setProperty("active", visible);
    _diagnosticsButton->style()->unpolish(_diagnosticsButton);
    _diagnosticsButton->style()->polish(_diagnosticsButton);
}

void CodexAgentWorkspace::appendUserMessage(const QString& text, const QStringList& imagePaths)
{
    QString displayText = text;
    displayText.remove(QRegularExpression(QStringLiteral("\\s*<image\\s+[^>]*>\\s*</image>"),
                                          QRegularExpression::CaseInsensitiveOption));
    displayText = displayText.trimmed();
    if (displayText.isEmpty()) displayText = tr("图片");
    _emptyStateLabel->hide();
    auto* row = new QFrame(_conversationHost);
    row->setObjectName(QStringLiteral("CodexUserMessage"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    rowLayout->addStretch(1);
    auto* bubble = new QFrame(row);
    bubble->setObjectName(QStringLiteral("CodexUserBubble"));
    bubble->setMaximumWidth(360);
    auto* bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(7, 7, 7, 4);
    bubbleLayout->setSpacing(6);
    for (const QString& path : imagePaths) {
        QImageReader reader(path);
        const QImage image = reader.read();
        if (image.isNull()) continue;
        auto* preview = new QLabel(bubble);
        preview->setObjectName(QStringLiteral("CodexUserImage"));
        preview->setAlignment(Qt::AlignCenter);
        preview->setPixmap(QPixmap::fromImage(image).scaled(280, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        preview->setToolTip(QFileInfo(path).fileName());
        preview->setCursor(Qt::PointingHandCursor);
        preview->setProperty("codexUserImagePath", path);
        preview->installEventFilter(this);
        bubbleLayout->addWidget(preview);
    }
    auto* label = new QLabel(displayText, bubble);
    label->setObjectName(QStringLiteral("CodexUserText"));
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    bubbleLayout->addWidget(label);
    rowLayout->addWidget(bubble);
    _conversationLayout->insertWidget(qMax(0, _conversationLayout->count() - 1), row);
    scrollToBottom();
}

void CodexAgentWorkspace::ensureAssistantMessage()
{
    if (_assistantBodyLayout) return;
    _emptyStateLabel->hide();
    auto* row = new QFrame(_conversationHost);
    row->setObjectName(QStringLiteral("CodexAssistantMessage"));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(10);
    auto* avatar = new QLabel(QStringLiteral("C"), row);
    avatar->setObjectName(QStringLiteral("CodexAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(26, 26);
    rowLayout->addWidget(avatar, 0, Qt::AlignTop);
    auto* body = new QWidget(row);
    _assistantBodyLayout = new QVBoxLayout(body);
    _assistantBodyLayout->setContentsMargins(0, 0, 0, 0);
    _assistantBodyLayout->setSpacing(7);
    auto* who = new QLabel(tr("Codex"), body);
    who->setObjectName(QStringLiteral("CodexWho"));
    _assistantBodyLayout->addWidget(who);
    _assistantTextLabel = new QLabel(body);
    _assistantTextLabel->setObjectName(QStringLiteral("CodexAssistantText"));
    _assistantTextLabel->setTextFormat(Qt::PlainText);
    _assistantTextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _assistantTextLabel->setWordWrap(true);
    _assistantTextLabel->hide();
    _assistantBodyLayout->addWidget(_assistantTextLabel);
    _assistantMetricsLabel = new QLabel(body);
    _assistantMetricsLabel->setObjectName(QStringLiteral("CodexMetricsFooter"));
    _assistantMetricsLabel->setWordWrap(true);
    _assistantMetricsLabel->setToolTip(tr("本轮真实 API / app-server usage；服务商未返回的字段显示未提供"));
    _assistantMetricsLabel->hide();
    _assistantBodyLayout->addWidget(_assistantMetricsLabel);
    rowLayout->addWidget(body, 1);
    _conversationLayout->insertWidget(qMax(0, _conversationLayout->count() - 1), row);
}

void CodexAgentWorkspace::ensureTaskCard()
{
    if (_taskCard) return;
    ensureAssistantMessage();
    _taskCard = new QFrame(_conversationHost);
    _taskCard->setObjectName(QStringLiteral("CodexTaskCard"));
    auto* layout = new QVBoxLayout(_taskCard);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QWidget(_taskCard);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);
    _taskToggle = new QToolButton(header);
    _taskToggle->setObjectName(QStringLiteral("CodexTaskHeader"));
    _taskToggle->setText(tr("任务进展"));
    _taskToggle->setCheckable(true);
    _taskToggle->setChecked(true);
    _taskToggle->setArrowType(Qt::DownArrow);
    _taskToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _taskToggle->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(_taskToggle, 1);
    _taskStatusLabel = new QLabel(tr("进行中"), header);
    _taskStatusLabel->setObjectName(QStringLiteral("CodexTaskStatus"));
    headerLayout->addWidget(_taskStatusLabel);
    layout->addWidget(header);
    _taskDetails = new QFrame(_taskCard);
    _taskDetails->setObjectName(QStringLiteral("CodexTaskDetails"));
    _taskDetailsLayout = new QVBoxLayout(_taskDetails);
    _taskDetailsLayout->setContentsMargins(0, 4, 0, 6);
    _taskDetailsLayout->setSpacing(0);
    layout->addWidget(_taskDetails);
    QToolButton* const taskToggle = _taskToggle;
    QFrame* const taskDetails = _taskDetails;
    QFrame* const taskCard = _taskCard;
    connect(taskToggle, &QToolButton::toggled, this, [this, taskToggle, taskDetails, taskCard](bool visible) {
        taskDetails->setVisible(visible);
        taskToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        taskCard->updateGeometry();
        if (_conversationScroll) _conversationScroll->updateGeometry();
    });
    _assistantBodyLayout->addWidget(_taskCard);
}

QLabel* CodexAgentWorkspace::ensureTaskStep(const QString& id)
{
    if (QLabel* existing = _taskSteps.value(id)) return existing;
    ensureTaskCard();
    auto* label = new QLabel(_taskDetails);
    label->setObjectName(QStringLiteral("CodexTaskStep"));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    _taskDetailsLayout->addWidget(label);
    _taskSteps.insert(id, label);
    return label;
}

void CodexAgentWorkspace::scrollToBottom()
{
    QTimer::singleShot(0, _conversationScroll, [scroll = _conversationScroll] {
        if (scroll && scroll->verticalScrollBar()) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        }
    });
}

void CodexAgentWorkspace::updateEmptyState()
{
    if (!_emptyStateLabel || _hasUserMessage) return;
    _emptyStateLabel->setText(_connected
        ? tr("开始新的任务")
        : (_retryAvailable ? tr("连接失败，可在顶部重试") : tr("正在连接 Codex...")));
    _emptyStateLabel->show();
}

void CodexAgentWorkspace::updateConversationTitle(const QString& prompt)
{
    if (_hasUserMessage) return;
    QString title = prompt.simplified();
    if (title.size() > 22) title = title.left(21) + QStringLiteral("…");
    _titleLabel->setText(title.isEmpty() ? tr("当前会话") : title);
    _hasUserMessage = true;
}

void CodexAgentWorkspace::clearConversationWidgets()
{
    while (QLayoutItem* item = _conversationLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    _emptyStateLabel = new QLabel(_conversationHost);
    _emptyStateLabel->setObjectName(QStringLiteral("CodexEmpty"));
    _emptyStateLabel->setAlignment(Qt::AlignCenter);
    _emptyStateLabel->setWordWrap(true);
    _conversationLayout->addWidget(_emptyStateLabel, 1);
    _conversationLayout->addStretch(1);
    _assistantTextLabel = nullptr;
    _assistantBodyLayout = nullptr;
    _taskCard = nullptr;
    _taskToggle = nullptr;
    _taskStatusLabel = nullptr;
    _taskDetails = nullptr;
    _taskDetailsLayout = nullptr;
    _taskSteps.clear();
}

} // namespace cgplay
