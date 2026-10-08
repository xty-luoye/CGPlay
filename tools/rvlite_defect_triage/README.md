# RVLite 离线缺陷分流器

这是一个不接入 RVLite 主程序的最小独立工具。它接收缺陷文字和可选截图，优先让本机 Ollama 的 `clef-flash` 做一次结构化决策；Ollama 不可用、版本过旧、模型未安装、截图不可读或响应不符合 schema 时，自动切换到确定性的本地关键词规则，并把降级原因写入 JSON。

## 已核对的 Ollama/Clef Flash API

核对日期：2026-10-06。Clef Flash 模型页说明它是 9B 多模态决策模型，需要 Ollama **0.35.1 或更高**，模型拉取命令是 `ollama pull clef-flash`。它使用 Ollama 的 `POST /v1/systemone`，不是普通的 `/api/chat`：

- `state` 必填，可以是字符串或 JSON 对象；本工具发送 `description` 和 `screenshot_attached`。
- `questions` 支持 `choice`、`noul`、`score`。本工具一次发送一个模块 `choice` 和一个严重度 `score`。
- 截图放在 `images` 数组中，必须是 PNG/JPEG/WebP 的**原始 base64**；不接受 URL 或 `data:` URL。
- 含图请求（base64 加 JSON）上限约 32 MiB；本工具把单张原图限制为 20 MiB，并对文字和组合请求提前做大小检查。
- `choice` 返回 `choice`、`probabilities`、`confidence`；`score` 返回加权 `score`、`probabilities`、`confidence`。`confidence` 表示概率分布集中度，不等于正确率。
- 本地调用不需要 API key。工具会先调用 `GET /api/version` 和 `GET /api/tags`，确认版本和模型，再调用 `/v1/systemone`。

原始资料：

- [Clef Flash 模型页](https://ollama.com/library/clef-flash:9b)
- [Ollama Decision 指南](https://docs.ollama.com/capabilities/decision)
- [System One API](https://docs.ollama.com/api/systemone)
- [Ollama v0.35.1 发布说明](https://github.com/ollama/ollama/releases/tag/v0.35.1)

## 安装和启动

只需要 Python 3.11+ 和本机 Ollama，不需要 Python 第三方包。

```powershell
# 终端 A（Ollama 已作为桌面服务运行时可跳过）
ollama serve

# 终端 B
ollama pull clef-flash

py -3.11 tools/rvlite_defect_triage/rvlite_triage.py `
  --text "播放 EXR 序列时每隔几帧卡顿并掉帧" `
  --image "C:\path\to\screenshot.png" `
  --output "tests/artifacts/rvlite_defect_triage/example.json"
```

模型未安装或 Ollama 没有运行时，最后一条命令仍会生成 JSON，只是 `model.mode` 为 `heuristic`。完全离线测试可以显式加 `--force-heuristic`：

```powershell
py -3.11 tools/rvlite_defect_triage/rvlite_triage.py `
  --text "Qt 菜单快捷键失效" `
  --force-heuristic `
  --output "triage.json"
```

可用参数：`--ollama-url`（默认 `http://127.0.0.1:11434`）、`--model`（默认 `clef-flash`）、`--timeout`（默认 30 秒）、`--image`、`--output`。

## 分类 schema

结果顶层包含 `schema_version`、`created_at`、`input`、`model`、`triage`、`raw_answers` 和 `errors`。`triage` 的稳定字段如下：

| 字段 | 类型 | 允许值/含义 |
| --- | --- | --- |
| `module` | string | `playback`、`video_export`、`exr_color`、`qt_ui`、`other` |
| `severity` | string | `blocker`、`high`、`medium`、`low`、`unknown` |
| `confidence` | number | 0 到 1，取模块和严重度置信度的较小值 |
| `human_confirmation_required` | boolean | 是否应由人工复核 |
| `human_confirmation_reasons` | string[] | 低置信度、概率接近、缺少视觉证据、跨模块信号、模型降级等原因 |

`model.mode` 是 `ollama` 或 `heuristic`。图片的 base64 永远不会写入输出文件，只保存路径、MIME 和 SHA-256。

人工确认会在以下情况触发：任一置信度低于 0.65、前两项概率差小于 0.15、模块为 `other`、文字提到截图/颜色/界面等视觉证据但没有截图、检测到多个模块信号，或使用了本地降级规则。降级 JSON 的 `errors` 会保留稳定的 `code`，例如 `ollama_unavailable`、`ollama_version_too_old`、`model_not_installed`、`invalid_model_response`、`image_read_error`、`request_too_large`。

## RVLite 示例

下面的结果是分流目标示例；实际置信度由 Clef Flash 或降级规则计算：

| 缺陷描述 | 预期模块 | 严重度建议 |
| --- | --- | --- |
| 播放 EXR 序列时每隔几帧卡顿并掉帧 | `playback` | `high` |
| 导出 H.264 后画面整体偏粉 | `video_export` | `high` |
| OCIO 视图变换后 Gamma 明显不一致 | `exr_color` | `medium` |
| Qt 菜单快捷键失效，鼠标点击仍可用 | `qt_ui` | `medium` |
| 打开 8K AV1 文件时程序直接崩溃 | `playback` | `blocker` |

## 降级处理

降级规则不上传文本或截图：它按固定关键词计数选择模块，并按“崩溃/无法启动/数据丢失 → blocker，功能失败 → high，影响但可绕过 → medium，轻微外观/文案 → low”的顺序给出建议。没有足够证据时返回 `other` 或 `unknown`，置信度保持在 0.2–0.4，并要求人工确认。这样即使本机没有 Ollama，分流器也能生成可审计的 JSON，而不会伪装成模型结论。

HTTP 错误会保留在 `errors` 中，并使用 `ollama_api_error` 作为人工确认原因；连接失败和无效 JSON 则使用 `ollama_unavailable`。

## 运行测试

测试只使用标准库和本地 mock HTTP 服务，不启动可见窗口，也不要求 Ollama：

```powershell
py -3.11 -m unittest tests.test_rvlite_defect_triage -v
```

