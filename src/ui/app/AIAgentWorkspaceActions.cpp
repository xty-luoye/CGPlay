#include "AIAgentWorkspace.h"
#include "AIAgentWorkspaceSupport.h"

#include "core/playback/api/IPlaybackService.h"
#include "services/ai/api/IAIProviderManager.h"
#include "services/ai/api/IAIWorkflowService.h"
#include "features/annotation/api/IAnnotationService.h"
#include "features/annotation/ReviewExport.h"
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QSet>
#include <QTextBrowser>
#include <QUrl>

namespace cgplay {

using namespace ai_agent_workspace_support;

void AIAgentWorkspace::_submitFrameCriticAgent()
{
    _seedPromptIfEmpty(zh(u8"请从构图、灯光、动画、FX、合成五个维度审阅当前帧，并按严重度给出最值得优先修改的问题。"));
    _submitCurrentFrameAnalysis();
}

void AIAgentWorkspace::_submitShotReviewAgent()
{
    _seedPromptIfEmpty(zh(u8"请审阅当前镜头，按帧抽样列出问题，并指出哪些问题需要优先修改。"));
    _setBatchScope(AIRequestScope::CurrentShot);
    _submitBatchScan();
}

void AIAgentWorkspace::_submitSequenceReviewAgent()
{
    _seedPromptIfEmpty(zh(u8"请审阅当前序列，按抽样帧总结连续性、色彩、合成和表演上的问题。"));
    _setBatchScope(AIRequestScope::CurrentSequence);
    _submitBatchScan();
}

void AIAgentWorkspace::_submitAnnotationSuggestionAgent()
{
    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        _setStatusMessage(zh(u8"提交失败"), kError);
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        _setStatusMessage(zh(u8"没有可用的 AI 服务"), kWarning);
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();
    if (!hasMedia) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法生成批注建议"), zh(u8"请先打开媒体文件。")));
        _setStatusMessage(zh(u8"请先打开媒体文件"), kWarning);
        return;
    }

    AIWorkflowRequest request;
    request.workflow = AIWorkflowKind::AnalyzeFrame;
    request.scope = AIRequestScope::CurrentFrame;
    request.providerId = providerId;
    request.model = _requestModel();
    request.systemPrompt = zh(u8"你是一名影视审片主管。请只输出可直接转换成批注的建议，"
                               "不要写前言，不要写总结，每行一条。"
                               "严格格式：frame=<帧号> | severity=<Critical/Major/Minor> | "
                               "type=<arrow|rectangle|circle|text|point> | title=<简短标题> | "
                               "comment=<给艺术家或合成师的修改建议>。");
    const QString prompt = _promptEdit->toPlainText().trimmed();
    request.userPrompt = prompt.isEmpty()
        ? zh(u8"请针对当前帧输出最多 5 条可以直接创建为批注的修改建议。")
        : prompt;
    request.attachFrames = true;
    request.requireFrames = true;
    request.includeAnnotations = true;
    request.sampleCount = 1;
    request.targetSize = QSize(1280, 720);
    request.options.insert(QStringLiteral("maxOutputTokens"), 1200);

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = ActiveJobMode::SuggestAnnotations;
    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        _setStatusMessage(zh(u8"提交失败"), kError);
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在生成可应用的批注建议..."))));
    _setStatusMessage(zh(u8"正在生成批注建议..."), kWarning);
    _refreshActionState();
}

void AIAgentWorkspace::_submitCurrentFrameAnalysis()
{
    if (!_activeJobId.isEmpty()) {
        _setStatusMessage(zh(u8"当前请求还在处理中，请稍等。"), kWarning);
        return;
    }

    const QString prompt = _promptEdit ? _promptEdit->toPlainText().trimmed() : QString();
    if (_tryHandleLocalControlPrompt(prompt)) {
        return;
    }

    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        _setStatusMessage(zh(u8"提交失败"), kError);
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        _setStatusMessage(zh(u8"没有可用的 AI 服务"), kWarning);
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();
    const QString normalizedPrompt = prompt.trimmed().toLower();
    const bool directMarkupPrompt = hasMedia && isDirectMarkupPrompt(normalizedPrompt);
    const bool useVisualContext = hasMedia && _shouldUseVisualContextForPrompt(prompt);
    const bool compareRuntimeActive = hasMedia && playback && playback->hasCompare();
    const bool useCompareScope = useVisualContext && (compareRuntimeActive || isLikelyComparePrompt(normalizedPrompt));
    const bool fastCompareMarkupPath = useCompareScope && directMarkupPrompt;
    const QString actionSpec = zh(u8"如果用户要求你直接操作播放器，请返回 <rvlite-actions>{\"actions\":[...]}</rvlite-actions>。"
                                    "可用动作包括 playback.play, playback.pause, playback.toggle_play, playback.stop, playback.seek, playback.seek_relative, playback.next_frame, playback.prev_frame, playback.goto_start, playback.goto_end, playback.set_in_point, playback.set_out_point, playback.clear_in_out, audio.mute, audio.unmute, audio.toggle_mute, audio.set_volume, compare.set_mode, compare.toggle, annotation.create, annotation.set_tool, annotation.undo, annotation.redo, annotation.delete_selected, ui.set_visible, ui.toggle_visible, ui.set_page, review.set_tab, view.fullscreen, translation.refresh, translation.clear_history。"
                                    "如果要直接在画面里圈出来、框出来或标出来，请优先返回 annotation.create；坐标请使用 coordSpace=normalized1000，并提供 x1/y1/x2/y2 或 points。"
                                    "播放类动作作用于当前 activeView，界面类动作作用于主窗口。");
    AIWorkflowRequest request;
    request.workflow = useCompareScope ? AIWorkflowKind::CompareShots : AIWorkflowKind::AnalyzeFrame;
    request.scope = useCompareScope
        ? AIRequestScope::CompareAB
        : (useVisualContext ? AIRequestScope::CurrentFrame : AIRequestScope::Custom);
    request.providerId = providerId;
    request.model = _requestModel();
    if (useCompareScope) {
        request.systemPrompt = zh(u8"你是 RVLite 内置的 AI 审片与控制助手。当前画面处于 A/B 对比语境。请优先利用播放器运行时提供的 A/B 映射、镜头名称、对比模式和附带截图来回答。");
        if (directMarkupPrompt) {
            request.systemPrompt += zh(u8"如果用户要求你直接把差异圈出来、框出来或标出来，不要只给文字描述，请优先返回 annotation.create 动作，每个区域一个 action。");
        }
        request.systemPrompt += actionSpec;
    } else if (directMarkupPrompt) {
        request.systemPrompt = zh(u8"你是 RVLite 内置的 AI 审片与控制助手。用户正在要求你直接在当前画面里圈出来、框出来或标出来。不要只给文字说明，请优先返回 annotation.create 动作，并把真正要标注的位置用 normalized1000 坐标写出来。"
                                   ) + actionSpec;
    } else if (useVisualContext) {
        request.systemPrompt = zh(u8"你是 RVLite 内置的 AI 审片与控制助手。用户可能在做正常对话，也可能在询问当前帧问题。请结合当前帧与播放器运行时状态回答。"
                                   ) + actionSpec;
    } else {
        request.systemPrompt = zh(u8"你是 RVLite 内置的 AI 助手，同时也是播放器控制代理。请结合播放器运行时状态回答。"
                                   ) + actionSpec + zh(u8"并尽量用最少动作完成用户目标。");
    }
    request.userPrompt = prompt.isEmpty() ? _defaultPrompt() : prompt;
    request.attachFrames = useVisualContext;
    request.requireFrames = false;
    request.includeAnnotations = useVisualContext && !fastCompareMarkupPath;
    request.sampleCount = useCompareScope ? 2 : 1;
    request.targetSize = fastCompareMarkupPath ? QSize(960, 540) : QSize(1280, 720);
    request.chatHistory = fastCompareMarkupPath ? _trimmedChatHistory(2) : _trimmedChatHistory(8);
    request.options.insert(
        QStringLiteral("conversationContextSummary"),
        _buildConversationContextSummary(fastCompareMarkupPath));
    if (!fastCompareMarkupPath) {
        request.options.insert(QStringLiteral("runtimeControlSummary"), _buildRuntimeControlSummary());
        request.options.insert(QStringLiteral("runtimeControlState"), _buildRuntimeControlState());
    }
    if (fastCompareMarkupPath) {
        request.options.insert(QStringLiteral("maxOutputTokens"), 280);
        request.options.insert(QStringLiteral("imageDetail"), QStringLiteral("low"));
        request.options.insert(QStringLiteral("frameEncoding"), QStringLiteral("jpeg"));
    } else if (!useVisualContext) {
        request.options.insert(QStringLiteral("maxOutputTokens"), 800);
    }

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = ActiveJobMode::AnalyzeFrame;
    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        _setStatusMessage(zh(u8"提交失败"), kError);
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"已发送，等待返回。"))));
    _setStatusMessage(zh(u8"已发送。"), kWarning);
    _refreshActionState();
}

void AIAgentWorkspace::_submitConnectionProbe()
{
    if (!_providerManager) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"连接测试失败"), zh(u8"AIProviderManager 不可用。")));
        _setStatusMessage(zh(u8"连接测试失败"), kError);
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"连接测试失败"), zh(u8"当前没有选中的 AI 服务。")));
        _setStatusMessage(zh(u8"连接测试失败"), kError);
        return;
    }

    AIRequest request;
    request.providerId = providerId;
    request.model = _requestModel();
    request.context.scope = AIRequestScope::Custom;
    request.systemPrompt = zh(u8"你是媒体审片应用的连接测试助手。请用一句简短的话确认当前模型可用。");
    request.userPrompt = zh(u8"请只回复：连接正常");
    request.options.insert(QStringLiteral("maxOutputTokens"), 48);

    _storeSelections();
    _activeJobId = _providerManager->submit(request);
    _activeJobMode = ActiveJobMode::ProbeConnection;
    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"连接测试失败"), zh(u8"ProviderManager 没有返回任务编号。")));
        _setStatusMessage(zh(u8"连接测试失败"), kError);
        return;
    }

    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在测试当前协议..."))));
    _setStatusMessage(zh(u8"正在测试连接..."), kWarning);
    _refreshActionState();
}

void AIAgentWorkspace::_cancelCurrentAnalysis()
{
    if (_activeJobMode == ActiveJobMode::ProbeConnection) {
        if (_activeJobId.isEmpty()) {
            return;
        }
        if (_providerManager && _providerManager->cancel(_activeJobId)) {
            _activeJobId.clear();
            _activeJobMode = ActiveJobMode::None;
            _activeSubmittedPrompt.clear();
            _resultView->setHtml(QStringLiteral("<div style='color:%1;'>%2</div>")
                                     .arg(kMuted, htmlEscape(zh(u8"当前 AI 任务已取消。"))));
            _setStatusMessage(zh(u8"当前 AI 任务已取消。"), kMuted);
            _refreshActionState();
            return;
        }
        _resultView->setHtml(_renderFailureHtml(zh(u8"取消失败"), zh(u8"当前任务无法取消。")));
        _setStatusMessage(zh(u8"取消失败"), kError);
        return;
    }

    if (_activeJobId.isEmpty() || !_workflowService) {
        return;
    }

    const QString canceledJobId = _activeJobId;
    if (_workflowService->cancel(canceledJobId)) {
        _activeJobId.clear();
        _activeJobMode = ActiveJobMode::None;
        _activeSubmittedPrompt.clear();
        _resultView->setHtml(
            QStringLiteral("<div style='color:%1;'>%2</div>")
                .arg(kMuted, htmlEscape(zh(u8"已取消当前分析任务。"))));
        _setStatusMessage(zh(u8"已取消分析任务。"), kMuted);
        _refreshActionState();
        return;
    }

    _resultView->setHtml(_renderFailureHtml(zh(u8"取消失败"), zh(u8"当前任务无法取消。")));
    _setStatusMessage(zh(u8"取消失败"), kError);
}


void AIAgentWorkspace::_submitBatchScan()
{
    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();
    if (!hasMedia) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法扫描"), zh(u8"请先打开媒体文件。")));
        return;
    }

    const AIRequestScope scope = static_cast<AIRequestScope>(_scanRangeCombo->currentData().toInt());
    const int sampleCount = _sampleCountCombo->currentData().toInt();

    AIWorkflowRequest request;
    request.workflow = (scope == AIRequestScope::CurrentShot) ? AIWorkflowKind::AnalyzeShot : AIWorkflowKind::AnalyzeSequence;
    request.scope = scope;
    request.providerId = providerId;
    request.model = _requestModel();
    request.systemPrompt = zh(u8"你是一名影视审片主管。请对提供的多个帧进行批量分析，"
                               "逐帧指出问题（构图、灯光、色彩、穿帮、追踪点残留等），"
                               "按严重程度分类。输出格式：\n"
                               "帧号 | 严重度 | 问题标题 | 详细描述");
    const QString prompt = _promptEdit->toPlainText().trimmed();
    request.userPrompt = prompt.isEmpty()
        ? zh(u8"请扫描以下帧，找出所有值得关注的问题。")
        : prompt;
    request.attachFrames = true;
    request.requireFrames = true;
    request.includeAnnotations = true;
    request.sampleCount = sampleCount;
    request.targetSize = QSize(1280, 720);

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = (scope == AIRequestScope::CurrentShot) ? ActiveJobMode::AnalyzeShot : ActiveJobMode::AnalyzeSequence;

    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在扫描 %1 帧，等待 AI 返回...").arg(sampleCount))));
    _setStatusMessage(zh(u8"正在批量扫描..."), kWarning);
    _refreshActionState();
}

// ============================================================
// 3. Review Report (ReportAgent)
// ============================================================

void AIAgentWorkspace::_submitReviewReport()
{
    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();

    AIWorkflowRequest request;
    request.workflow = AIWorkflowKind::GenerateSummary;
    request.scope = hasMedia ? AIRequestScope::CurrentReview : AIRequestScope::Custom;
    request.providerId = providerId;
    request.model = _requestModel();
    request.systemPrompt = zh(u8"你是一名资深审片主管。请根据所有已标注的问题和发现，"
                               "生成一份完整的审片总结报告。报告应包含：\n"
                               "1. 问题统计（严重/主要/次要数量）\n"
                               "2. 按类别分组的问题汇总\n"
                               "3. 建议修改优先级\n"
                               "4. 总体质量评估");
    const QString prompt = _promptEdit->toPlainText().trimmed();
    request.userPrompt = prompt.isEmpty()
        ? zh(u8"请生成完整的审片总结报告。")
        : prompt;
    request.attachFrames = false;
    request.includeAnnotations = true;
    request.sampleCount = 1;
    request.chatHistory = _trimmedChatHistory(6);
    request.options.insert(QStringLiteral("conversationContextSummary"), _buildConversationContextSummary(true));

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = ActiveJobMode::GenerateSummary;

    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在生成审片报告..."))));
    _setStatusMessage(zh(u8"正在生成报告..."), kWarning);
    _refreshActionState();
}

void AIAgentWorkspace::_exportReviewReport(const AIResponse& response)
{
    QVector<AnnotationItem> annotations;
    if (_annotationService) {
        annotations = _annotationService->annotations();
    }

    const QString defaultPath = QDir::homePath() + "/CGPlay_ReviewReport_" +
        QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".html";

    const QString filepath = QFileDialog::getSaveFileName(
        this, zh(u8"导出审片报告"), defaultPath,
        zh(u8"HTML 报告 (*.html);;JSON 数据 (*.json)"));

    if (filepath.isEmpty()) {
        return;
    }

    bool ok = false;
    if (filepath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        ok = ReviewExport::exportJson(annotations, filepath);
    } else {
        QString mediaTitle;
        IPlaybackService* playback = runtimePlaybackService(_playbackService);
        if (playback && playback->isValid()) {
            mediaTitle = QFileInfo(playback->currentPath()).fileName();
        }
        ok = ReviewExport::exportHtml(annotations, filepath, mediaTitle);
    }

    if (ok) {
        _setStatusMessage(zh(u8"报告已导出：%1").arg(filepath), kSuccess);
    } else {
        _setStatusMessage(zh(u8"导出失败"), kError);
    }
}

// ============================================================
// 4. QC Check (QCAgent)
// ============================================================

void AIAgentWorkspace::_submitQCScan()
{
    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();
    if (!hasMedia) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法检查"), zh(u8"请先打开媒体文件。")));
        return;
    }

    const QString customPrompt = _promptEdit->toPlainText().trimmed();
    const QStringList qcChecks = customPrompt.isEmpty()
        ? QStringList{
            zh(u8"色彩偏差：检查画面是否存在偏色、色阶断裂、色彩空间不匹配"),
            zh(u8"暗部噪点：检查暗部区域是否有明显噪点或 banding"),
            zh(u8"边缘锯齿：检查物体边缘是否有锯齿、追踪点残留"),
            zh(u8"穿帮检查：检查画面中是否有穿帮元素（线缆、标记点、支架等）"),
            zh(u8"合成问题：检查合成层边缘、抠像残留、匹配度"),
        }
        : QStringList{ customPrompt };

    QStringList qcList;
    for (int i = 0; i < qcChecks.size(); ++i) {
        qcList << QStringLiteral("%1. %2").arg(i + 1).arg(qcChecks[i]);
    }

    AIWorkflowRequest request;
    request.workflow = AIWorkflowKind::QCScan;
    request.scope = AIRequestScope::CurrentFrame;
    request.providerId = providerId;
    request.model = _requestModel();
    request.systemPrompt = zh(u8"你是一名专业 QC 检查员。请对当前帧执行以下质量检查：\n%1\n\n"
                               "每项检查输出格式：\n"
                               "检查项: [PASS/FAIL/WARNING]\n"
                               "说明: 简短描述\n").arg(qcList.join(QStringLiteral("\n")));
    request.userPrompt = zh(u8"请执行 QC 检查并报告结果。");
    request.attachFrames = true;
    request.requireFrames = true;
    request.includeAnnotations = true;
    request.sampleCount = 1;
    request.targetSize = QSize(1280, 720);
    request.chatHistory = _trimmedChatHistory(6);
    request.options.insert(QStringLiteral("conversationContextSummary"), _buildConversationContextSummary(true));
    request.options.insert(QStringLiteral("maxOutputTokens"), 2000);

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = ActiveJobMode::QCScan;

    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在执行 QC 检查（%1 项）...").arg(qcChecks.size()))));
    _setStatusMessage(zh(u8"正在执行 QC 检查..."), kWarning);
    _refreshActionState();
}

// ============================================================
// 5. Smart Search (SearchAgent)
// ============================================================

void AIAgentWorkspace::_submitSmartSearch()
{
    if (!_searchEdit) {
        return;
    }

    const QString query = _searchEdit->text().trimmed();
    if (query.isEmpty()) {
        _setStatusMessage(zh(u8"请输入搜索关键词"), kWarning);
        return;
    }

    QVector<AISearchResult> results;
    if (_workflowService) {
        results = _workflowService->searchAnnotations(query);
    }

    _lastSearchResults = results;
    _resultView->setHtml(_renderSearchResults(results));
    _setStatusMessage(
        results.isEmpty() ? zh(u8"未找到匹配结果") : zh(u8"找到 %1 条结果").arg(results.size()),
        results.isEmpty() ? kMuted : kSuccess);
}

QString AIAgentWorkspace::_renderSearchResults(const QVector<AISearchResult>& results) const
{
    QString html;
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>")
        .arg(kText, htmlEscape(zh(u8"搜索结果（%1 条）").arg(results.size())));

    if (results.isEmpty()) {
        html += QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"未找到匹配的批注或问题。试试其他关键词，如\"严重\"\"未解决\"\"色彩\"等。")));
        return html;
    }

    for (int i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        html += QStringLiteral(
            "<div style='margin:6px 0;padding:8px;border:1px solid rgba(255,255,255,0.06);border-radius:4px;'>"
            "<b style='color:%1;'>[%2]</b> %3"
            "<div style='color:%4;margin-top:2px;'>%5</div>"
            "<div style='color:%6;margin-top:2px;'>%7</div>"
            "<div style='margin-top:4px;'><a href='goto_frame_%8' style='color:%9;text-decoration:none;'>[跳转到帧 %8]</a></div>"
            "</div>")
            .arg(
                kAccent,
                htmlEscape(_severityText(r.severity)),
                htmlEscape(r.title),
                kMuted,
                htmlEscape(zh(u8"帧 %1 · 来源：%2").arg(QString::number(r.frame), r.source)),
                kText,
                htmlEscape(r.snippet),
                QString::number(r.frame),
                kAccent);
    }
    return html;
}

// ============================================================
// Annotation application (AnnotateAgent)
// ============================================================

void AIAgentWorkspace::_onResultAnchorClicked(const QUrl& url)
{
    const QString action = url.toString();

    if (action == QStringLiteral("apply_all_annotations")) {
        _applyAllAnnotationSuggestions();
        return;
    }

    if (action.startsWith(QStringLiteral("apply_annotation_"))) {
        const int index = action.mid(QStringLiteral("apply_annotation_").length()).toInt();
        _applyAnnotationSuggestion(index);
        return;
    }

    if (action.startsWith(QStringLiteral("goto_frame_"))) {
        const int frame = action.mid(QStringLiteral("goto_frame_").length()).toInt();
        IPlaybackService* playback = runtimePlaybackService(_playbackService);
        if (playback && playback->isValid()) {
            playback->seekToFrame(frame);
            _setStatusMessage(zh(u8"已跳转到帧 %1").arg(frame), kSuccess);
        }
        return;
    }
}

void AIAgentWorkspace::_applyAnnotationSuggestion(int index)
{
    if (!_annotationService || index < 0 || index >= _lastResponse.annotationSuggestions.size()) {
        _setStatusMessage(zh(u8"无法应用批注：索引无效"), kError);
        return;
    }

    const AIAnnotationSuggestion& sugg = _lastResponse.annotationSuggestions[index];

    AnnotationItem item;
    item.frame = sugg.frame;
    item.type = sugg.annotationType;
    item.color = sugg.color;
    item.comment = sugg.comment.isEmpty() ? sugg.title : sugg.comment;
    if (!sugg.points.isEmpty()) {
        item.points = sugg.points;
    } else {
        // Default to center of frame if no points provided
        const int w = _playbackService ? 1280 : 1280;
        const int h = _playbackService ? 720 : 720;
        item.points = { QPointF(w * 0.4, h * 0.4), QPointF(w * 0.6, h * 0.6) };
    }

    const QString id = _annotationService->addAnnotation(item);
    if (!id.isEmpty()) {
        _setStatusMessage(zh(u8"已应用批注：%1（帧 %2）").arg(sugg.title, QString::number(sugg.frame)), kSuccess);
    } else {
        _setStatusMessage(zh(u8"应用批注失败"), kError);
    }
}

void AIAgentWorkspace::_applyAllAnnotationSuggestions()
{
    if (!_annotationService || _lastResponse.annotationSuggestions.isEmpty()) {
        _setStatusMessage(zh(u8"没有可应用的批注建议"), kMuted);
        return;
    }

    int applied = 0;
    for (int i = 0; i < _lastResponse.annotationSuggestions.size(); ++i) {
        const AIAnnotationSuggestion& sugg = _lastResponse.annotationSuggestions[i];

        AnnotationItem item;
        item.frame = sugg.frame;
        item.type = sugg.annotationType;
        item.color = sugg.color;
        item.comment = sugg.comment.isEmpty() ? sugg.title : sugg.comment;
        if (!sugg.points.isEmpty()) {
            item.points = sugg.points;
        } else {
            item.points = { QPointF(512, 288), QPointF(768, 432) };
        }

        const QString id = _annotationService->addAnnotation(item);
        if (!id.isEmpty()) {
            ++applied;
        }
    }

    _setStatusMessage(zh(u8"已批量应用 %1 条批注").arg(applied), kSuccess);
}

// ============================================================
// 6. A/B Version Compare (CompareAgent)
// ============================================================

void AIAgentWorkspace::_submitCompareAB()
{
    if (!_workflowService) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"IAIWorkflowService 不可用。")));
        return;
    }

    const QString providerId = _selectedProviderId();
    if (providerId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法发送"), zh(u8"没有可用的 AI 服务。")));
        return;
    }

    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();
    if (!hasMedia) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"无法对比"), zh(u8"请先打开媒体文件。")));
        return;
    }

    AIWorkflowRequest request;
    request.workflow = AIWorkflowKind::CompareShots;
    request.scope = AIRequestScope::CompareAB;
    request.providerId = providerId;
    request.model = _requestModel();
    request.systemPrompt = zh(u8"你是一名影视审片主管，正在对比两个版本的画面。"
                               "请仔细分析两帧之间的差异，包括：\n"
                               "1. 色彩/亮度/对比度变化\n"
                               "2. 几何/构图变化（位移、缩放、旋转）\n"
                               "3. 特效/合成层差异\n"
                               "4. 新增或消失的元素\n"
                               "5. 整体质量是否改善或退化\n\n"
                               "输出格式：\n"
                               "差异类别 | 变化描述 | 严重程度\n"
                               "最后给出总结：版本B相比版本A是改善还是退化。");
    const QString prompt = _promptEdit->toPlainText().trimmed();
    request.userPrompt = prompt.isEmpty()
        ? zh(u8"请对比分析当前 A/B 两个版本的画面差异。")
        : prompt;
    request.attachFrames = true;
    request.requireFrames = true;
    request.includeAnnotations = true;
    request.sampleCount = 2;
    request.targetSize = QSize(1280, 720);
    request.chatHistory = _trimmedChatHistory(6);
    request.options.insert(QStringLiteral("conversationContextSummary"), _buildConversationContextSummary(true));

    _storeSelections();
    _activeJobId = _workflowService->submit(request);
    _activeJobMode = ActiveJobMode::CompareAB;

    if (_activeJobId.isEmpty()) {
        _resultView->setHtml(_renderFailureHtml(zh(u8"提交失败"), zh(u8"AI 工作流未返回 jobId。")));
        return;
    }

    _activeSubmittedPrompt = request.userPrompt;
    if (_promptEdit) {
        _promptEdit->clear();
    }
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"正在对比 A/B 版本，等待 AI 返回..."))));
    _setStatusMessage(zh(u8"正在 A/B 对比分析..."), kWarning);
    _refreshActionState();
}

// ============================================================
// 7. Problem Frame Navigation (SmartJumpAgent)
// ============================================================

void AIAgentWorkspace::_showProblemFrames()
{
    _resultView->setHtml(_renderProblemFramesHtml());

    const int count = _renderProblemFramesHtml().contains(QStringLiteral("goto_frame_")) ? 1 : 0;
    // Count actual problem frames
    int problemCount = 0;
    if (_annotationService) {
        const auto annotations = _annotationService->annotations();
        QSet<int> frames;
        for (const auto& ann : annotations) {
            frames.insert(ann.frame);
        }
        problemCount = frames.size();
    }
    // Add AI finding frames
    QSet<int> findingFrames;
    for (const auto& f : _lastResponse.findings) {
        findingFrames.insert(f.frame);
    }
    problemCount += findingFrames.size();

    if (problemCount == 0) {
        _setStatusMessage(zh(u8"当前没有问题帧"), kMuted);
    } else {
        _setStatusMessage(zh(u8"共 %1 个问题帧，点击跳转").arg(problemCount), kSuccess);
    }
}

QString AIAgentWorkspace::_renderProblemFramesHtml() const
{
    // Collect all problem frames from annotations + AI findings
    struct ProblemFrame {
        int frame = 0;
        QString source;
        QString title;
        QString snippet;
        AISeverity severity = AISeverity::Unknown;
    };

    QVector<ProblemFrame> problems;

    // From annotations
    if (_annotationService) {
        const auto annotations = _annotationService->annotations();
        for (const auto& ann : annotations) {
            ProblemFrame pf;
            pf.frame = ann.frame;
            pf.source = zh(u8"批注");
            pf.title = annotationTypeString(ann.type);
            pf.snippet = ann.latestCommentText();
            if (ann.status == ReviewStatus::Open) {
                pf.severity = AISeverity::Major;
            } else if (ann.status == ReviewStatus::InProgress) {
                pf.severity = AISeverity::Minor;
            } else {
                pf.severity = AISeverity::Info;
            }
            problems.append(pf);
        }
    }

    // From AI findings
    for (const auto& finding : _lastResponse.findings) {
        ProblemFrame pf;
        pf.frame = finding.frame;
        pf.source = zh(u8"AI 发现");
        pf.title = finding.title.isEmpty() ? zh(u8"未命名问题") : finding.title;
        pf.snippet = finding.description;
        pf.severity = finding.severity;
        problems.append(pf);
    }

    // From annotation suggestions (not yet applied)
    for (const auto& sugg : _lastResponse.annotationSuggestions) {
        ProblemFrame pf;
        pf.frame = sugg.frame;
        pf.source = zh(u8"AI 建议");
        pf.title = sugg.title.isEmpty() ? zh(u8"批注建议") : sugg.title;
        pf.snippet = sugg.comment;
        pf.severity = sugg.severity;
        problems.append(pf);
    }

    // Sort by frame number
    std::sort(problems.begin(), problems.end(),
        [](const ProblemFrame& a, const ProblemFrame& b) {
            return a.frame < b.frame;
        });

    QString html;
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>")
        .arg(kText, htmlEscape(zh(u8"问题帧导航（%1 个问题）").arg(problems.size())));

    if (problems.isEmpty()) {
        html += QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"当前没有标记的问题帧。"
                                        "运行 QC 检查或批量扫描后，AI 发现的问题会自动出现在这里。"
                                        "也可以先添加批注，然后点击此按钮快速跳转。")));
        return html;
    }

    // Group by frame
    int currentFrame = -1;
    for (const auto& pf : problems) {
        if (pf.frame != currentFrame) {
            if (currentFrame >= 0) {
                html += QStringLiteral("</div>");
            }
            currentFrame = pf.frame;
            html += QStringLiteral(
                "<div style='margin:6px 0;padding:8px;border:1px solid rgba(255,255,255,0.06);border-radius:4px;'>"
                "<a href='goto_frame_%1' style='color:%2;text-decoration:none;font-weight:600;font-size:14px;'>帧 %1 →</a>")
                .arg(QString::number(pf.frame), kAccent);
        }
        html += QStringLiteral(
            "<div style='margin-top:4px;padding-left:12px;'>"
            "<b style='color:%1;'>[%2]</b> "
            "<span style='color:%3;'>%4</span> "
            "<span style='color:%5;font-size:11px;'>(%6)</span>"
            "<div style='color:%7;margin-top:2px;'>%8</div>"
            "</div>")
            .arg(
                kAccent,
                htmlEscape(_severityText(pf.severity)),
                kText,
                htmlEscape(pf.title),
                kMuted,
                htmlEscape(pf.source),
                kMuted,
                htmlEscape(pf.snippet));
    }
    if (currentFrame >= 0) {
        html += QStringLiteral("</div>");
    }

    html += QStringLiteral(
        "<div style='margin-top:10px;color:%1;'>%2</div>")
        .arg(kMuted, htmlEscape(zh(u8"点击帧号可跳转到该帧。")));

    return html;
}

// ============================================================
// 8. Annotation Aggregation (AnnotationSummaryAgent)
// ============================================================

void AIAgentWorkspace::_showAnnotationSummary()
{
    _resultView->setHtml(_renderAnnotationSummaryHtml());

    if (!_annotationService || _annotationService->annotations().isEmpty()) {
        _setStatusMessage(zh(u8"当前没有批注可聚合"), kMuted);
    } else {
        _setStatusMessage(zh(u8"批注聚合已生成"), kSuccess);
    }
}

QString AIAgentWorkspace::_renderAnnotationSummaryHtml() const
{
    QString html;
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>")
        .arg(kText, htmlEscape(zh(u8"批注智能聚合")));

    if (!_annotationService) {
        html += QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"批注服务不可用。")));
        return html;
    }

    const auto annotations = _annotationService->annotations();
    if (annotations.isEmpty()) {
        html += QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"当前没有任何批注。"
                                        "可以通过 AI 分析后点击\"应用\"添加批注，"
                                        "或手动在画面上标注。")));
        return html;
    }

    // Statistics
    int openCount = 0;
    int inProgressCount = 0;
    int resolvedCount = 0;
    int criticalCount = 0;
    int majorCount = 0;
    int minorCount = 0;

    // Group by type
    QMap<AnnotationType, QVector<const AnnotationItem*>> byType;
    // Group by frame ranges
    QMap<int, QVector<const AnnotationItem*>> byFrame;

    for (const auto& ann : annotations) {
        byType[ann.type].append(&ann);
        byFrame[ann.frame].append(&ann);

        switch (ann.status) {
        case ReviewStatus::Open: ++openCount; break;
        case ReviewStatus::InProgress: ++inProgressCount; break;
        case ReviewStatus::Resolved: ++resolvedCount; break;
        default: break;
        }
    }

    // Summary header
    html += QStringLiteral(
        "<div style='margin-bottom:12px;padding:10px;border:1px solid rgba(255,255,255,0.06);border-radius:6px;background:#111418;'>"
        "<div style='font-weight:600;color:%1;margin-bottom:6px;'>%2</div>"
        "<div style='color:%3;'>%4</div>"
        "</div>")
        .arg(
            kText,
            htmlEscape(zh(u8"统计概览")),
            kMuted,
            htmlEscape(zh(u8"总批注：%1 条\n未解决：%2  进行中：%3  已解决：%4")
                .arg(annotations.size())
                .arg(openCount)
                .arg(inProgressCount)
                .arg(resolvedCount)));

    // By type
    html += QStringLiteral("<div style='font-weight:600;color:%1;margin-top:10px;margin-bottom:6px;'>%2</div>")
        .arg(kText, htmlEscape(zh(u8"按类型分组")));
    html += QStringLiteral("<ul>");
    for (auto it = byType.constBegin(); it != byType.constEnd(); ++it) {
        const QString typeName = annotationTypeString(it.key());
        const int count = it.value().size();
        const int typeOpen = std::count_if(it.value().begin(), it.value().end(),
            [](const AnnotationItem* a) { return a->status == ReviewStatus::Open; });
        html += QStringLiteral("<li style='margin-bottom:4px;'><b>%1</b>：%2 条")
            .arg(htmlEscape(typeName), QString::number(count));
        if (typeOpen > 0) {
            html += QStringLiteral(" <span style='color:%1;'>(%2 未解决)</span>")
                .arg(kError, QString::number(typeOpen));
        }
        html += QStringLiteral("</li>");
    }
    html += QStringLiteral("</ul>");

    // By frame (checklist)
    html += QStringLiteral("<div style='font-weight:600;color:%1;margin-top:10px;margin-bottom:6px;'>%2</div>")
        .arg(kText, htmlEscape(zh(u8"问题帧 Checklist（%1 帧）").arg(byFrame.size())));
    html += QStringLiteral("<ul>");
    for (auto it = byFrame.constBegin(); it != byFrame.constEnd(); ++it) {
        const int frame = it.key();
        const auto& items = it.value();
        const int frameOpen = std::count_if(items.begin(), items.end(),
            [](const AnnotationItem* a) { return a->status == ReviewStatus::Open; });

        QString statusIcon = frameOpen > 0
            ? QStringLiteral("<span style='color:%1;'>●</span>").arg(kError)
            : QStringLiteral("<span style='color:%1;'>✓</span>").arg(kSuccess);

        html += QStringLiteral("<li style='margin-bottom:6px;'>%1 <a href='goto_frame_%2' style='color:%3;text-decoration:none;font-weight:600;'>帧 %2</a> — %4 条批注")
            .arg(statusIcon, QString::number(frame), kAccent, QString::number(items.size()));

        // List each annotation under this frame
        for (const auto* ann : items) {
            QString statusColor = kMuted;
            QString statusText;
            switch (ann->status) {
            case ReviewStatus::Open: statusColor = kError; statusText = zh(u8"未解决"); break;
            case ReviewStatus::InProgress: statusColor = kWarning; statusText = zh(u8"进行中"); break;
            case ReviewStatus::Resolved: statusColor = kSuccess; statusText = zh(u8"已解决"); break;
            default: break;
            }
            html += QStringLiteral(
                "<div style='margin-left:16px;margin-top:2px;color:%1;'>"
                "· [%2] %3 <span style='color:%4;font-size:11px;'>(%5)</span>"
                "</div>")
                .arg(kText,
                    htmlEscape(annotationTypeString(ann->type)),
                    htmlEscape(ann->latestCommentText().left(80)),
                    statusColor,
                    htmlEscape(statusText));
        }
    }
    html += QStringLiteral("</ul>");

    // Action items
    if (openCount > 0) {
        html += QStringLiteral(
            "<div style='margin-top:10px;padding:8px;border:1px solid rgba(255,107,107,0.2);border-radius:6px;background:rgba(255,107,107,0.05);'>"
            "<b style='color:%1;'>%2</b><div style='color:%3;margin-top:4px;'>%4</div>"
            "</div>")
            .arg(kError,
                htmlEscape(zh(u8"待处理项目")),
                kMuted,
                htmlEscape(zh(u8"当前有 %1 条未解决批注，建议优先处理。点击上方帧号可跳转查看。").arg(openCount)));
    }

    return html;
}

} // namespace cgplay
