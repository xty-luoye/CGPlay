#include "AIAgentWorkspace.h"
#include "AIAgentWorkspaceSupport.h"

#include <QComboBox>
#include <QMap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QTextBrowser>

namespace cgplay {

using namespace ai_agent_workspace_support;

AIProviderInfo AIAgentWorkspace::_selectedProviderInfo() const
{
    const QString providerId = _selectedProviderId();
    for (const auto& providerInfo : _providerInfos) {
        if (providerInfo.providerId == providerId) {
            return providerInfo;
        }
    }
    return AIProviderInfo{};
}

QString AIAgentWorkspace::_renderResponseHtml(const AIResponse& response) const
{
    QString html;
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>")
        .arg(kText, htmlEscape(zh(u8"分析完成")));
    html += QStringLiteral("<div style='color:%1;margin-bottom:10px;'>%2<br/>%3<br/>%4</div>")
        .arg(
            kMuted,
            htmlEscape(zh(u8"服务：%1").arg(response.providerId)),
            htmlEscape(zh(u8"模型：%1").arg(response.model)),
            htmlEscape(zh(u8"耗时：%1 ms").arg(QString::number(response.durationMs))));

    QStringList tokenParts;
    if (response.promptTokens >= 0) {
        tokenParts.push_back(zh(u8"输入 %1").arg(QString::number(response.promptTokens)));
    }
    if (response.completionTokens >= 0) {
        tokenParts.push_back(zh(u8"输出 %1").arg(QString::number(response.completionTokens)));
    }
    if (response.totalTokens >= 0) {
        tokenParts.push_back(zh(u8"总计 %1").arg(QString::number(response.totalTokens)));
    }
    if (!tokenParts.isEmpty()) {
        html += QStringLiteral("<div style='color:%1;margin-bottom:8px;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"Token 消耗：%1").arg(tokenParts.join(zh(u8" / ")))));
    }
    if (response.cachedPromptTokens >= 0) {
        QString cacheText = zh(u8"Token 缓存命中：%1").arg(QString::number(response.cachedPromptTokens));
        if (response.promptCacheHitRate >= 0.0) {
            cacheText += zh(u8"（命中率 %1%）")
                .arg(QString::number(response.promptCacheHitRate, 'f', 1));
        }
        html += QStringLiteral("<div style='color:%1;margin-bottom:8px;'>%2</div>")
            .arg(kMuted, htmlEscape(cacheText));
    }

    // Frame capture diagnostic
    if (response.frameCount > 0) {
        html += QStringLiteral("<div style='color:%1;margin-bottom:8px;'>%2</div>")
            .arg(kSuccess, htmlEscape(zh(u8"✓ 已附带 %1 帧截图").arg(response.frameCount)));
    } else if (!response.frameCaptureError.isEmpty()) {
        html += QStringLiteral("<div style='color:%1;margin-bottom:8px;'>%2</div>")
            .arg(kError, htmlEscape(zh(u8"✗ 截图失败：%1").arg(response.frameCaptureError)));
    } else {
        html += QStringLiteral("<div style='color:%1;margin-bottom:8px;'>%2</div>")
            .arg(kWarning, htmlEscape(zh(u8"⚠ 未附带帧截图（可能当前模型不支持图片，或未打开媒体）")));
    }

    if (!response.findings.isEmpty()) {
        html += QStringLiteral("<div style='font-weight:600;color:%1;margin-bottom:6px;'>%2</div><ul>")
            .arg(kText, htmlEscape(zh(u8"问题列表")));
        for (const auto& finding : response.findings) {
            const QString title = finding.title.isEmpty() ? zh(u8"未命名问题") : finding.title;
            const QString description = finding.description.isEmpty()
                ? zh(u8"没有更多说明。")
                : finding.description;
            const QString category = finding.category.isEmpty()
                ? zh(u8"未分类")
                : finding.category;
            html += QStringLiteral(
                "<li style='margin-bottom:8px;'><b>[%1]</b> %2"
                "<div style='color:%3;'>%4</div>"
                "<div style='margin-top:2px;'>%5</div></li>")
                .arg(
                    htmlEscape(_severityText(finding.severity)),
                    htmlEscape(title),
                    kMuted,
                    htmlEscape(zh(u8"帧 %1 · %2").arg(QString::number(finding.frame), category)),
                    htmlEscape(description));
        }
        html += QStringLiteral("</ul>");
    }

    if (!response.annotationSuggestions.isEmpty()) {
        html += QStringLiteral(
            "<div style='margin-top:10px;color:%1;font-weight:600;'>%2</div>")
            .arg(kSuccess, htmlEscape(zh(u8"批注建议（%1 条）").arg(QString::number(response.annotationSuggestions.size()))));
        html += QStringLiteral(
            "<div style='margin:4px 0;'><a href='apply_all_annotations' style='color:%1;text-decoration:none;'>"
            "[全部应用]</a></div>")
            .arg(kAccent);
        for (int i = 0; i < response.annotationSuggestions.size(); ++i) {
            const auto& sugg = response.annotationSuggestions[i];
            const QString title = sugg.title.isEmpty() ? zh(u8"批注 %1").arg(i + 1) : sugg.title;
            html += QStringLiteral(
                "<div style='margin:4px 0;padding:6px;border:1px solid rgba(255,255,255,0.06);border-radius:4px;'>"
                "<b>[%1]</b> %2 <span style='color:%3;'>帧 %4</span>"
                "<div style='color:%3;margin-top:2px;'>%5</div>"
                "<div style='margin-top:4px;'><a href='apply_annotation_%6' style='color:%7;text-decoration:none;'>[应用]</a></div>"
                "</div>")
                .arg(
                    htmlEscape(_severityText(sugg.severity)),
                    htmlEscape(title),
                    kMuted,
                    QString::number(sugg.frame),
                    htmlEscape(sugg.comment),
                    QString::number(i),
                    kAccent);
        }
    }

    if (!response.reviewSummary.plainText.trimmed().isEmpty()) {
        html += QStringLiteral(
            "<div style='font-weight:600;color:%1;margin-top:10px;margin-bottom:6px;'>%2</div>"
            "<div style='color:%3;'>%4</div>")
            .arg(kText, htmlEscape(zh(u8"总结")), kMuted, htmlEscape(response.reviewSummary.plainText));
    }

    if (response.findings.isEmpty() && !response.rawText.trimmed().isEmpty()) {
        html += QStringLiteral(
            "<div style='font-weight:600;color:%1;margin-top:10px;margin-bottom:6px;'>%2</div>"
            "<div style='color:%3;'>%4</div>")
            .arg(kText, htmlEscape(zh(u8"原始输出")), kMuted, htmlEscape(response.rawText));
    }

    if (response.findings.isEmpty() && response.rawText.trimmed().isEmpty()) {
        html += QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"服务已返回结果，但暂时没有结构化 findings。")));
    }
    return html;
}

QString AIAgentWorkspace::_renderFailureHtml(const QString& title, const QString& detail) const
{
    return QStringLiteral(
        "<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>"
        "<div style='color:%3;'>%4</div>")
        .arg(kError, htmlEscape(title), kMuted, htmlEscape(detail));
}

QString AIAgentWorkspace::_severityText(AISeverity severity)
{
    switch (severity) {
    case AISeverity::Critical:
        return zh(u8"严重");
    case AISeverity::Major:
        return zh(u8"主要");
    case AISeverity::Minor:
        return zh(u8"次要");
    case AISeverity::Info:
        return zh(u8"提示");
    case AISeverity::Unknown:
    default:
        return zh(u8"未知");
    }
}

bool AIAgentWorkspace::_shouldUseVisualContextForPrompt(const QString& prompt) const
{
    const QString normalized = prompt.trimmed().toLower();
    if (normalized.isEmpty()) {
        return true;
    }

    if (isDirectMarkupPrompt(normalized)) {
        return true;
    }

    static const QStringList visualKeywords{
        QStringLiteral("当前帧"),
        QStringLiteral("这一帧"),
        QStringLiteral("画面"),
        QStringLiteral("镜头"),
        QStringLiteral("序列"),
        QStringLiteral("视频"),
        QStringLiteral("截图"),
        QStringLiteral("构图"),
        QStringLiteral("灯光"),
        QStringLiteral("动画"),
        QStringLiteral("合成"),
        QStringLiteral("特效"),
        QStringLiteral("批注"),
        QStringLiteral("对比"),
        QStringLiteral("比对"),
        QStringLiteral("区别"),
        QStringLiteral("差异"),
        QStringLiteral("差别"),
        QStringLiteral("不同"),
        QStringLiteral("两个版本"),
        QStringLiteral("两版"),
        QStringLiteral("版本a"),
        QStringLiteral("版本b"),
        QStringLiteral("a版"),
        QStringLiteral("b版"),
        QStringLiteral("before"),
        QStringLiteral("after"),
        QStringLiteral("compare"),
        QStringLiteral("difference"),
        QStringLiteral("diff"),
        QStringLiteral("shot"),
        QStringLiteral("frame"),
        QStringLiteral("scene"),
        QStringLiteral("video"),
        QStringLiteral("image"),
        QStringLiteral("screen"),
        QStringLiteral("composition"),
        QStringLiteral("lighting"),
        QStringLiteral("animation"),
        QStringLiteral("fx"),
        QStringLiteral("vfx"),
        QStringLiteral("annotat")
    };

    for (const QString& keyword : visualKeywords) {
        if (normalized.contains(keyword)) {
            return true;
        }
    }
    return isLikelyComparePrompt(normalized);
}

void AIAgentWorkspace::_seedPromptIfEmpty(const QString& text)
{
    if (!_promptEdit) {
        return;
    }
    if (_promptEdit->toPlainText().trimmed().isEmpty()) {
        _promptEdit->setPlainText(text);
    }
}

void AIAgentWorkspace::_setBatchScope(AIRequestScope scope)
{
    if (!_scanRangeCombo) {
        return;
    }
    const int index = _scanRangeCombo->findData(static_cast<int>(scope));
    if (index >= 0) {
        _scanRangeCombo->setCurrentIndex(index);
    }
}

QVector<AIAnnotationSuggestion> AIAgentWorkspace::_parseAnnotationSuggestions(
    const QString& rawText,
    int fallbackFrame) const
{
    QVector<AIAnnotationSuggestion> suggestions;
    QSet<QString> dedupeKeys;
    const QStringList lines = rawText.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("```")) ||
            line.startsWith(QStringLiteral("总结")) ||
            line.startsWith(QStringLiteral("说明")) ||
            line.startsWith(QStringLiteral("备注")))
        {
            continue;
        }

        line.remove(QRegularExpression(QStringLiteral("^[\\-\\*•\\d\\s\\.)、]+")));
        if (line.isEmpty() || !line.contains(QLatin1Char('|'))) {
            continue;
        }
        const QString compactLine = line;
        if (compactLine.contains(QStringLiteral("---")) ||
            compactLine.contains(QStringLiteral("===")))
        {
            continue;
        }

        const QStringList rawParts = line.split(QLatin1Char('|'), Qt::KeepEmptyParts);
        QStringList positionalParts;
        QMap<QString, QString> fields;
        for (QString part : rawParts) {
            part = part.trimmed();
            if (part.isEmpty()) {
                continue;
            }
            const int equalPos = part.indexOf(QLatin1Char('='));
            const int colonPos = part.indexOf(QRegularExpression(QStringLiteral("[:：]")));
            const int splitPos = equalPos >= 0 ? equalPos : colonPos;
            if (splitPos > 0) {
                const QString key = part.left(splitPos).trimmed().toLower();
                const QString value = part.mid(splitPos + 1).trimmed();
                fields.insert(key, value);
            } else {
                positionalParts.push_back(part);
            }
        }

        QString frameToken = fields.value(QStringLiteral("frame"));
        if (frameToken.isEmpty()) {
            frameToken = fields.value(QStringLiteral("帧号"));
        }
        if (frameToken.isEmpty()) {
            frameToken = fields.value(QStringLiteral("帧"));
        }

        QString severityToken = fields.value(QStringLiteral("severity"));
        if (severityToken.isEmpty()) {
            severityToken = fields.value(QStringLiteral("严重度"));
        }

        QString typeToken = fields.value(QStringLiteral("type"));
        if (typeToken.isEmpty()) {
            typeToken = fields.value(QStringLiteral("标注类型"));
        }

        QString title = fields.value(QStringLiteral("title"));
        if (title.isEmpty()) {
            title = fields.value(QStringLiteral("标题"));
        }

        QString comment = fields.value(QStringLiteral("comment"));
        if (comment.isEmpty()) {
            comment = fields.value(QStringLiteral("建议"));
        }
        if (comment.isEmpty()) {
            comment = fields.value(QStringLiteral("备注"));
        }
        if (comment.isEmpty()) {
            comment = fields.value(QStringLiteral("说明"));
        }

        if (!positionalParts.isEmpty()) {
            if (frameToken.isEmpty() && positionalParts.size() > 0) {
                frameToken = positionalParts.value(0);
            }
            if (severityToken.isEmpty() && positionalParts.size() > 1) {
                severityToken = positionalParts.value(1);
            }
            if (typeToken.isEmpty() && positionalParts.size() > 2) {
                typeToken = positionalParts.value(2);
            }
            if (title.isEmpty() && positionalParts.size() > 3) {
                title = positionalParts.value(3);
            }
            if (comment.isEmpty() && positionalParts.size() > 4) {
                comment = positionalParts.mid(4).join(QStringLiteral(" | "));
            }
        }

        const QString normalizedFrameToken = frameToken.trimmed().toLower();
        const QString normalizedTitle = title.trimmed().toLower();
        if (normalizedFrameToken == QStringLiteral("frame") ||
            normalizedFrameToken == QStringLiteral("帧") ||
            normalizedFrameToken == QStringLiteral("帧号") ||
            normalizedTitle == QStringLiteral("title") ||
            normalizedTitle == QStringLiteral("标题"))
        {
            continue;
        }

        bool frameOk = false;
        const int parsedFrame = frameToken.remove(QRegularExpression(QStringLiteral("[^0-9\\-]"))).toInt(&frameOk);
        const int frame = frameOk ? parsedFrame : fallbackFrame;

        AIAnnotationSuggestion suggestion;
        suggestion.frame = frame;
        suggestion.severity = parseSeverityToken(severityToken);
        suggestion.annotationType = parseAnnotationTypeToken(typeToken);
        suggestion.title = title.isEmpty() ? zh(u8"AI 建议批注") : title;
        suggestion.comment = comment.isEmpty() ? suggestion.title : comment;
        suggestion.color = colorForSeverity(suggestion.severity);

        const QString dedupeKey = QStringLiteral("%1|%2|%3")
            .arg(QString::number(suggestion.frame), suggestion.title, suggestion.comment);
        if (dedupeKeys.contains(dedupeKey)) {
            continue;
        }
        dedupeKeys.insert(dedupeKey);
        suggestions.push_back(suggestion);
    }
    return suggestions;
}

// ============================================================
// 1. Multi-turn Chat (ChatHistoryAgent) — integrated into _submitCurrentFrameAnalysis
// ============================================================

void AIAgentWorkspace::_clearChatHistory()
{
    _chatHistory.clear();
    _activeSubmittedPrompt.clear();
    _lastResponse = AIResponse{};
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"对话已清空。"))));
    _setStatusMessage(zh(u8"对话已清空"), kMuted);
}

QString AIAgentWorkspace::_renderChatHtml() const
{
    if (_chatHistory.isEmpty()) {
        return {};
    }

    QString html;
    html += QStringLiteral("<div style='font-weight:600;color:%1;margin-bottom:8px;'>%2</div>")
        .arg(kText, htmlEscape(zh(u8"对话历史")));

    for (const auto& msg : _chatHistory) {
        QString roleText;
        QString roleColor;
        switch (msg.role) {
        case AIChatRole::User:
            roleText = zh(u8"我");
            roleColor = kAccent;
            break;
        case AIChatRole::Assistant:
            roleText = zh(u8"AI");
            roleColor = kSuccess;
            break;
        default:
            continue;
        }
        html += QStringLiteral(
            "<div style='margin-bottom:8px;padding:8px;border-radius:6px;background:#111418;'>"
            "<b style='color:%1;'>%2</b> "
            "<span style='color:%3;font-size:11px;'>%4</span>"
            "<div style='color:%5;margin-top:4px;'>%6</div>"
            "</div>")
            .arg(
                roleColor,
                htmlEscape(roleText),
                kMuted,
                msg.model.isEmpty() ? QString() : htmlEscape(msg.model),
                kText,
                htmlEscape(msg.content));
    }
    return html;
}

// ============================================================
// 2. Batch Frame Scan (BatchScanAgent)
// ============================================================


} // namespace cgplay
