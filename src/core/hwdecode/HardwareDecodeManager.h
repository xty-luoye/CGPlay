#pragma once
// CGPlay HardwareDecodeManager.h
// v1.5 — 硬件解码探测、配置、状态

#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>

namespace cgplay {

// ─── 硬件解码类型 ────────────────────────────────────────────────
enum class HwAccelType
{
    None,           // 软件解码
    D3D11VA,        // Windows Direct3D 11 Video Acceleration
    DXVA2,          // Windows DirectX Video Acceleration 2
    NVDEC,          // NVIDIA NVDEC (CUDA)
    AMF,            // AMD Advanced Media Framework
    QuickSync,      // Intel Quick Sync Video
    Auto            // 自动选择最佳可用
};

inline QString hwAccelName(HwAccelType t)
{
    switch (t) {
    case HwAccelType::None:      return "Software (CPU)";
    case HwAccelType::D3D11VA:   return "Direct3D 11 VA";
    case HwAccelType::DXVA2:     return "DXVA 2.0";
    case HwAccelType::NVDEC:     return "NVIDIA NVDEC";
    case HwAccelType::AMF:       return "AMD AMF";
    case HwAccelType::QuickSync: return "Intel QuickSync";
    case HwAccelType::Auto:      return "Auto (Recommended)";
    default: return "Unknown";
    }
}

inline QString hwAccelFFmpegName(HwAccelType t)
{
    switch (t) {
    case HwAccelType::D3D11VA:   return "d3d11va";
    case HwAccelType::DXVA2:     return "dxva2";
    case HwAccelType::NVDEC:     return "cuda";
    case HwAccelType::AMF:       return "amf";
    case HwAccelType::QuickSync: return "qsv";
    case HwAccelType::Auto:      return "auto";
    default: return "";
    }
}

// ─── GPU 信息 ─────────────────────────────────────────────────────
struct GpuInfo
{
    QString  name;          // GPU 名称
    QString  vendor;        // "NVIDIA", "AMD", "Intel", "Microsoft"
    bool     supportsNvdec    = false;
    bool     supportsAmf      = false;
    bool     supportsQuickSync = false;
    bool     supportsD3d11va  = false;
    bool     supportsDxva2    = false;
    unsigned long long vramMb = 0;
};

// ─── HardwareDecodeManager ────────────────────────────────────────
class HardwareDecodeManager : public QObject
{
    Q_OBJECT
public:
    explicit HardwareDecodeManager(QObject* parent = nullptr);
    ~HardwareDecodeManager() override;

    // ── 探测 ──────────────────────────────────────────────────────
    void probe();                           // 探测 CPU + GPU 能力
    bool isAvailable() const { return _available; }
    bool isProbed()     const { return _probed; }

    // ── GPU 信息 ──────────────────────────────────────────────────
    const std::vector<GpuInfo>& gpus() const { return _gpus; }
    int primaryGpuIndex()       const { return _primaryGpu; }

    // ── 配置 ──────────────────────────────────────────────────────
    HwAccelType hwAccelType()          const { return _hwType; }
    void        setHwAccelType(HwAccelType t);
    bool        isEnabled()            const { return _hwType != HwAccelType::None; }

    // ── 可用硬件解码器列表 ──────────────────────────────────────
    QStringList availableAccelTypes()  const;
    QString     currentAccelName()     const { return hwAccelName(_hwType); }
    QString     currentAccelFFmpeg()   const { return hwAccelFFmpegName(_hwType); }

    // ── 统计 (运行时) ────────────────────────────────────────────
    void  recordFrameDecoded(bool hw = true);   // API 保留
    int   hwDecodedFrames()    const { return _hwFrames; }
    int   swDecodedFrames()    const { return _swFrames; }
    float hwDecodeRatio()      const {
        int total = _hwFrames + _swFrames;
        return total > 0 ? float(_hwFrames) / float(total) : 0.f;
    }

Q_SIGNALS:
    void probeFinished(bool available);
    void hwAccelChanged(HwAccelType type);

private:
    void _probeWindows();    // D3D/DXGI GPU 探测
    void _probeCpu();        // CPU 特性探测
    void _loadSettings();
    void _saveSettings();

    std::vector<GpuInfo> _gpus;
    int  _primaryGpu = 0;
    bool _probed     = false;
    bool _available  = false;

    HwAccelType _hwType = HwAccelType::Auto;

    // Runtime stats
    int _hwFrames = 0;
    int _swFrames = 0;
};

} // namespace cgplay
