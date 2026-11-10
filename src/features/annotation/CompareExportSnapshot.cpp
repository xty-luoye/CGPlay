#include "CompareExportSnapshot.h"

#include "playback/api/IPlaybackService.h"
#include "ocio/OcioManager.h"
#include "viewer/CompareToolbar.h"
#include "viewer/TlViewport.h"

#include <tlRender/Timeline/Timeline.h>
#include <tlRender/Core/Version.h>
#include <tlRender/IO/IO.h>
#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cgplay {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

QString number(double value)
{
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite compare snapshot value");
    return QString::number(value, 'g', 17);
}

QString modeName(tl::Compare value)
{
    switch (value) {
    case tl::Compare::First: return QStringLiteral("a");
    case tl::Compare::B: return QStringLiteral("b");
    case tl::Compare::Wipe: return QStringLiteral("wipe");
    case tl::Compare::Overlay: return QStringLiteral("overlay");
    case tl::Compare::Difference: return QStringLiteral("difference");
    case tl::Compare::Horizontal: return QStringLiteral("horizontal");
    case tl::Compare::Vertical: return QStringLiteral("vertical");
    case tl::Compare::Tile: return QStringLiteral("tile");
    default: throw std::runtime_error("Unsupported compare mode");
    }
}

tl::Compare nativeMode(const QString& value)
{
    for (const auto mode : {tl::Compare::First, tl::Compare::B, tl::Compare::Wipe, tl::Compare::Overlay,
                           tl::Compare::Difference, tl::Compare::Horizontal, tl::Compare::Vertical, tl::Compare::Tile}) {
        if (modeName(mode) == value) return mode;
    }
    throw std::runtime_error("Unknown compare export mode");
}

bool dataLayer(const QString& name)
{
    const QStringList data{QStringLiteral("n"), QStringLiteral("p"), QStringLiteral("z"),
        QStringLiteral("normal"), QStringLiteral("normals"), QStringLiteral("position"),
        QStringLiteral("depth"), QStringLiteral("velocity"), QStringLiteral("motion"),
        QStringLiteral("motionvector"), QStringLiteral("motionvectors"), QStringLiteral("uv"),
        QStringLiteral("id"), QStringLiteral("objectid"), QStringLiteral("materialid"),
        QStringLiteral("mask"), QStringLiteral("disparity")};
    for (const QString& token : name.toLower().split(QRegularExpression(QStringLiteral("[._/\\s-]+")), Qt::SkipEmptyParts)) {
        if (data.contains(token) || token.startsWith(QStringLiteral("crypto"))) return true;
    }
    return false;
}

bool imagePath(const QString& suffix)
{
    return QStringList{QStringLiteral("exr"), QStringLiteral("png"), QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("dpx"), QStringLiteral("tif"), QStringLiteral("tiff"),
        QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("hdr"), QStringLiteral("pfm"),
        QStringLiteral("cin"), QStringLiteral("sgi"), QStringLiteral("iff"), QStringLiteral("tga")}.contains(suffix);
}

void validateDisplay(const tl::DisplayOptions& display, const ColorTransformSnapshot& color)
{
    if (display.channels != ftk::ChannelDisplay::Color)
        throw std::runtime_error("对比视频导出目前只支持 RGB 颜色显示，请先退出单通道/Alpha 显示。");
    if (display.mirror.x || display.mirror.y)
        throw std::runtime_error("当前镜像显示尚不能用于对比导出，请先关闭镜像。");
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
    if (display.negative)
        throw std::runtime_error("当前负片显示尚不能用于对比导出，请先关闭。");
#else
    if (display.imageFilters != ftk::ImageFilters())
        throw std::runtime_error("当前自定义图像过滤方式尚不能用于对比导出。");
#endif
    if (display.exposure.enabled || display.softClip.enabled)
        throw std::runtime_error("当前额外曝光曲线/Soft Clip 尚不能用于对比导出。");
    tl::Color expectedColor;
    const float gain = std::exp2(color.exposure);
    expectedColor.enabled = color.exposure != 0.F;
    expectedColor.brightness = ftk::V3F(gain, gain, gain);
    if (display.color.enabled != expectedColor.enabled ||
        (display.color.enabled && display.color != expectedColor))
        throw std::runtime_error("当前颜色分级不能由导出颜色快照表示，请先恢复标准曝光设置。");
    tl::Levels expectedLevels;
    expectedLevels.enabled = color.gamma != 1.F;
    expectedLevels.gamma = color.gamma;
    if (display.levels.enabled != expectedLevels.enabled ||
        (display.levels.enabled && display.levels != expectedLevels))
        throw std::runtime_error("当前 Levels 分级不能由导出颜色快照表示，请先恢复标准 Gamma 设置。");
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
    const auto& aspect = display.aspectRatio;
    if (!std::isfinite(aspect.value.num) || !std::isfinite(aspect.value.den) ||
        !((aspect.value.num == 0.F && aspect.value.den == 0.F) || aspect.value.isValid()) ||
        (aspect.type != tl::AspectRatioType::Pixel && aspect.type != tl::AspectRatioType::Display))
#else
    if (!std::isfinite(display.aspectRatio) || display.aspectRatio < 0.F)
#endif
        throw std::runtime_error("当前显示宽高比无效。");
}

QJsonValue layoutAspectValue(const tl::DisplayOptions& display)
{
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
    const nlohmann::json value = display.aspectRatio;
    return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).object();
#else
    return display.aspectRatio;
#endif
}

QJsonObject sourceValue(const ftk::Path& path, const ftk::Path& audioPath, const OTIO_NS::TimeRange& range,
    const tl::IOInfo& io, int layer, const ftk::ImageOptions& image,
    const tl::DisplayOptions& display, const ColorTransformSnapshot& color)
{
    if (layer < 0 || static_cast<size_t>(layer) >= io.video.size())
        throw std::runtime_error("当前视频层不在实际已加载源的范围内。");
    const auto& info = io.video[layer];
    if (info.size.w <= 0 || info.size.h <= 0 || !std::isfinite(info.pixelAspectRatio) || info.pixelAspectRatio <= 0.F)
        throw std::runtime_error("当前源图像尺寸或像素宽高比无效。");
    if (ftk::getChannelCount(info.type) < 3 || dataLayer(QString::fromStdString(info.name)))
        throw std::runtime_error("数据 AOV/非 RGB 视频层不能当作颜色导出。");
    if (image.channelDisplay != ftk::ChannelDisplay::Color || image.videoLevels != ftk::InputVideoLevels::FromFile ||
        image.imageFilters != ftk::ImageFilters())
        throw std::runtime_error("当前通道、视频范围覆盖或自定义图像过滤尚不能用于对比导出。");
    const QString suffix = QString::fromStdString(path.getExt()).toLower().remove(QChar('.'));
    if (suffix == QStringLiteral("otio") || suffix == QStringLiteral("otioz"))
        throw std::runtime_error("对比导出当前需要实际视频文件或图像序列，不能直接导出 OTIO 多片段时间线。");
    const bool sequence = imagePath(suffix);
    if (io.video.size() != 1)
        throw std::runtime_error("当前对比导出尚未确认多视频流/多 part 层映射，请先使用单 RGB 视频流或单颜色层图像。");
    QString filename = QString::fromStdString(path.get());
    if (sequence && path.getFrames())
        filename = QString::fromStdString(path.getFrame(path.getFrames()->min(), true));
    else if (sequence && path.hasSeqWildcard())
        throw std::runtime_error("图像序列路径尚未解析到实际首帧。");
    if (filename.isEmpty()) throw std::runtime_error("当前源路径为空。");
    const double rate = range.duration().rate();
    const double length = range.duration().value();
    if (!std::isfinite(rate) || rate <= 0.0 || !std::isfinite(length) || length <= 0.0 || length > 9007199254740991.0)
        throw std::runtime_error("当前源时间范围或真实帧率无效。");
    const double count = std::ceil(length);
    const QString alpha = ftk::getChannelCount(info.type) == 3 || image.alphaBlend == ftk::AlphaBlend::None
        ? QStringLiteral("opaque")
        : image.alphaBlend == ftk::AlphaBlend::Premultiplied ? QStringLiteral("premultiplied") : QStringLiteral("straight");
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
    const double pixelAspect = static_cast<double>(tl::getAspectRatio(info, display.aspectRatio)) * info.size.h / info.size.w;
#else
    const double pixelAspect = display.aspectRatio > 0.F
        ? static_cast<double>(display.aspectRatio) * info.size.h / info.size.w : info.pixelAspectRatio;
#endif
    if (!std::isfinite(pixelAspect) || pixelAspect <= 0.0)
        throw std::runtime_error("当前显示像素宽高比无效。");
    QJsonObject source{
        {QStringLiteral("kind"), sequence ? QStringLiteral("sequence") : QStringLiteral("video")},
        {QStringLiteral("path"), filename},
        {QStringLiteral("timelinePath"), QString::fromStdString(path.get())},
        {QStringLiteral("width"), info.size.w}, {QStringLiteral("height"), info.size.h},
        {QStringLiteral("pixelAspect"), number(pixelAspect)},
        {QStringLiteral("nativePixelAspect"), number(info.pixelAspectRatio)},
        {QStringLiteral("encoding"), suffix == QStringLiteral("exr") || suffix == QStringLiteral("hdr") || suffix == QStringLiteral("pfm")
            ? QStringLiteral("scene-linear") : QStringLiteral("display")},
        {QStringLiteral("alphaMode"), alpha}, {QStringLiteral("rotationCw"), 0},
        {QStringLiteral("videoStream"), sequence ? 0 : layer}, {QStringLiteral("audioStream"), 0},
        {QStringLiteral("audioAvailable"), !sequence && io.audio.isValid() && (audioPath.isEmpty() || audioPath.get() == path.get())},
        {QStringLiteral("audioUnavailableReason"), !audioPath.isEmpty() && audioPath.get() != path.get()
            ? QStringLiteral("当前对比导出只支持源文件内嵌音轨，外部音轨尚未接入。") : QString()},
        {QStringLiteral("layerName"), QString::fromStdString(info.name)},
        {QStringLiteral("imageType"), QString::fromStdString(ftk::to_string(info.type))},
        {QStringLiteral("color"), color.toJson()},
        {QStringLiteral("clock"), QJsonObject{{QStringLiteral("fps"), number(rate)},
            {QStringLiteral("timelineStartFrames"), number(range.start_time().rescaled_to(rate).value())},
            {QStringLiteral("frameCount"), count}}}
    };
    return source;
}

} // namespace

bool updateCompareExportLayout(QJsonObject* job, const QString& mode, QString* error)
{
    if (error) error->clear();
    if (!job) return fail(error, QStringLiteral("导出任务为空。"));
    try {
        const tl::Compare compare = nativeMode(mode);
        const QJsonObject sources = job->value(QStringLiteral("sources")).toObject();
        std::vector<ftk::ImageInfo> infos;
        for (const QString& key : {QStringLiteral("a"), QStringLiteral("b")}) {
            const QJsonObject source = sources.value(key).toObject();
            ftk::ImageInfo info(source.value(QStringLiteral("width")).toInt(),
                                source.value(QStringLiteral("height")).toInt(), ftk::ImageType::RGB_U8);
            bool ok = false;
            info.pixelAspectRatio = source.value(QStringLiteral("nativePixelAspect")).toString().toFloat(&ok);
            if (!ok || info.size.w <= 0 || info.size.h <= 0 || !std::isfinite(info.pixelAspectRatio) || info.pixelAspectRatio <= 0.F)
                return fail(error, QStringLiteral("冻结源的布局尺寸无效。"));
            infos.push_back(info);
        }
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
        const QJsonValue aspectValue = job->value(QStringLiteral("layoutAspectOverride"));
        if (!aspectValue.isObject()) return fail(error, QStringLiteral("冻结源缺少显示宽高比设置。"));
        const auto aspect = nlohmann::json::parse(QJsonDocument(aspectValue.toObject()).toJson(QJsonDocument::Compact).toStdString())
            .get<tl::AspectRatioOptions>();
        tl::DisplayOptions layoutDisplay;
        layoutDisplay.aspectRatio = aspect;
        validateDisplay(layoutDisplay, ColorTransformSnapshot());
        tl::CompareOptions layoutOptions;
        layoutOptions.compare = compare;
        const auto boxes = tl::getBoxes(layoutOptions, aspect, infos);
        const auto size = tl::getRenderSize(layoutOptions, aspect, infos);
#else
        const float aspect = static_cast<float>(job->value(QStringLiteral("layoutAspectOverride")).toDouble());
        const auto boxes = tl::getBoxes(compare, infos, aspect);
        const auto size = tl::getRenderSize(compare, infos, aspect);
#endif
        if (boxes.size() != 2 || size.w <= 0 || size.h <= 0)
            return fail(error, QStringLiteral("无法计算当前双路比较布局。"));
        const auto boxValue = [](const ftk::Box2I& box) {
            return QJsonArray{box.x(), box.y(), box.w(), box.h()};
        };
        job->insert(QStringLiteral("layout"), QJsonObject{{QStringLiteral("width"), size.w},
            {QStringLiteral("height"), size.h}, {QStringLiteral("a"), boxValue(boxes[0])},
            {QStringLiteral("b"), boxValue(boxes[1])}});
        QJsonObject options = job->value(QStringLiteral("compare")).toObject();
        options.insert(QStringLiteral("mode"), mode);
        job->insert(QStringLiteral("compare"), options);
        return true;
    } catch (const std::exception& exception) {
        return fail(error, QString::fromUtf8(exception.what()));
    }
}

QJsonObject captureCompareExportInputs(const IPlaybackService& playback, const CompareToolbar& toolbar,
    const OcioManager& ocio, const TlViewport& viewport, QString* error)
{
    if (error) error->clear();
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()) {
        fail(error, QStringLiteral("对比导出快照必须在界面线程读取。"));
        return {};
    }
#if !CGPLAY_HAS_TLRENDER
    fail(error, QStringLiteral("当前构建没有 tlRender 对比导出能力。"));
    return {};
#else
    try {
        const auto player = playback.player();
        if (!player || player->getCompare().size() != 1 || !player->getCompare().front())
            throw std::runtime_error("请先加载 A 和一个实际可用的 B 镜头。");
        const auto timelineB = player->getCompare().front();
        const QJsonObject displayValue = viewport.exportDisplaySnapshot();
        if (!displayValue.value(QStringLiteral("valid")).toBool()) {
            fail(error, displayValue.value(QStringLiteral("error")).toString());
            return {};
        }
        const auto native = nlohmann::json::parse(QJsonDocument(displayValue).toJson(QJsonDocument::Compact).toStdString());
        const auto color = ocio.exportColorSnapshot();
        const auto actualOcio = native.at("ocio").get<tl::OCIOOptions>();
        if (actualOcio.enabled != color.enabled || (actualOcio.enabled &&
            (actualOcio.input != color.input.toStdString() || actualOcio.display != color.display.toStdString() ||
             actualOcio.view != color.view.toStdString() || actualOcio.look != color.look.toStdString())))
            throw std::runtime_error("播放器颜色状态正在同步，请稍后重新打开导出。");
        if (color.enabled && color.outputEncoding() == QStringLiteral("unsupported"))
            throw std::runtime_error("当前显示目标不是受支持的 sRGB/Rec.709 SDR，不能按普通视频导出。");
        if (native.at("lut").get<tl::LUTOptions>().enabled)
            throw std::runtime_error("当前外部 LUT 尚不能用于对比导出，请先关闭 LUT。");
        const auto background = native.at("background").get<tl::BackgroundOptions>();
        if (background.type != tl::Background::Solid || background.solidColor.r != 0.F ||
            background.solidColor.g != 0.F || background.solidColor.b != 0.F)
            throw std::runtime_error("对比视频当前只支持黑色背景，请先关闭棋盘格/渐变或自定义背景。");
        const auto foreground = native.at("foreground").get<tl::ForegroundOptions>();
#if TLRENDER_VERSION_MAJOR > 0 || TLRENDER_VERSION_MINOR >= 27
        if (foreground.grid.enabled || foreground.centerMarker.enabled ||
            foreground.missingIndicator.enabled || foreground.clippingWarning.enabled)
#else
        if (foreground.grid.enabled || foreground.outline.enabled)
#endif
            throw std::runtime_error("当前网格或画面轮廓尚不能用于对比导出，请先关闭。");
        const auto displayOptions = native.at("displayOptions").get<std::vector<tl::DisplayOptions>>();
        const auto imageOptions = native.at("imageOptions").get<std::vector<ftk::ImageOptions>>();
        const auto displayAt = [&displayOptions](size_t index) {
            return index < displayOptions.size() ? displayOptions[index] : tl::DisplayOptions();
        };
        const auto imageAt = [&imageOptions](size_t index) {
            return index < imageOptions.size() ? imageOptions[index] : ftk::ImageOptions();
        };
        validateDisplay(displayAt(0), color);
        validateDisplay(displayAt(1), color);
        const auto& bLayers = player->getCompareVideoLayers();
        const int bLayer = bLayers.empty() ? player->getVideoLayer() : bLayers[0];
        QJsonObject a = sourceValue(player->getPath(), player->getAudioPath(), player->getTimeRange(), player->getIOInfo(),
                                          player->getVideoLayer(), imageAt(0), displayAt(0), color);
        // Automatic decode caches are the actual reader source, but the user's
        // original media must remain protected from an output alias as well.
        a.insert(QStringLiteral("originalPath"), playback.currentPath());
        const QJsonObject b = sourceValue(timelineB->getPath(), timelineB->getAudioPath(), timelineB->getTimeRange(), timelineB->getIOInfo(),
                                          bLayer, imageAt(1), displayAt(1), color);
        const tl::CompareOptions options = toolbar.compareOptions();
        const auto timeMode = player->getCompareTime();
        if (timeMode != tl::CompareTime::Relative && timeMode != tl::CompareTime::Absolute)
            throw std::runtime_error("当前比较时间模式无效。");
        const double count = a.value(QStringLiteral("clock")).toObject().value(QStringLiteral("frameCount")).toDouble();
        const int in = playback.hasInPoint() ? playback.inPoint() : 0;
        const double out = playback.hasOutPoint() ? playback.outPoint() : count - 1;
        QJsonObject result{{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("sources"), QJsonObject{{QStringLiteral("a"), a}, {QStringLiteral("b"), b}}},
            {QStringLiteral("compare"), QJsonObject{{QStringLiteral("mode"), modeName(options.compare)},
                {QStringLiteral("wipeCenter"), QJsonArray{options.wipeCenter.x, options.wipeCenter.y}},
                {QStringLiteral("wipeRotation"), options.wipeRotation}, {QStringLiteral("overlay"), options.overlay},
                {QStringLiteral("time"), timeMode == tl::CompareTime::Relative ? QStringLiteral("relative") : QStringLiteral("absolute")}}},
            {QStringLiteral("range"), QJsonObject{{QStringLiteral("start"), 0}, {QStringLiteral("end"), count - 1}}},
            {QStringLiteral("selection"), QJsonObject{{QStringLiteral("hasInOut"), playback.hasInOutRange()},
                {QStringLiteral("in"), in}, {QStringLiteral("out"), out}}},
            {QStringLiteral("aAudioOffsetSeconds"), number(player->getAudioOffset())},
            {QStringLiteral("layoutAspectOverride"), layoutAspectValue(displayAt(0))},
            {QStringLiteral("displaySnapshot"), displayValue}};
        if (!updateCompareExportLayout(&result, modeName(options.compare), error)) return {};
        return result;
    } catch (const std::exception& exception) {
        fail(error, QString::fromUtf8(exception.what()));
        return {};
    }
#endif
}

} // namespace cgplay
