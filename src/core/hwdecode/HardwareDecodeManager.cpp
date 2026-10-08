// CGPlay HardwareDecodeManager.cpp
// v1.5 — 硬件解码探测与配置

#include "HardwareDecodeManager.h"

#include <QSettings>
#include <QDebug>

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
    _available = !_gpus.empty();

    // 如果没有 GPU 支持，回退到纯软件
    if (!_available) {
        _hwType = HwAccelType::None;
    }

    Q_EMIT probeFinished(_available);
    qDebug() << "[HWDecode] Probe complete:"
             << _gpus.size() << "GPU(s), hwAccel=" << currentAccelName();
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
            gpu.supportsNvdec = true;    // NVIDIA GPU supports NVDEC
        } else if (desc.VendorId == 0x1002) {
            gpu.vendor = "AMD";
            gpu.supportsAmf = true;
        } else if (desc.VendorId == 0x8086) {
            gpu.vendor = "Intel";
            gpu.supportsQuickSync = true;
        } else if (desc.VendorId == 0x1414) {
            gpu.vendor = "Microsoft";
        } else {
            gpu.vendor = "Unknown";
        }

        // All modern Windows GPUs support D3D11VA and DXVA2
        gpu.supportsD3d11va = true;
        gpu.supportsDxva2   = true;

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
    QStringList list;
    list << hwAccelName(HwAccelType::None);
    list << hwAccelName(HwAccelType::Auto);

    // Always available on Windows
    list << hwAccelName(HwAccelType::D3D11VA);
    list << hwAccelName(HwAccelType::DXVA2);

    // Check GPU-specific decoders
    for (const auto& g : _gpus) {
        if (g.supportsNvdec && !list.contains(hwAccelName(HwAccelType::NVDEC)))
            list << hwAccelName(HwAccelType::NVDEC);
        if (g.supportsAmf && !list.contains(hwAccelName(HwAccelType::AMF)))
            list << hwAccelName(HwAccelType::AMF);
        if (g.supportsQuickSync && !list.contains(hwAccelName(HwAccelType::QuickSync)))
            list << hwAccelName(HwAccelType::QuickSync);
    }

    return list;
}

// ─── 配置 ──────────────────────────────────────────────────────────
void HardwareDecodeManager::setHwAccelType(HwAccelType t)
{
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
    QSettings s("CGPlay", "CGPlay");
    int hw = s.value("hwdecode/type", static_cast<int>(HwAccelType::Auto)).toInt();
    if (hw >= 0 && hw <= static_cast<int>(HwAccelType::Auto))
        _hwType = static_cast<HwAccelType>(hw);
}

void HardwareDecodeManager::_saveSettings()
{
    QSettings s("CGPlay", "CGPlay");
    s.setValue("hwdecode/type", static_cast<int>(_hwType));
}

} // namespace cgplay
