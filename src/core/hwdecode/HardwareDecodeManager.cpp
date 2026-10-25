// CGPlay HardwareDecodeManager.cpp
// v1.5 — 硬件解码探测与配置

#include "HardwareDecodeManager.h"

#include <QSettings>
#include <QDebug>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace cgplay {

HardwareDecodeManager::HardwareDecodeManager(QObject* parent)
    : QObject(parent)
{
    _loadSettings();
}

HardwareDecodeManager::~HardwareDecodeManager()
{
    _saveSettings();
}

// ─── 探测 ──────────────────────────────────────────────────────────
void HardwareDecodeManager::probe()
{
    _gpus.clear();
    _available = false;

#ifdef Q_OS_WIN
    _probeWindows();
#endif

    _probed    = true;
    // Adapter inventory does not establish decoder support. The bundled reader
    // never creates AVHWDeviceContext/get_format/download state.
    _available = false;

    Q_EMIT probeFinished(_available);
    qDebug() << "[HWDecode] Probe complete:"
             << _gpus.size() << "GPU(s), requested=" << requestedAccelName()
             << "actual=" << currentAccelName() << fallbackReason();
}

// ─── Windows GPU 探测 ─────────────────────────────────────────────
void HardwareDecodeManager::_probeWindows()
{
#ifdef Q_OS_WIN
    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    if (FAILED(hr) || !factory) {
        qWarning() << "[HWDecode] CreateDXGIFactory failed:" << hr;
        return;
    }

    UINT adapterIndex = 0;
    IDXGIAdapter* adapter = nullptr;

    while (factory->EnumAdapters(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        GpuInfo gpu;
        gpu.name   = QString::fromWCharArray(desc.Description);
        gpu.vramMb = desc.DedicatedVideoMemory / (1024ULL * 1024ULL);

        // Detect vendor
        if (desc.VendorId == 0x10DE) {
            gpu.vendor = "NVIDIA";
        } else if (desc.VendorId == 0x1002) {
            gpu.vendor = "AMD";
        } else if (desc.VendorId == 0x8086) {
            gpu.vendor = "Intel";
        } else if (desc.VendorId == 0x1414) {
            gpu.vendor = "Microsoft";
        } else {
            gpu.vendor = "Unknown";
        }

        // Do not infer a usable codec/device path from the vendor identifier.

        _gpus.push_back(gpu);

        // Primary adapter is typically index 0
        if (adapterIndex == 0) {
            _primaryGpu = adapterIndex;
        }

        adapter->Release();
        adapterIndex++;
    }

    factory->Release();

    // If no primary found, use index 0
    if (_gpus.size() > static_cast<size_t>(_primaryGpu))
        _primaryGpu = 0;

    qDebug() << "[HWDecode] Found" << _gpus.size() << "GPU(s):";
    for (size_t i = 0; i < _gpus.size(); ++i) {
        const auto& g = _gpus[i];
        QString caps;
        if (g.supportsNvdec)     caps += " NVDEC";
        if (g.supportsAmf)       caps += " AMF";
        if (g.supportsQuickSync) caps += " QuickSync";
        if (g.supportsD3d11va)   caps += " D3D11VA";
        if (g.supportsDxva2)     caps += " DXVA2";
        qDebug() << "  [" << i << "]" << g.name << "|" << g.vendor
                 << "|" << g.vramMb << "MB |" << caps;
    }
#endif
}

void HardwareDecodeManager::_probeCpu()
{
    // CPU 特性探测（预留扩展点）
}

// ─── 可用解码器列表 ──────────────────────────────────────────────
QStringList HardwareDecodeManager::availableAccelTypes() const
{
    return {hwAccelName(HwAccelType::None)};
}

QString HardwareDecodeManager::fallbackReason() const
{
    return _hwType == HwAccelType::None ? QString() :
        QStringLiteral("当前播放内核未接入硬件解码，已使用软件解码；保留原请求设置。");
}

// ─── 配置 ──────────────────────────────────────────────────────────
void HardwareDecodeManager::setHwAccelType(HwAccelType t)
{
    if (static_cast<int>(t) < 0 || t > HwAccelType::Auto) return;
    if (_hwType == t) return;
    _hwType = t;
    _saveSettings();
    Q_EMIT hwAccelChanged(t);
}

// ─── 统计 ──────────────────────────────────────────────────────────
void HardwareDecodeManager::recordFrameDecoded(bool hw)
{
    if (hw) _hwFrames++;
    else    _swFrames++;
}

// ─── 持久化 ────────────────────────────────────────────────────────
void HardwareDecodeManager::_loadSettings()
{
    const QString application = QCoreApplication::applicationName();
    QSettings s("CGPlay", application.startsWith("CGPlayAutomation_") ? application : "CGPlay");
    int hw = s.value("hwdecode/type", static_cast<int>(HwAccelType::Auto)).toInt();
    if (hw >= 0 && hw <= static_cast<int>(HwAccelType::Auto))
        _hwType = static_cast<HwAccelType>(hw);
}

void HardwareDecodeManager::_saveSettings()
{
    const QString application = QCoreApplication::applicationName();
    QSettings s("CGPlay", application.startsWith("CGPlayAutomation_") ? application : "CGPlay");
    s.setValue("hwdecode/type", static_cast<int>(_hwType));
}

} // namespace cgplay
