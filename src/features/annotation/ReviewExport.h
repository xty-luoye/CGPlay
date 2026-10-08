#pragma once
// CGPlay ReviewExport.h — 批注导出：JSON / HTML / 视频导出 / 视频烧录批注

#include <QString>
#include <QVector>
#include <QMap>
#include <QImage>
#include <functional>
#include <atomic>
#include <utility>

// OCIO options — use the real tlRender header when available, otherwise stub
#if __has_include(<tlRender/Timeline/ColorOptions.h>)
  #include <tlRender/Timeline/ColorOptions.h>
#elif defined(CGPLAY_HAS_TLRENDER) && CGPLAY_HAS_TLRENDER
  #include <tlRender/Timeline/ColorOptions.h>
#else
  namespace tl {
    enum class OCIOConfig { File, EnvVar, BuiltIn, Count, First = BuiltIn };
    struct OCIOOptions {
        bool        enabled  = false;
        OCIOConfig  config   = OCIOConfig::File;
        std::string fileName;
        std::string input;
        std::string display;
        std::string view;
        bool operator==(const OCIOOptions&) const { return true; }
        bool operator!=(const OCIOOptions&) const { return false; }
    };
  }
#endif

class QWidget;

namespace cgplay {

struct AnnotationItem;

enum class VideoExportCodec
{
    H264,
    H265,
    ProRes422HQ,
    ProRes4444
};

QString videoExportCodecId(VideoExportCodec codec);

class ReviewExport
{
public:
    // ── JSON 导出 ─────────────────────────────────────────────────────
    static bool exportJson(const QVector<AnnotationItem>& annotations,
                           const QString& filepath);

    // ── HTML Report 导出 ──────────────────────────────────────────────
    static bool exportHtml(const QVector<AnnotationItem>& annotations,
                           const QString& filepath,
                           const QString& mediaTitle = {});

    // ── 探测源文件帧率 ──────────────────────────────────────────────
    static double detectFps(const QString& srcMedia);

    // ── 探测源文件帧数 ──────────────────────────────────────────────
    static int  detectFrameCount(const QString& srcMedia);

    // ── 判断是否为单帧图像格式 (EXR/PNG/DPX/TIFF/JPG) ───────────────
    static bool isStillImage(const QString& path);

    // ── 为图像序列构建 ffmpeg glob 输入（如 render.0001.exr → render_*.exr）───
    // 返回空字符串表示此文件不是序列的一部分
    static QString buildSequenceGlob(const QString& path);

    // ── 纯视频导出 (不烧录批注，选帧范围 → 转码) ───────────────────
    static bool exportVideoClean(const QString& srcMedia,
                                 const QString& outVideo,
                                 std::pair<int, int> frameRange,
                                 double fps = 0,
                                 VideoExportCodec codec = VideoExportCodec::H264,
                                 std::function<void(double)> progress = {},
                                 const std::atomic_bool* cancel = nullptr);

    // ── 视频烧录批注导出 (需要ffmpeg) ────────────────────────────────
    static bool exportVideoAnnotated(const QVector<AnnotationItem>& annotations,
                                     const QString& srcMedia,
                                     const QString& outVideo,
                                     int mediaW, int mediaH,
                                     std::pair<int, int> frameRange,
                                     double fps = 0,
                                     const tl::OCIOOptions& ocio = {},
                                     VideoExportCodec codec = VideoExportCodec::H264,
                                     std::function<void(double)> progress = {},
                                     const std::atomic_bool* cancel = nullptr);

    // ── ffmpeg 管理 ──────────────────────────────────────────────────
    // 查找 ffmpeg: 程序目录 → PATH
    static QString locateFfmpeg();
    // 检查 ffmpeg 是否可用
    static bool hasFfmpeg();
    // 确保 ffmpeg 可用，不可用时尝试自动下载
    // parentWidget 用于进度对话框
    static bool ensureFfmpeg(QWidget* parentWidget);

    // ── 获取最后一次导出失败的详细错误（用于 UI 显示）──────────────────
    static QString lastError();

private:
    static bool _downloadFfmpeg(QWidget* parentWidget);
    static QString _ffmpegDir();

    // ── OCIO CPU 处理 ────────────────────────────────────────────────
    static bool applyOcioToImage(QImage& img,
                                 const tl::OCIOOptions& ocio);
};

} // namespace cgplay
