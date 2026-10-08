# AI protocol contract checks

This standalone Qt test compiles the real discovery, validator, provider and connection-wizard code. It uses loopback HTTP servers and dummy credentials only; it does not call cloud APIs or require an account. Qt runs with the offscreen platform, and the report must contain zero visible top-level windows.

From the repository root, configure with your Qt 6.5+ CMake path:

```powershell
cmake -S tests/ai_protocol_contracts -B tests/artifacts/ai_protocol_contracts/build -DQt6_DIR=<Qt>/lib/cmake/Qt6
cmake --build tests/artifacts/ai_protocol_contracts/build --config Release
$env:PATH = '<Qt>/bin;' + $env:PATH
$env:QT_QPA_PLATFORM_PLUGIN_PATH = '<Qt>/plugins/platforms'
& tests/artifacts/ai_protocol_contracts/build/Release/ai_protocol_contracts.exe tests/artifacts/ai_protocol_contracts/report.json
```

The process returns nonzero when an assertion fails. Coverage includes Responses and Chat Completions token limits, native Gemini/Anthropic/Ollama request schemas, Qwen/DeepSeek compatible requests, model discovery pagination, model ID preservation, malformed responses, redirect credential isolation, and manual model entry/save through the real connection widget.

Protocol references checked on 2026-10-08:

- https://developers.openai.com/api/reference/python/resources/responses/methods/create
- https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create
- https://api-docs.deepseek.com/api/create-chat-completion/
- https://www.alibabacloud.com/help/en/model-studio/qwen-api-via-openai-chat-completions
- https://ai.google.dev/api/generate-content
- https://ai.google.dev/api/models
- https://platform.claude.com/docs/en/api/models/list
- https://doc.qt.io/qt-6/qnetworkrequest.html#RedirectPolicy-enum

Live availability and permissions for a model still depend on the user's provider/account. These checks verify protocol behavior, not a paid inference against every model.
