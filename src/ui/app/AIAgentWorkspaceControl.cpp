#include "AIAgentWorkspace.h"
#include "AIAgentWorkspaceSupport.h"

#include "core/playback/api/IPlaybackService.h"
#include "features/annotation/api/IAnnotationService.h"
#include "features/annotation/AnnotationToolbar.h"
#include "features/annotation/ReviewPanel.h"
#include "features/playlist/PlaylistPanel.h"
#include "settings/api/ISettingsService.h"
#include "ui/app/NavigationRail.h"
#include "ui/viewer/CompareToolbar.h"
#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QWidget>

namespace cgplay {

using namespace ai_agent_workspace_support;


QString AIAgentWorkspace::_selectedProviderId() const
{
    return _providerCombo ? _providerCombo->currentData().toString().trimmed() : QString();
}

bool AIAgentWorkspace::_isAutoModelSelectionEnabled() const
{
    if (_autoModelCheck) {
        return _autoModelCheck->isChecked();
    }
    return _userSettings
        ? _userSettings->value(QString::fromLatin1(kAutoModelSelectionKey), true).toBool()
        : true;
}

QString AIAgentWorkspace::_requestModel() const
{
    if (_selectedProviderId() == QString::fromLatin1(kOpenAIProviderId) &&
        _isAutoModelSelectionEnabled()) {
        return QString();
    }
    return _selectedModel().trimmed();
}

QString AIAgentWorkspace::_selectedModel() const
{
    return _modelCombo ? _modelCombo->currentText().trimmed() : QString();
}

QString AIAgentWorkspace::_defaultPrompt() const
{
    return zh(u8"帮我看看这个项目或者这个镜头现在最值得注意的问题，直接给我一个简洁结论。");
}

QJsonObject AIAgentWorkspace::_buildRuntimeControlState() const
{
    QJsonObject state;
    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    if (playback) {
        state.insert(QStringLiteral("mediaOpen"), playback->isValid());
        state.insert(QStringLiteral("mediaPath"), playback->currentPath());
        state.insert(QStringLiteral("currentFrame"), playback->currentFrame());
        state.insert(QStringLiteral("totalFrames"), playback->totalFrames());
        state.insert(QStringLiteral("fps"), playback->fps());
        state.insert(QStringLiteral("playbackState"), playback->playbackState());
        state.insert(QStringLiteral("hasCompare"), playback->hasCompare());
        state.insert(QStringLiteral("muted"), playback->isMuted());
        state.insert(QStringLiteral("volume"), playback->getVolume());
        state.insert(QStringLiteral("inPoint"), playback->inPoint());
        state.insert(QStringLiteral("outPoint"), playback->outPoint());
    }

    const QString activeViewId = runtimeActiveViewId();
    state.insert(QStringLiteral("activeViewId"), activeViewId);
    state.insert(QStringLiteral("hasActiveView"), !activeViewId.isEmpty());

    QWidget* topLevel = window();
    if (topLevel) {
        state.insert(QStringLiteral("fullScreen"), topLevel->isFullScreen());
    }

    if (auto* nav = topLevel ? topLevel->findChild<NavigationRail*>() : nullptr) {
        state.insert(QStringLiteral("navPage"), static_cast<int>(nav->currentPage()));
        state.insert(QStringLiteral("navPageText"), navigationPageDisplayText(static_cast<int>(nav->currentPage())));
        state.insert(QStringLiteral("navRailVisible"), nav->isVisible());
    }

    if (auto* playlist = topLevel ? topLevel->findChild<PlaylistPanel*>() : nullptr) {
        state.insert(QStringLiteral("leftPanelVisible"), playlist->isVisible());
    }

    if (auto* reviewPanel = topLevel ? topLevel->findChild<ReviewPanel*>(QStringLiteral("ReviewPanel")) : nullptr) {
        state.insert(QStringLiteral("reviewPanelVisible"), reviewPanel->isVisible());
    }

    if (auto* annotationToolbar = topLevel ? topLevel->findChild<AnnotationToolbar*>(QStringLiteral("AnnotationToolbar")) : nullptr) {
        state.insert(QStringLiteral("annotationToolbarVisible"), annotationToolbar->isVisible());
    }

    if (auto* compareToolbar = topLevel ? topLevel->findChild<CompareToolbar*>() : nullptr) {
        state.insert(QStringLiteral("compareToolbarVisible"), compareToolbar->isVisible());
        state.insert(QStringLiteral("compareMode"), compareToolbar->compareMode());
        state.insert(QStringLiteral("compareModeText"), compareModeDisplayText(compareToolbar->compareMode()));
        state.insert(QStringLiteral("compareALabel"), compareToolbar->shotALabel());
        state.insert(QStringLiteral("compareBLabel"), compareToolbar->shotBLabel());
    }

    IAnnotationService* annotationService = runtimeAnnotationService(_annotationService);
    if (annotationService) {
        const int tool = annotationService->currentTool();
        state.insert(QStringLiteral("annotationCount"), annotationService->count());
        state.insert(QStringLiteral("annotationTool"), tool);
        state.insert(QStringLiteral("annotationToolText"), annotationToolDisplayText(tool));
        state.insert(QStringLiteral("annotationColor"), annotationService->currentToolColor().name());
        state.insert(QStringLiteral("selectedAnnotationId"), annotationService->selectedAnnotationId());
    }

    if (auto* aiDock = topLevel ? topLevel->findChild<QDockWidget*>(QStringLiteral("AIAgentWorkspaceDock")) : nullptr) {
        state.insert(QStringLiteral("aiWorkspaceVisible"), aiDock->isVisible());
    } else {
        state.insert(QStringLiteral("aiWorkspaceVisible"), isVisible());
    }

    if (QAction* translationAction = findWindowAction(this, zh(u8"字幕翻译"))) {
        state.insert(QStringLiteral("translationVisible"), translationAction->isChecked());
    }
    return state;
}

QString AIAgentWorkspace::_buildRuntimeControlSummary() const
{
    const QJsonObject state = _buildRuntimeControlState();
    QStringList lines;
    lines << zh(u8"播放器运行时状态：");
    lines << zh(u8"- activeViewId：%1").arg(state.value(QStringLiteral("activeViewId")).toString().isEmpty()
        ? zh(u8"--")
        : state.value(QStringLiteral("activeViewId")).toString());
    lines << zh(u8"- 媒体已打开：%1").arg(state.value(QStringLiteral("mediaOpen")).toBool() ? zh(u8"是") : zh(u8"否"));
    lines << zh(u8"- 当前帧：%1 / %2")
        .arg(state.value(QStringLiteral("currentFrame")).toInt())
        .arg(state.value(QStringLiteral("totalFrames")).toInt());
    lines << zh(u8"- FPS：%1").arg(QString::number(state.value(QStringLiteral("fps")).toDouble(), 'f', 2));

    const int playbackState = state.value(QStringLiteral("playbackState")).toInt();
    QString playbackStateText = zh(u8"停止");
    if (playbackState == 1) {
        playbackStateText = zh(u8"正放");
    } else if (playbackState == 2) {
        playbackStateText = zh(u8"倒放");
    }
    lines << zh(u8"- 播放状态：%1").arg(playbackStateText);
    lines << zh(u8"- 静音：%1，音量：%2")
        .arg(state.value(QStringLiteral("muted")).toBool() ? zh(u8"开") : zh(u8"关"))
        .arg(QString::number(state.value(QStringLiteral("volume")).toDouble(), 'f', 2));
    lines << zh(u8"- 全屏：%1").arg(state.value(QStringLiteral("fullScreen")).toBool() ? zh(u8"是") : zh(u8"否"));
    lines << zh(u8"- 左侧面板：%1，右侧面板：%2")
        .arg(state.value(QStringLiteral("leftPanelVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"))
        .arg(state.value(QStringLiteral("reviewPanelVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"));
    lines << zh(u8"- 批注工具：%1，对比工具栏：%2")
        .arg(state.value(QStringLiteral("annotationToolbarVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"))
        .arg(state.value(QStringLiteral("compareToolbarVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"));
    lines << zh(u8"- AI 工作台：%1，字幕翻译：%2")
        .arg(state.value(QStringLiteral("aiWorkspaceVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"))
        .arg(state.value(QStringLiteral("translationVisible")).toBool() ? zh(u8"显示") : zh(u8"隐藏"));
    if (!state.value(QStringLiteral("navPageText")).toString().isEmpty()) {
        lines << zh(u8"- 当前页面：%1").arg(state.value(QStringLiteral("navPageText")).toString());
    }

    if (state.value(QStringLiteral("hasCompare")).toBool()) {
        lines << zh(u8"- A/B 对比：已加载");
        lines << zh(u8"- 对比模式：%1").arg(state.value(QStringLiteral("compareModeText")).toString());
        lines << zh(u8"- A 镜头：%1").arg(state.value(QStringLiteral("compareALabel")).toString());
        lines << zh(u8"- B 镜头：%1").arg(state.value(QStringLiteral("compareBLabel")).toString());
    } else {
        lines << zh(u8"- A/B 对比：未加载");
    }

    if (runtimeAnnotationService(_annotationService)) {
        lines << zh(u8"- 批注数量：%1").arg(state.value(QStringLiteral("annotationCount")).toInt());
        lines << zh(u8"- 当前批注工具：%1").arg(state.value(QStringLiteral("annotationToolText")).toString());
    }

    lines << zh(u8"说明：播放/音量/批注类动作作用于当前 activeView；全屏、面板显隐、导航页和翻译开关作用于主窗口 UI。");
    lines << zh(u8"可执行动作：playback.play, playback.pause, playback.toggle_play, playback.stop, playback.seek, playback.seek_relative, playback.next_frame, playback.prev_frame, playback.goto_start, playback.goto_end, playback.set_in_point, playback.set_out_point, playback.clear_in_out, audio.mute, audio.unmute, audio.toggle_mute, audio.set_volume, compare.set_mode, compare.toggle, annotation.create, annotation.set_tool, annotation.undo, annotation.redo, annotation.delete_selected, ui.set_visible, ui.toggle_visible, ui.set_page, review.set_tab, view.fullscreen, translation.refresh, translation.clear_history。");
    lines << zh(u8"如果用户要你直接操作播放器，请返回 <rvlite-actions>{\"actions\":[...]}</rvlite-actions>，并在 actions 之外用一句中文说明你做了什么。需要直接在画面里圈出来/框出来时，请优先使用 annotation.create，坐标使用 normalized1000。");
    return lines.join(QStringLiteral("\n"));
}

QVector<AIChatMessage> AIAgentWorkspace::_trimmedChatHistory(int maxMessages) const
{
    if (maxMessages <= 0 || _chatHistory.isEmpty()) {
        return {};
    }
    const int historySize = static_cast<int>(_chatHistory.size());
    const int takeCount = std::min(maxMessages, historySize);
    QVector<AIChatMessage> trimmed;
    trimmed.reserve(takeCount);
    for (int i = historySize - takeCount; i < historySize; ++i) {
        trimmed.push_back(_chatHistory.at(i));
    }
    return trimmed;
}

QString AIAgentWorkspace::_buildConversationContextSummary(bool compact) const
{
    QStringList lines;
    const int historyLimit = compact ? 4 : 8;
    const int textLimit = compact ? 90 : 180;

    const QVector<AIChatMessage> recentHistory = _trimmedChatHistory(historyLimit);
    if (!recentHistory.isEmpty()) {
        lines << zh(u8"最近对话：");
        for (const AIChatMessage& msg : recentHistory) {
            QString roleText;
            switch (msg.role) {
            case AIChatRole::User:
                roleText = zh(u8"用户");
                break;
            case AIChatRole::Assistant:
                roleText = zh(u8"AI");
                break;
            case AIChatRole::System:
                roleText = zh(u8"系统");
                break;
            }
            lines << QStringLiteral("- %1：%2")
                .arg(roleText, compactContextText(msg.content, textLimit));
        }
    }

    const QJsonObject runtimeState = _buildRuntimeControlState();
    QStringList runtimeParts;
    const QString activeViewId = runtimeState.value(QStringLiteral("activeViewId")).toString().trimmed();
    if (!activeViewId.isEmpty()) {
        runtimeParts << zh(u8"activeView=%1").arg(activeViewId);
    }
    if (runtimeState.value(QStringLiteral("mediaOpen")).toBool()) {
        runtimeParts << zh(u8"frame=%1/%2")
            .arg(runtimeState.value(QStringLiteral("currentFrame")).toInt())
            .arg(runtimeState.value(QStringLiteral("totalFrames")).toInt());
    }
    if (runtimeState.value(QStringLiteral("hasCompare")).toBool()) {
        runtimeParts << zh(u8"AB=%1")
            .arg(runtimeState.value(QStringLiteral("compareModeText")).toString());
        const QString aLabel = runtimeState.value(QStringLiteral("compareALabel")).toString().trimmed();
        const QString bLabel = runtimeState.value(QStringLiteral("compareBLabel")).toString().trimmed();
        if (!aLabel.isEmpty() || !bLabel.isEmpty()) {
            runtimeParts << zh(u8"A=%1, B=%2")
                .arg(aLabel.isEmpty() ? QStringLiteral("--") : aLabel,
                     bLabel.isEmpty() ? QStringLiteral("--") : bLabel);
        }
    }
    const QString selectedAnnotationId =
        runtimeState.value(QStringLiteral("selectedAnnotationId")).toString().trimmed();
    if (!selectedAnnotationId.isEmpty()) {
        runtimeParts << zh(u8"selectedAnnotation=%1").arg(selectedAnnotationId);
    }
    if (!runtimeParts.isEmpty()) {
        lines << zh(u8"当前运行时：%1").arg(runtimeParts.join(QStringLiteral(" | ")));
    }

    if (_lastResponse.success ||
        !_lastResponse.findings.isEmpty() ||
        !_lastResponse.annotationSuggestions.isEmpty() ||
        !_lastResponse.reviewSummary.plainText.trimmed().isEmpty() ||
        !_lastResponse.rawText.trimmed().isEmpty())
    {
        QStringList lastParts;
        if (!_lastResponse.findings.isEmpty()) {
            QStringList findingTitles;
            const int findingCount = static_cast<int>(_lastResponse.findings.size());
            const int limit = std::min(compact ? 2 : 3, findingCount);
            for (int i = 0; i < limit; ++i) {
                const QString title = _lastResponse.findings.at(i).title.trimmed();
                if (!title.isEmpty()) {
                    findingTitles << compactContextText(title, compact ? 28 : 40);
                }
            }
            lastParts << zh(u8"findings=%1").arg(_lastResponse.findings.size());
            if (!findingTitles.isEmpty()) {
                lastParts << zh(u8"重点=%1").arg(findingTitles.join(zh(u8"、")));
            }
        }
        if (!_lastResponse.annotationSuggestions.isEmpty()) {
            lastParts << zh(u8"suggestions=%1").arg(_lastResponse.annotationSuggestions.size());
        }
        if (lastParts.isEmpty()) {
            const QString plainText = !_lastResponse.reviewSummary.plainText.trimmed().isEmpty()
                ? _lastResponse.reviewSummary.plainText
                : _lastResponse.rawText;
            if (!plainText.trimmed().isEmpty()) {
                lastParts << compactContextText(plainText, compact ? 90 : 180);
            }
        }
        if (!lastParts.isEmpty()) {
            lines << zh(u8"上一轮结果：%1").arg(lastParts.join(QStringLiteral(" | ")));
        }
    }

    return lines.join(QStringLiteral("\n"));
}

QString AIAgentWorkspace::_stripAiActionBlock(const QString& text, QString* actionPayload) const
{
    static const QRegularExpression blockPattern(
        QStringLiteral("<rvlite-actions>([\\s\\S]*?)</rvlite-actions>"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = blockPattern.match(text);
    if (match.hasMatch()) {
        if (actionPayload) {
            *actionPayload = match.captured(1).trimmed();
        }
        QString cleaned = text;
        cleaned.remove(blockPattern);
        return cleaned.trimmed();
    }
    if (actionPayload) {
        actionPayload->clear();
    }
    return text.trimmed();
}

bool AIAgentWorkspace::_tryHandleLocalControlPrompt(const QString& prompt)
{
    const QString trimmedPrompt = prompt.trimmed();
    if (trimmedPrompt.isEmpty()) {
        return false;
    }
    if (trimmedPrompt.contains(QLatin1Char('\n')) || trimmedPrompt.size() > 80) {
        return false;
    }

    QString normalized = trimmedPrompt.toLower();
    normalized.replace(QStringLiteral("帮我"), QString());
    normalized.replace(QStringLiteral("请帮我"), QString());
    normalized.replace(QStringLiteral("请"), QString());
    normalized.replace(QStringLiteral("麻烦"), QString());
    normalized.replace(QStringLiteral("直接"), QString());
    normalized.replace(QStringLiteral("立刻"), QString());
    normalized.replace(QStringLiteral("马上"), QString());
    normalized = normalized.trimmed();

    const bool looksLikeInfoQuestion =
        normalized.contains(QStringLiteral("为什么")) ||
        normalized.contains(QStringLiteral("怎么")) ||
        normalized.contains(QStringLiteral("如何")) ||
        normalized.contains(QStringLiteral("什么")) ||
        normalized.contains(QStringLiteral("哪些")) ||
        normalized.contains(QStringLiteral("多少")) ||
        normalized.contains(QStringLiteral("原因"));
    if (looksLikeInfoQuestion) {
        return false;
    }

    const auto containsAny = [&](const QStringList& tokens) {
        for (const QString& token : tokens) {
            if (normalized.contains(token)) {
                return true;
            }
        }
        return false;
    };

    QJsonArray actions;
    const auto appendAction = [&](const QString& type, QJsonObject action = QJsonObject()) {
        action.insert(QStringLiteral("type"), type);
        actions.append(action);
    };

    const bool wantsShow = containsAny({ QStringLiteral("显示"), QStringLiteral("打开"), QStringLiteral("展开") });
    const bool wantsHide = containsAny({ QStringLiteral("隐藏"), QStringLiteral("关闭"), QStringLiteral("收起") });
    const bool wantsToggle = containsAny({ QStringLiteral("切换"), QStringLiteral("toggle") });

    const auto handleVisibilityTarget = [&](const QString& target, const QStringList& keywords) {
        if (!containsAny(keywords)) {
            return;
        }
        if (wantsShow && !wantsHide) {
            appendAction(QStringLiteral("ui.set_visible"), {
                { QStringLiteral("target"), target },
                { QStringLiteral("visible"), true }
            });
        } else if (wantsHide && !wantsShow) {
            appendAction(QStringLiteral("ui.set_visible"), {
                { QStringLiteral("target"), target },
                { QStringLiteral("visible"), false }
            });
        } else if (wantsToggle) {
            appendAction(QStringLiteral("ui.toggle_visible"), {
                { QStringLiteral("target"), target }
            });
        }
    };

    if (containsAny({ QStringLiteral("取消静音"), QStringLiteral("恢复声音"), QStringLiteral("打开声音"), QStringLiteral("取消静音") })) {
        appendAction(QStringLiteral("audio.unmute"));
    } else if (containsAny({ QStringLiteral("静音"), QStringLiteral("关声音") })) {
        appendAction(QStringLiteral("audio.mute"));
    }

    QRegularExpression volumePattern(QStringLiteral("音量\\s*([0-9]{1,3})"));
    QRegularExpressionMatch volumeMatch = volumePattern.match(normalized);
    if (volumeMatch.hasMatch()) {
        appendAction(QStringLiteral("audio.set_volume"), {
            { QStringLiteral("value"), volumeMatch.captured(1).toInt() }
        });
    }

    const bool explicitPlayCommand =
        containsAny({ QStringLiteral("继续播放"), QStringLiteral("开始播放"), QStringLiteral("恢复播放") }) ||
        normalized == QStringLiteral("播放");

    if (containsAny({ QStringLiteral("暂停播放"), QStringLiteral("暂停") })) {
        appendAction(QStringLiteral("playback.pause"));
    } else if (containsAny({ QStringLiteral("停止播放"), QStringLiteral("停止") })) {
        appendAction(QStringLiteral("playback.stop"));
    } else if (explicitPlayCommand) {
        appendAction(QStringLiteral("playback.play"));
    }

    if (containsAny({ QStringLiteral("下一帧"), QStringLiteral("前进一帧"), QStringLiteral("往后一帧") })) {
        appendAction(QStringLiteral("playback.next_frame"));
    }
    if (containsAny({ QStringLiteral("上一帧"), QStringLiteral("后退一帧"), QStringLiteral("往前一帧") })) {
        appendAction(QStringLiteral("playback.prev_frame"));
    }
    if (containsAny({ QStringLiteral("跳到开头"), QStringLiteral("回到开头"), QStringLiteral("第一帧") })) {
        appendAction(QStringLiteral("playback.goto_start"));
    }
    if (containsAny({ QStringLiteral("跳到结尾"), QStringLiteral("最后一帧") })) {
        appendAction(QStringLiteral("playback.goto_end"));
    }

    QRegularExpression seekPattern(QStringLiteral("(跳到|转到|定位到|到第)\\s*(\\d+)\\s*帧"));
    QRegularExpressionMatch seekMatch = seekPattern.match(normalized);
    if (seekMatch.hasMatch()) {
        appendAction(QStringLiteral("playback.seek"), {
            { QStringLiteral("frame"), seekMatch.captured(2).toInt() }
        });
    }

    if (containsAny({ QStringLiteral("退出全屏"), QStringLiteral("关闭全屏"), QStringLiteral("取消全屏") })) {
        appendAction(QStringLiteral("view.fullscreen"), {
            { QStringLiteral("enabled"), false }
        });
    } else if (containsAny({ QStringLiteral("全屏") })) {
        appendAction(QStringLiteral("view.fullscreen"), {
            { QStringLiteral("enabled"), true }
        });
    }

    handleVisibilityTarget(
        QStringLiteral("playlist"),
        { QStringLiteral("播放列表"), QStringLiteral("左侧面板") });
    handleVisibilityTarget(
        QStringLiteral("review_panel"),
        { QStringLiteral("审片面板"), QStringLiteral("批注面板"), QStringLiteral("右侧面板"), QStringLiteral("审阅面板") });
    handleVisibilityTarget(
        QStringLiteral("annotation_toolbar"),
        { QStringLiteral("批注工具"), QStringLiteral("绘制工具"), QStringLiteral("标注工具") });
    handleVisibilityTarget(
        QStringLiteral("compare_toolbar"),
        { QStringLiteral("对比工具栏"), QStringLiteral("ab工具栏"), QStringLiteral("a/b工具栏") });
    handleVisibilityTarget(
        QStringLiteral("ai_workspace"),
        { QStringLiteral("ai工作台"), QStringLiteral("ai 面板"), QStringLiteral("ai面板") });
    handleVisibilityTarget(
        QStringLiteral("translation"),
        { QStringLiteral("翻译条"), QStringLiteral("字幕翻译") });

    if (containsAny({ QStringLiteral("矩形工具"), QStringLiteral("矩形批注"), QStringLiteral("切到矩形"), QStringLiteral("矩形标注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("rectangle") }
        });
    } else if (containsAny({ QStringLiteral("箭头工具"), QStringLiteral("箭头批注"), QStringLiteral("切到箭头"), QStringLiteral("箭头标注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("arrow") }
        });
    } else if (containsAny({ QStringLiteral("圆形工具"), QStringLiteral("圆圈工具"), QStringLiteral("切到圆形"), QStringLiteral("圆形标注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("circle") }
        });
    } else if (containsAny({ QStringLiteral("文字工具"), QStringLiteral("文本工具"), QStringLiteral("切到文字"), QStringLiteral("文字标注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("text") }
        });
    } else if (containsAny({ QStringLiteral("自由绘制"), QStringLiteral("画笔工具"), QStringLiteral("涂鸦工具"), QStringLiteral("切到自由绘制") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("freedraw") }
        });
    } else if (containsAny({ QStringLiteral("点标记"), QStringLiteral("点工具"), QStringLiteral("切到点"), QStringLiteral("点批注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("point") }
        });
    } else if (containsAny({ QStringLiteral("选择工具"), QStringLiteral("切到选择"), QStringLiteral("选择批注") })) {
        appendAction(QStringLiteral("annotation.set_tool"), {
            { QStringLiteral("tool"), QStringLiteral("select") }
        });
    }

    if (containsAny({ QStringLiteral("撤销批注"), QStringLiteral("撤销") })) {
        appendAction(QStringLiteral("annotation.undo"));
    }
    if (containsAny({ QStringLiteral("重做批注"), QStringLiteral("重做") })) {
        appendAction(QStringLiteral("annotation.redo"));
    }
    if (containsAny({ QStringLiteral("删除批注"), QStringLiteral("删除当前批注"), QStringLiteral("删掉当前批注") })) {
        appendAction(QStringLiteral("annotation.delete_selected"));
    }

    if (actions.isEmpty()) {
        return false;
    }

    const QJsonDocument actionDoc(QJsonObject{
        { QStringLiteral("actions"), actions }
    });
    const QStringList results =
        _executeAiControlActions(QString::fromUtf8(actionDoc.toJson(QJsonDocument::Compact)));

    QString assistantText;
    if (results.isEmpty()) {
        assistantText = zh(u8"本地快速控制没有执行出有效动作。");
    } else {
        assistantText = zh(u8"已直接执行：%1").arg(results.join(zh(u8"；")));
    }

    AIChatMessage userMsg;
    userMsg.role = AIChatRole::User;
    userMsg.content = trimmedPrompt;
    userMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
    _chatHistory.append(userMsg);

    AIChatMessage assistantMsg;
    assistantMsg.role = AIChatRole::Assistant;
    assistantMsg.content = assistantText;
    assistantMsg.model = zh(u8"本地控制");
    assistantMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
    _chatHistory.append(assistantMsg);

    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(_renderChatHtml());

    const bool hasError = std::any_of(
        results.begin(),
        results.end(),
        [](const QString& text) {
            return text.contains(zh(u8"无法")) ||
                text.contains(zh(u8"失败")) ||
                text.contains(zh(u8"不可用"));
        });
    _setStatusMessage(
        hasError ? zh(u8"本地命令已处理，但有部分动作失败") : zh(u8"已本地快速执行播放器命令"),
        hasError ? kWarning : kSuccess);
    _refreshActionState();
    return true;
}

QStringList AIAgentWorkspace::_executeAiControlActions(const QString& actionPayload) const
{
    QStringList results;
    if (actionPayload.trimmed().isEmpty()) {
        return results;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(actionPayload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        results << zh(u8"AI 动作解析失败");
        return results;
    }

    const QJsonArray actions = doc.object().value(QStringLiteral("actions")).toArray();
    QWidget* topLevel = window();
    CompareToolbar* compareToolbar = topLevel ? topLevel->findChild<CompareToolbar*>() : nullptr;
    ReviewPanel* reviewPanel = topLevel ? topLevel->findChild<ReviewPanel*>(QStringLiteral("ReviewPanel")) : nullptr;
    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    IAnnotationService* annotationService = runtimeAnnotationService(_annotationService);

    for (const QJsonValue& value : actions) {
        const QJsonObject action = value.toObject();
        const QString type = normalizedToken(action.value(QStringLiteral("type")).toString());
        if (type.isEmpty()) {
            continue;
        }

        if (type == QStringLiteral("view.fullscreen") ||
            type == QStringLiteral("view.set_fullscreen") ||
            type == QStringLiteral("ui.fullscreen") ||
            type == QStringLiteral("fullscreen"))
        {
            if (!topLevel) {
                results << zh(u8"主窗口不可用");
                continue;
            }
            const bool requested = action.contains(QStringLiteral("enabled"))
                ? action.value(QStringLiteral("enabled")).toBool(!topLevel->isFullScreen())
                : !topLevel->isFullScreen();
            if (topLevel->isFullScreen() != requested) {
                if (!triggerWindowAction(this, zh(u8"全屏"))) {
                    if (requested) {
                        topLevel->showFullScreen();
                    } else {
                        topLevel->showNormal();
                    }
                }
            }
            results << (requested ? zh(u8"进入全屏") : zh(u8"退出全屏"));
        } else if (type == QStringLiteral("ui.set_visible") ||
                   type == QStringLiteral("panel.set_visible"))
        {
            const QString target = action.value(QStringLiteral("target")).toString();
            const bool visible = extractDesiredVisible(action, true);
            QString label;
            if (!setUiTargetVisible(this, target, visible, &label)) {
                results << zh(u8"无法切换界面项：%1").arg(target.isEmpty() ? zh(u8"未命名目标") : target);
                continue;
            }
            results << zh(u8"%1已%2").arg(label, visible ? zh(u8"显示") : zh(u8"隐藏"));
        } else if (type == QStringLiteral("ui.toggle_visible") ||
                   type == QStringLiteral("panel.toggle"))
        {
            const QString target = action.value(QStringLiteral("target")).toString();
            QString label;
            if (!toggleUiTargetVisible(this, target, &label)) {
                results << zh(u8"无法切换界面项：%1").arg(target.isEmpty() ? zh(u8"未命名目标") : target);
                continue;
            }
            bool known = false;
            const bool visibleNow = uiTargetVisible(this, target, &known);
            results << zh(u8"%1已切换为%2").arg(label, visibleNow ? zh(u8"显示") : zh(u8"隐藏"));
        } else if (type.startsWith(QStringLiteral("panel.")) &&
                   type != QStringLiteral("panel.set_visible") &&
                   type != QStringLiteral("panel.toggle"))
        {
            QString label;
            const QString target = type.mid(QStringLiteral("panel.").size());
            const bool visible = extractDesiredVisible(action, true);
            if (!setUiTargetVisible(this, target, visible, &label)) {
                results << zh(u8"无法切换界面项：%1").arg(target);
                continue;
            }
            results << zh(u8"%1已%2").arg(label, visible ? zh(u8"显示") : zh(u8"隐藏"));
        } else if (type == QStringLiteral("ui.set_page") ||
                   type == QStringLiteral("nav.set_page") ||
                   type == QStringLiteral("navigation.set_page"))
        {
            int page = -1;
            if (action.value(QStringLiteral("page")).isDouble()) {
                page = action.value(QStringLiteral("page")).toInt(-1);
            } else {
                page = navigationPageFromCode(action.value(QStringLiteral("page")).toString());
            }
            if (page < 0) {
                page = navigationPageFromCode(action.value(QStringLiteral("target")).toString());
            }
            if (!setNavigationPage(this, page)) {
                results << zh(u8"无法切换导航页");
                continue;
            }
            results << zh(u8"已切换到%1页").arg(navigationPageDisplayText(page));
        } else if (type == QStringLiteral("review.set_tab")) {
            if (!reviewPanel) {
                results << zh(u8"审片面板当前不可用");
                continue;
            }
            int tabIndex = -1;
            if (action.value(QStringLiteral("tab")).isDouble()) {
                tabIndex = action.value(QStringLiteral("tab")).toInt(-1);
            } else {
                tabIndex = reviewTabFromCode(action.value(QStringLiteral("tab")).toString());
            }
            if (tabIndex < 0) {
                tabIndex = reviewTabFromCode(action.value(QStringLiteral("name")).toString());
            }
            if (tabIndex < 0) {
                results << zh(u8"无法识别的审片页签");
                continue;
            }
            reviewPanel->setCurrentTab(tabIndex);
            results << zh(u8"已切换到审片面板的%1页签").arg(reviewTabDisplayText(tabIndex));
        } else if (type == QStringLiteral("translation.refresh")) {
            results << zh(u8"字幕翻译刷新已合并到当前字幕生成链路，请使用播放栏字幕翻译开关或重新生成字幕");
        } else if (type == QStringLiteral("translation.clear_history")) {
            results << zh(u8"旧实时翻译历史已移除，当前字幕翻译由媒体指纹缓存隔离管理");
        } else if (type == QStringLiteral("annotation.create")) {
            if (!annotationService) {
                results << zh(u8"批注服务不可用");
                continue;
            }

            AnnotationItem item;
            item.frame = action.value(QStringLiteral("frame")).toInt(playback ? playback->currentFrame() : 0);
            item.type = parseAnnotationTypeToken(
                action.value(QStringLiteral("tool")).toString(
                    action.value(QStringLiteral("shape")).toString(
                        action.value(QStringLiteral("annotationType")).toString())));
            const AISeverity severity =
                parseSeverityToken(action.value(QStringLiteral("severity")).toString());
            item.color = annotationColorFromAction(action, colorForSeverity(severity));
            item.author = QStringLiteral("AI");

            const QString title = action.value(QStringLiteral("title")).toString().trimmed();
            QString comment = action.value(QStringLiteral("comment")).toString().trimmed();
            if (comment.isEmpty()) {
                comment = action.value(QStringLiteral("text")).toString().trimmed();
            }
            if (comment.isEmpty()) {
                comment = title;
            } else if (!title.isEmpty() && !comment.startsWith(title)) {
                comment = title + zh(u8"：") + comment;
            }
            item.comment = comment.isEmpty() ? zh(u8"AI 视窗批注") : comment;
            item.points = buildAnnotationPoints(action, item.type, runtimeActiveMediaSize());

            const QString id = annotationService->addAnnotation(item);
            if (id.isEmpty()) {
                results << zh(u8"创建视窗批注失败");
                continue;
            }
            annotationService->selectAnnotation(id);
            results << (title.isEmpty()
                    ? zh(u8"已创建视窗批注")
                    : zh(u8"已创建视窗批注：%1").arg(title));
        } else if (type == QStringLiteral("annotation.set_tool") ||
                   type == QStringLiteral("annotation.select_tool"))
        {
            if (!annotationService) {
                results << zh(u8"批注服务不可用");
                continue;
            }
            const int tool = annotationToolFromCode(action.value(QStringLiteral("tool")).toString());
            if (tool < 0) {
                results << zh(u8"无法识别的批注工具");
                continue;
            }
            setUiTargetVisible(this, QStringLiteral("annotation_toolbar"), true, nullptr);
            annotationService->setTool(tool);
            results << zh(u8"批注工具切换为%1").arg(annotationToolDisplayText(tool));
        } else if (type == QStringLiteral("annotation.undo")) {
            if (!annotationService) {
                results << zh(u8"批注服务不可用");
                continue;
            }
            annotationService->undo();
            results << zh(u8"已撤销上一条批注操作");
        } else if (type == QStringLiteral("annotation.redo")) {
            if (!annotationService) {
                results << zh(u8"批注服务不可用");
                continue;
            }
            annotationService->redo();
            results << zh(u8"已重做上一条批注操作");
        } else if (type == QStringLiteral("annotation.delete_selected")) {
            if (!annotationService) {
                results << zh(u8"批注服务不可用");
                continue;
            }
            annotationService->removeSelectedAnnotation();
            results << zh(u8"已删除当前选中的批注");
        } else if (type == QStringLiteral("compare.toggle")) {
            if (!compareToolbar) {
                results << zh(u8"A/B 对比当前不可用");
                continue;
            }
            compareToolbar->toggleCompare();
            results << zh(u8"已切换 A/B 对比状态");
        } else if (!playback) {
            results << zh(u8"当前 activeView 的播放器服务不可用");
            break;
        } else if (type == QStringLiteral("playback.play") || type == QStringLiteral("play")) {
            playback->play();
            results << zh(u8"开始播放");
        } else if (type == QStringLiteral("playback.pause") || type == QStringLiteral("pause")) {
            playback->pause();
            results << zh(u8"暂停播放");
        } else if (type == QStringLiteral("playback.toggle_play") || type == QStringLiteral("toggle_play")) {
            playback->togglePlay();
            results << zh(u8"已切换播放状态");
        } else if (type == QStringLiteral("playback.stop") || type == QStringLiteral("stop")) {
            playback->stop();
            results << zh(u8"停止播放");
        } else if (type == QStringLiteral("playback.next_frame") || type == QStringLiteral("next_frame")) {
            playback->nextFrame();
            results << zh(u8"前进一帧");
        } else if (type == QStringLiteral("playback.prev_frame") || type == QStringLiteral("prev_frame")) {
            playback->prevFrame();
            results << zh(u8"后退一帧");
        } else if (type == QStringLiteral("playback.goto_start") || type == QStringLiteral("goto_start")) {
            playback->gotoStart();
            results << zh(u8"跳到开头");
        } else if (type == QStringLiteral("playback.goto_end") || type == QStringLiteral("goto_end")) {
            playback->gotoEnd();
            results << zh(u8"跳到结尾");
        } else if (type == QStringLiteral("playback.seek") || type == QStringLiteral("seek")) {
            const int frame = action.value(QStringLiteral("frame")).toInt(playback->currentFrame());
            playback->seekToFrame(frame);
            results << zh(u8"跳到第 %1 帧").arg(frame);
        } else if (type == QStringLiteral("playback.seek_relative") || type == QStringLiteral("seek_relative")) {
            const int deltaFrames = action.value(QStringLiteral("deltaFrames")).toInt(
                action.value(QStringLiteral("delta")).toInt(0));
            playback->seekRelative(deltaFrames);
            results << zh(u8"相对跳帧 %1").arg(deltaFrames);
        } else if (type == QStringLiteral("playback.set_in_point") || type == QStringLiteral("set_in_point")) {
            const int frame = action.value(QStringLiteral("frame")).toInt(playback->currentFrame());
            playback->setInPoint(frame);
            results << zh(u8"设置入点到第 %1 帧").arg(frame);
        } else if (type == QStringLiteral("playback.set_out_point") || type == QStringLiteral("set_out_point")) {
            const int frame = action.value(QStringLiteral("frame")).toInt(playback->currentFrame());
            playback->setOutPoint(frame);
            results << zh(u8"设置出点到第 %1 帧").arg(frame);
        } else if (type == QStringLiteral("playback.clear_in_out") || type == QStringLiteral("clear_in_out")) {
            playback->clearInOutPoints();
            results << zh(u8"清除入出点");
        } else if (type == QStringLiteral("audio.mute") || type == QStringLiteral("mute")) {
            playback->setMute(true);
            results << zh(u8"已静音");
        } else if (type == QStringLiteral("audio.unmute") || type == QStringLiteral("unmute")) {
            playback->setMute(false);
            results << zh(u8"已取消静音");
        } else if (type == QStringLiteral("audio.toggle_mute") || type == QStringLiteral("toggle_mute")) {
            playback->toggleMute();
            results << zh(u8"已切换静音状态");
        } else if (type == QStringLiteral("audio.set_volume") || type == QStringLiteral("set_volume")) {
            double volume = action.value(QStringLiteral("value")).toDouble(playback->getVolume());
            if (volume > 1.0 && volume <= 100.0) {
                volume /= 100.0;
            }
            volume = std::clamp(volume, 0.0, 1.0);
            playback->setVolume(static_cast<float>(volume));
            results << zh(u8"音量设置为 %1%").arg(QString::number(volume * 100.0, 'f', 0));
        } else if (type == QStringLiteral("compare.set_mode") || type == QStringLiteral("compare_mode")) {
            if (!compareToolbar) {
                results << zh(u8"A/B 对比当前不可用");
                continue;
            }
            int mode = -1;
            if (action.value(QStringLiteral("mode")).isDouble()) {
                mode = action.value(QStringLiteral("mode")).toInt(-1);
            } else {
                mode = compareModeFromCode(action.value(QStringLiteral("mode")).toString());
            }
            if (mode < 0) {
                results << zh(u8"无法识别的 A/B 模式");
                continue;
            }
            if (mode != 0 && !playback->hasCompare()) {
                results << zh(u8"当前还没有可用的 B 版本对比");
                continue;
            }
            compareToolbar->setCompareMode(mode);
            results << zh(u8"A/B 模式切换为 %1").arg(compareModeDisplayText(mode));
        } else {
            results << zh(u8"未支持的动作：%1").arg(type);
        }
    }

    return results;
}


} // namespace cgplay
