// CGPlay TlViewport.cpp
// Bridges tl::ui::Viewport into a Qt QOpenGLWidget,
// replicating tl::qtwidget::ContainerWidget logic.

#include "TlViewport.h"
#include "core/GPUUploadQueue.h"

#include <tlRender/UI/Viewport.h>
#include <tlRender/GL/Render.h>

#include <ftk/GL/Init.h>
#include <ftk/GL/Mesh.h>
#include <ftk/GL/OffscreenBuffer.h>
#include <ftk/GL/Shader.h>

#include <ftk/Core/Context.h>
#include <ftk/Core/Observable.h>
#include <ftk/UI/IconSystem.h>

#include <QElapsedTimer>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cgplay {

namespace {

constexpr int kInteractiveTimerIntervalMs = 8;
constexpr int kIdleTimerIntervalMs = 33;
constexpr int kInteractionBoostMs = 250;
constexpr int kDeferredViewSyncDelayMs = 20;
constexpr double kZoomSignalEpsilon = 0.0001;

bool isActivePlayback(tl::Playback playback)
{
    return playback == tl::Playback::Forward || playback == tl::Playback::Reverse;
}

bool compareOptionsEqual(const tl::CompareOptions& a, const tl::CompareOptions& b)
{
    return a.compare == b.compare &&
           std::fabs(a.wipeCenter.x - b.wipeCenter.x) < 1e-6f &&
           std::fabs(a.wipeCenter.y - b.wipeCenter.y) < 1e-6f &&
           std::fabs(a.wipeRotation - b.wipeRotation) < 1e-6f &&
           std::fabs(a.overlay - b.overlay) < 1e-6f;
}

// ─── ContainerWindow ──────────────────────────────────────────────────────
// Minimal ftk::IWindow that acts as a parent for the widget tree.
// Identical to tl::qtwidget::ContainerWindow in ContainerWidget.cpp.
class ContainerWindow : public ftk::IWindow
{
    FTK_NON_COPYABLE(ContainerWindow);

public:
    void _init(const std::shared_ptr<ftk::Context>& ctx) {
        IWindow::_init(ctx, nullptr, "cgplay::ContainerWindow");
    }
    ContainerWindow() {}
    ~ContainerWindow() override {}

    static std::shared_ptr<ContainerWindow> create(
        const std::shared_ptr<ftk::Context>& ctx) {
        auto w = std::shared_ptr<ContainerWindow>(new ContainerWindow);
        w->_init(ctx);
        return w;
    }

    uint32_t getID() const override { return 0; }
    int getScreen() const override { return 0; }

    void setGeometry(const ftk::Box2I& value) override {
        IWindow::setGeometry(value);
        for (const auto& child : getChildren())
            child->setGeometry(value);
    }

protected:
    void _update(
        const std::shared_ptr<ftk::FontSystem>&,
        const std::shared_ptr<ftk::IconSystem>&,
        const std::shared_ptr<ftk::Style>&) override {}
};

} // namespace

// ─── Private ───────────────────────────────────────────────────────────────────
struct TlViewport::Private
{
    std::weak_ptr<ftk::Context> context;
    std::shared_ptr<ftk::Style> style;
    std::shared_ptr<ftk::IconSystem> iconSystem;
    std::shared_ptr<ftk::FontSystem> fontSystem;
    std::shared_ptr<ftk::IRender> render;
    std::shared_ptr<tl::ui::Viewport> viewport;
    std::shared_ptr<ContainerWindow> window;
    std::shared_ptr<ftk::gl::Shader> shader;
    std::shared_ptr<ftk::gl::OffscreenBuffer> buffer;
    std::shared_ptr<ftk::gl::VBO> vbo;
    std::shared_ptr<ftk::gl::VAO> vao;
    std::unique_ptr<QTimer> timer;
    std::shared_ptr<tl::Player> player;
    std::shared_ptr<ftk::Observer<tl::Playback> > playbackObserver;
    tl::Playback playback = tl::Playback::Stop;
    QElapsedTimer interactionTimer;
    int timerIntervalMs = kIdleTimerIntervalMs;

    // v1.1 GPU upload queue
    std::unique_ptr<GPUUploadQueue> uploadQueue;

    QElapsedTimer renderTimer;
    qint64 renderFrameCount = 0;
    qint64 timerTickCount = 0;
    qint64 timerPendingSizeSyncCount = 0;
    qint64 timerHasDrawUpdateCount = 0;
    qint64 paintDrawUpdateCount = 0;
    qint64 paintNoDrawUpdateCount = 0;
    qint64 forceRenderCount = 0;
    qint64 markInteractionCount = 0;
    qint64 timerInteractiveDecisionCount = 0;
    qint64 timerPlaybackDecisionCount = 0;
    qint64 timerIdleDecisionCount = 0;
    qint64 timerIntervalChangeCount = 0;
    qint64 updateRequestsTotal = 0;
    qint64 updateRequestsFromTimer = 0;
    qint64 updateRequestsFromForceRender = 0;
    qint64 updateRequestsFromResize = 0;
    qint64 updateRequestsFromFrameView = 0;
    qint64 updateRequestsFromCompareOptions = 0;
    qint64 updateRequestsFromClearPlayer = 0;
    bool pendingDrawUpdateKnown = false;
    bool pendingDrawUpdate = false;
    bool pendingSizeSync = true;
    bool hasLastCompareOptions = false;
    tl::CompareOptions lastCompareOptions;
    QSize lastWidgetSize;
    qreal lastDevicePixelRatio = 0.0;
    ftk::V2I lastEmittedViewPos;
    double lastEmittedZoom = 0.0;
    bool hasLastEmittedViewTransform = false;
    bool updateQueued = false;
    bool forceRenderQueued = false;
};

// ─── Constructor / Destructor ──────────────────────────────────────────────────
TlViewport::TlViewport(
    const std::shared_ptr<ftk::Context>& context,
    const std::shared_ptr<ftk::Style>& style,
    QWidget* parent)
    : QOpenGLWidget(parent)
    , _p(std::make_unique<Private>())
{
    _p->context = context;
    _p->style = style;
    _p->iconSystem = context->getSystem<ftk::IconSystem>();
    _p->fontSystem = context->getSystem<ftk::FontSystem>();

    // Create a fake window to hold the widget tree
    _p->window = ContainerWindow::create(context);

    // Create the ui::Viewport (same class DJV uses)
    _p->viewport = tl::ui::Viewport::create(context);
    _p->viewport->setParent(_p->window);
    _p->lastWidgetSize = size();
    _p->lastDevicePixelRatio = devicePixelRatioF();

    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Periodic timer to check for draw/size updates
    _p->timer = std::make_unique<QTimer>(this);
    _p->timer->setTimerType(Qt::PreciseTimer);
    connect(_p->timer.get(), &QTimer::timeout, this, [this] {
        if (!_p) return;
        ++_p->timerTickCount;
        const QSize widgetSize = size();
        const qreal dpr = devicePixelRatioF();
        if (_p->lastWidgetSize != widgetSize ||
            !qFuzzyCompare(_p->lastDevicePixelRatio, dpr)) {
            _p->lastWidgetSize = widgetSize;
            _p->lastDevicePixelRatio = dpr;
            _p->pendingSizeSync = true;
        }
        _tickEvent();
        const bool pendingSizeSync = _p->pendingSizeSync;
        if (pendingSizeSync) {
            ++_p->timerPendingSizeSyncCount;
            _sizeHintEvent();
            _setGeometry();
            updateGeometry();
            _p->pendingSizeSync = false;
        }
        const bool hasDrawUpdate = _hasDrawUpdate(_p->window);
        if (hasDrawUpdate) {
            ++_p->timerHasDrawUpdateCount;
            _p->pendingDrawUpdate = true;
            _p->pendingDrawUpdateKnown = true;
        } else {
            _p->pendingDrawUpdate = false;
            _p->pendingDrawUpdateKnown = false;
        }
        if (hasDrawUpdate) {
            _requestUpdate("timer");
        }
        _refreshTimerInterval(pendingSizeSync);
    });
    _p->timer->start(_p->timerIntervalMs);
    _p->renderTimer.start();
}

TlViewport::~TlViewport()
{
    makeCurrent();
}

// ─── View API ──────────────────────────────────────────────────────────────────
const ftk::V2I& TlViewport::viewPos() const {
    return _p->viewport->getViewPos();
}
double TlViewport::zoom() const {
    return _p->viewport->getZoom();
}
bool TlViewport::hasFrameView() const {
    return _p->viewport->hasFrameView();
}

// ─── v1.1 GPUUploadQueue ───────────────────────────────────────────────────────
GPUUploadQueue* TlViewport::uploadQueue() const
{
    return _p->uploadQueue.get();
}

void TlViewport::resetRenderStats()
{
    _p->renderFrameCount = 0;
    _p->renderTimer.restart();
    _p->timerTickCount = 0;
    _p->timerPendingSizeSyncCount = 0;
    _p->timerHasDrawUpdateCount = 0;
    _p->paintDrawUpdateCount = 0;
    _p->paintNoDrawUpdateCount = 0;
    _p->forceRenderCount = 0;
    _p->markInteractionCount = 0;
    _p->timerInteractiveDecisionCount = 0;
    _p->timerPlaybackDecisionCount = 0;
    _p->timerIdleDecisionCount = 0;
    _p->timerIntervalChangeCount = 0;
    _p->updateRequestsTotal = 0;
    _p->updateRequestsFromTimer = 0;
    _p->updateRequestsFromForceRender = 0;
    _p->updateRequestsFromResize = 0;
    _p->updateRequestsFromFrameView = 0;
    _p->updateRequestsFromCompareOptions = 0;
    _p->updateRequestsFromClearPlayer = 0;
}

qint64 TlViewport::renderFrameCount() const
{
    return _p->renderFrameCount;
}

double TlViewport::renderStatsSeconds() const
{
    return _p->renderTimer.isValid()
        ? static_cast<double>(_p->renderTimer.elapsed()) / 1000.0
        : 0.0;
}

double TlViewport::renderFps() const
{
    const double seconds = renderStatsSeconds();
    return seconds > 0.0 ? static_cast<double>(_p->renderFrameCount) / seconds : 0.0;
}

QJsonObject TlViewport::benchmarkDiagnostics() const
{
    QJsonObject out;
    out["timer_tick_count"] = static_cast<double>(_p->timerTickCount);
    out["timer_pending_size_sync_count"] = static_cast<double>(_p->timerPendingSizeSyncCount);
    out["timer_has_draw_update_count"] = static_cast<double>(_p->timerHasDrawUpdateCount);
    out["paint_draw_update_count"] = static_cast<double>(_p->paintDrawUpdateCount);
    out["paint_no_draw_update_count"] = static_cast<double>(_p->paintNoDrawUpdateCount);
    out["force_render_count"] = static_cast<double>(_p->forceRenderCount);
    out["mark_interaction_count"] = static_cast<double>(_p->markInteractionCount);
    out["timer_interactive_decision_count"] = static_cast<double>(_p->timerInteractiveDecisionCount);
    out["timer_playback_decision_count"] = static_cast<double>(_p->timerPlaybackDecisionCount);
    out["timer_idle_decision_count"] = static_cast<double>(_p->timerIdleDecisionCount);
    out["timer_interval_change_count"] = static_cast<double>(_p->timerIntervalChangeCount);
    out["current_timer_interval_ms"] = _p->timerIntervalMs;
    out["update_requests_total"] = static_cast<double>(_p->updateRequestsTotal);
    out["update_requests_from_timer"] = static_cast<double>(_p->updateRequestsFromTimer);
    out["update_requests_from_force_render"] = static_cast<double>(_p->updateRequestsFromForceRender);
    out["update_requests_from_resize"] = static_cast<double>(_p->updateRequestsFromResize);
    out["update_requests_from_frame_view"] = static_cast<double>(_p->updateRequestsFromFrameView);
    out["update_requests_from_compare_options"] = static_cast<double>(_p->updateRequestsFromCompareOptions);
    out["update_requests_from_clear_player"] = static_cast<double>(_p->updateRequestsFromClearPlayer);
    return out;
}

// ─── Slots ─────────────────────────────────────────────────────────────────────
void TlViewport::setPlayer(const std::shared_ptr<tl::Player>& player) {
    _p->playbackObserver.reset();
    _p->player = player;
    _p->playback = player ? player->getPlayback() : tl::Playback::Stop;
    _p->hasLastEmittedViewTransform = false;
    _p->viewport->setPlayer(player);
    if (_p->player) {
        _p->playbackObserver = ftk::Observer<tl::Playback>::create(
            _p->player->observePlayback(),
            [this](tl::Playback playback) {
                if (!_p) {
                    return;
                }
                _p->playback = playback;
                _refreshTimerInterval(true);
            });
    }
    _markInteractionActive();
}
void TlViewport::clearPlayer() {
    _p->playbackObserver.reset();
    _p->player.reset();
    _p->playback = tl::Playback::Stop;
    _p->hasLastEmittedViewTransform = false;
    _p->viewport->setPlayer({});
    _forceRender();
    _requestUpdate("clear_player");
}

void TlViewport::setOCIOOptions(const tl::OCIOOptions& opts) {
    _p->viewport->setOCIOOptions(opts);
}
void TlViewport::setLUTOptions(const tl::LUTOptions& opts) {
    _p->viewport->setLUTOptions(opts);
}
void TlViewport::setImageOptions(const std::vector<ftk::ImageOptions>& opts) {
    _p->viewport->setImageOptions(opts);
}
void TlViewport::setDisplayOptions(const std::vector<tl::DisplayOptions>& opts) {
    _p->viewport->setDisplayOptions(opts);
}
void TlViewport::setCompareOptions(const tl::CompareOptions& opts) {
    if (_p->hasLastCompareOptions && compareOptionsEqual(_p->lastCompareOptions, opts)) {
        return;
    }
    _p->lastCompareOptions = opts;
    _p->hasLastCompareOptions = true;
    _p->viewport->setCompareOptions(opts);
    // Compare mode / mix slider changes need an immediate redraw on the Qt host
    // side, otherwise overlay mode can appear stuck until another viewport change.
    _forceRender();
    _requestUpdate("compare_options");
}
void TlViewport::setBackgroundOptions(const tl::BackgroundOptions& opts) {
    _p->viewport->setBackgroundOptions(opts);
}
void TlViewport::setForegroundOptions(const tl::ForegroundOptions& opts) {
    _p->viewport->setForegroundOptions(opts);
}
void TlViewport::setViewPosAndZoom(const ftk::V2I& pos, double z) {
    _p->viewport->setViewPosAndZoom(pos, z);
    _forceRender();
    _emitViewPosAndZoomChangedIfNeeded(pos, z);
}
void TlViewport::setZoom(double z, const ftk::V2I& focus) {
    _p->viewport->setZoom(z, focus);
    _forceRender();
    _emitViewPosAndZoomChangedIfNeeded(_p->viewport->getViewPos(), _p->viewport->getZoom());
}
void TlViewport::setFrameView(bool v) {
    // 先同步最新 geometry，避免 frame view 计算基于旧尺寸
    _setGeometry();
    _p->viewport->setFrameView(v);
    _forceRender();
    Q_EMIT frameViewChanged(v);
    _requestUpdate("frame_view");
    // 关键：setFrameView 会改变内部 zoom/viewPos，必须通知外部同步
    // 使用 QTimer 延迟一帧发射，确保 ftk 内部已完成重算
    QTimer::singleShot(kDeferredViewSyncDelayMs, this, [this]() {
        if (_p->viewport) {
            _emitViewPosAndZoomChangedIfNeeded(_p->viewport->getViewPos(), _p->viewport->getZoom());
        }
    });
}
void TlViewport::resetZoom() {
    _p->viewport->resetZoom();
    _forceRender();
    _emitViewPosAndZoomChangedIfNeeded(_p->viewport->getViewPos(), _p->viewport->getZoom());
}
void TlViewport::zoomIn() {
    _p->viewport->zoomIn();
    _forceRender();
    _emitViewPosAndZoomChangedIfNeeded(_p->viewport->getViewPos(), _p->viewport->getZoom());
}
void TlViewport::zoomOut() {
    _p->viewport->zoomOut();
    _forceRender();
    _emitViewPosAndZoomChangedIfNeeded(_p->viewport->getViewPos(), _p->viewport->getZoom());
}

// ─── Coordinate Mapping ──────────────────────────────────────────────────────
// viewPos 语义：图像左上角在 widget 逻辑坐标系中的位置。
// zoom 语义：1 个图像像素对应 zoom 个 widget 逻辑像素。
// 因此坐标转换直接在逻辑像素空间完成，无需 dpr。
QPointF TlViewport::imageToWidget(double ix, double iy, int /*imgW*/, int /*imgH*/) const
{
    ftk::V2I vp = viewPos();
    double z = zoom();
    return QPointF(vp.x + ix * z, vp.y + iy * z);
}

QPointF TlViewport::widgetToImage(double wx, double wy, int /*imgW*/, int /*imgH*/) const
{
    ftk::V2I vp = viewPos();
    double z = zoom();
    return QPointF((wx - vp.x) / z, (wy - vp.y) / z);
}

// ─── OpenGL ────────────────────────────────────────────────────────────────────
void TlViewport::initializeGL()
{
    initializeOpenGLFunctions();
    ftk::gl::initGLAD();
    glEnable(GL_DITHER);

    try {
        auto ctx = _p->context.lock();
        if (!ctx) {
            qCritical() << "[TlViewport] Context expired, cannot create renderer";
            return;
        }
        _p->render = tl::gl::Render::create(
            ctx->getLogSystem(),
            ctx->getSystem<ftk::FontSystem>());

        // Shader for blitting the offscreen buffer to screen
        const std::string vs =
            "#version 410\n"
            "\n"
            "in vec3 vPos;\n"
            "in vec2 vTexture;\n"
            "out vec2 fTexture;\n"
            "\n"
            "uniform struct Transform {\n"
            "    mat4 mvp;\n"
            "} transform;\n"
            "\n"
            "void main() {\n"
            "    gl_Position = transform.mvp * vec4(vPos, 1.0);\n"
            "    fTexture = vTexture;\n"
            "}\n";
        const std::string fs =
            "#version 410\n"
            "\n"
            "in vec2 fTexture;\n"
            "out vec4 fColor;\n"
            "\n"
            "uniform sampler2D textureSampler;\n"
            "\n"
            "void main() {\n"
            "    fColor = texture(textureSampler, fTexture);\n"
            "}\n";
        _p->shader = ftk::gl::Shader::create(vs, fs);

        // v1.1: 初始化 GPU 上传队列
        _p->uploadQueue = std::make_unique<GPUUploadQueue>();
        _p->uploadQueue->initialize(this);
    }
    catch (const std::exception& e) {
        if (auto ctx = _p->context.lock())
            ctx->log("cgplay::TlViewport", e.what(), ftk::LogType::Error);
    }

    _sizeHintEvent();
}

void TlViewport::resizeGL(int w, int h)
{
    Q_UNUSED(w);
    Q_UNUSED(h);
    _p->vao.reset();
    _p->vbo.reset();
    _p->lastWidgetSize = size();
    _p->lastDevicePixelRatio = devicePixelRatioF();
    _p->pendingSizeSync = true;
    _setGeometry();
    _markInteractionActive();
    // 尺寸变化后强制重绘，否则 OpenGL framebuffer 可能不更新
    _requestUpdate("resize");
    Q_EMIT viewportResized();
}

void TlViewport::paintGL()
{
    _p->updateQueued = false;
    ++_p->renderFrameCount;

    // v1.1: 处理 PBO 上传队列
    if (_p->uploadQueue) {
        _p->uploadQueue->processUploads();
    }

    const float dpr = window()->devicePixelRatio();
    const ftk::Size2I renderSize(
        static_cast<int>(width() * dpr),
        static_cast<int>(height() * dpr));

    // 记录渲染前的 zoom/viewPos，检测 _frameView() 是否在 drawEvent 中改变了它们
    const auto zoomBefore = _p->viewport->getZoom();
    const auto posBefore  = _p->viewport->getViewPos();

    const bool hasDrawUpdate = _p->pendingDrawUpdateKnown
        ? _p->pendingDrawUpdate
        : _hasDrawUpdate(_p->window);
    _p->pendingDrawUpdateKnown = false;
    _p->pendingDrawUpdate = false;

    if (hasDrawUpdate) {
        ++_p->paintDrawUpdateCount;
        try {
            // Create / resize offscreen buffer
            if (renderSize.isValid()) {
                if (ftk::gl::doCreate(
                        _p->buffer,
                        renderSize,
                        ftk::gl::TextureType::RGBA_F32)) {
                    _p->buffer = ftk::gl::OffscreenBuffer::create(
                        renderSize,
                        ftk::gl::TextureType::RGBA_F32);
                }
            } else {
                _p->buffer.reset();
            }

            // Render viewport into offscreen buffer
            if (_p->render && _p->buffer) {
                ftk::gl::OffscreenBufferBinding binding(_p->buffer);

                ftk::RenderOptions renderOpts;
                renderOpts.clearColor =
                    _p->style->getColorRole(ftk::ColorRole::Window);

                _p->render->begin(renderSize, renderOpts);

                ftk::DrawEvent drawEvent(
                    _p->fontSystem,
                    _p->iconSystem,
                    dpr,
                    _p->style,
                    _p->render);

                _p->render->setClipRectEnabled(true);
                _drawEvent(
                    _p->window,
                    ftk::Box2I(ftk::V2I(), renderSize),
                    drawEvent);
                _p->render->setClipRectEnabled(false);
                _p->render->end();
            }
        }
        catch (const std::exception& e) {
            if (auto ctx = _p->context.lock()) {
                ctx->log(
                    "cgplay::TlViewport",
                    e.what(),
                    ftk::LogType::Error);
            }
        }
    } else {
        ++_p->paintNoDrawUpdateCount;
    }

    // 检测 _frameView() 在 drawEvent 中是否改变了 zoom/viewPos
    // （当 frameView==true 时，drawEvent 会自动调用 _frameView 重新计算缩放）
    const auto zoomAfter = _p->viewport->getZoom();
    const auto posAfter  = _p->viewport->getViewPos();
    if (zoomAfter != zoomBefore || posBefore != posAfter) {
        _emitViewPosAndZoomChangedIfNeeded(posAfter, zoomAfter);
    }

    // Blit offscreen buffer to the Qt widget's default framebuffer
    glViewport(0, 0, renderSize.w, renderSize.h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (_p->buffer && _p->shader) {
        _p->shader->bind();

        const auto pm = ftk::ortho(
            0.f,
            static_cast<float>(renderSize.w),
            static_cast<float>(renderSize.h),
            0.f,
            -1.f,
            1.f);
        _p->shader->setUniform("transform.mvp", pm);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, _p->buffer->getColorID());

        // Build quad mesh covering the full widget
        const auto mesh = ftk::mesh(
            ftk::Box2I(0, 0, renderSize.w, renderSize.h), true);

        if (!_p->vbo) {
            _p->vbo = ftk::gl::VBO::create(
                mesh.triangles.size() * 3,
                ftk::gl::VBOType::Pos2_F32_UV_U16);
        }
        if (_p->vbo) {
            _p->vbo->copy(
                convert(mesh, ftk::gl::VBOType::Pos2_F32_UV_U16));
        }
        if (!_p->vao && _p->vbo) {
            _p->vao = ftk::gl::VAO::create(
                ftk::gl::VBOType::Pos2_F32_UV_U16,
                _p->vbo->getID());
        }
        if (_p->vao && _p->vbo) {
            _p->vao->bind();
            _p->vao->draw(GL_TRIANGLES, 0, _p->vbo->getSize());
        }
    }
}

// ─── Tick / Size / Draw internals ──────────────────────────────────────────────
void TlViewport::_tickEvent()
{
    ftk::TickEvent tickEvent;
    // Walk the widget tree: window → viewport
    for (const auto& child : _p->window->getChildren())
        child->tickEvent(true, true, tickEvent);
    _p->window->tickEvent(true, true, tickEvent);
}

bool TlViewport::_hasSizeUpdate(const std::shared_ptr<ftk::IWidget>& widget) const
{
    bool out = widget->hasSizeUpdate();
    if (!out) {
        for (const auto& child : widget->getChildren()) {
            out |= _hasSizeUpdate(child);
        }
    }
    return out;
}

void TlViewport::_sizeHintEvent()
{
    const float dpr = window()->devicePixelRatio();
    ftk::SizeHintEvent ev(_p->fontSystem, _p->iconSystem, dpr, _p->style);
    for (const auto& child : _p->window->getChildren())
        child->sizeHintEvent(ev);
    _p->window->sizeHintEvent(ev);
}

void TlViewport::_setGeometry()
{
    const float dpr = window()->devicePixelRatio();
    const ftk::Box2I geom(
        0, 0,
        static_cast<int>(width() * dpr),
        static_cast<int>(height() * dpr));
    _p->window->setGeometry(geom);
}

void TlViewport::_emitViewPosAndZoomChangedIfNeeded(const ftk::V2I& pos, double zoom)
{
    const bool samePos =
        _p->hasLastEmittedViewTransform &&
        _p->lastEmittedViewPos == pos;
    const bool sameZoom =
        _p->hasLastEmittedViewTransform &&
        std::abs(_p->lastEmittedZoom - zoom) <= kZoomSignalEpsilon;
    if (samePos && sameZoom) {
        return;
    }
    _p->lastEmittedViewPos = pos;
    _p->lastEmittedZoom = zoom;
    _p->hasLastEmittedViewTransform = true;
    Q_EMIT viewPosAndZoomChanged(pos, zoom);
}

void TlViewport::_markInteractionActive()
{
    ++_p->markInteractionCount;
    _p->interactionTimer.start();
    _refreshTimerInterval(true);
}

int TlViewport::_desiredPlaybackTimerIntervalMs() const
{
    double playbackFps = 0.0;
    if (_p->player) {
        const double speed = std::abs(_p->player->getSpeed());
        const double mediaRate = _p->player->getTimeRange().duration().rate();
        if (mediaRate > 0.0 && speed > 0.0 && speed <= 8.0) {
            playbackFps = mediaRate * speed;
        } else if (speed > 0.0) {
            playbackFps = speed;
        } else {
            playbackFps = mediaRate;
        }
    }

    if (playbackFps <= 0.0) {
        return 24;
    }

    const double clampedPlaybackFps = std::clamp(playbackFps, 1.0, 60.0);
    const int intervalMs = static_cast<int>(std::floor(1000.0 / clampedPlaybackFps));
    return std::clamp(intervalMs, 16, 50);
}

void TlViewport::_refreshTimerInterval(bool pendingSizeUpdate)
{
    const bool recentInteraction =
        _p->interactionTimer.isValid() &&
        _p->interactionTimer.elapsed() < kInteractionBoostMs;
    const bool activePlayback = isActivePlayback(_p->playback);
    int desiredInterval = kIdleTimerIntervalMs;
    if (pendingSizeUpdate || recentInteraction) {
        ++_p->timerInteractiveDecisionCount;
        desiredInterval = kInteractiveTimerIntervalMs;
    } else if (activePlayback) {
        ++_p->timerPlaybackDecisionCount;
        desiredInterval = _desiredPlaybackTimerIntervalMs();
    } else {
        ++_p->timerIdleDecisionCount;
    }
    if (_p->timer && _p->timerIntervalMs != desiredInterval) {
        _p->timerIntervalMs = desiredInterval;
        ++_p->timerIntervalChangeCount;
        _p->timer->setTimerType(
            (pendingSizeUpdate || recentInteraction || activePlayback)
                ? Qt::PreciseTimer
                : Qt::CoarseTimer);
        _p->timer->start(desiredInterval);
    }
}

void TlViewport::_forceRender()
{
    _markInteractionActive();
    if (_p->forceRenderQueued) {
        return;
    }
    _p->forceRenderQueued = true;

    QTimer::singleShot(0, this, [this]() {
        if (!_p || !_p->window) {
            return;
        }
        _p->forceRenderQueued = false;

        // tl::ui::Viewport marks doRender only after a geometry change. Keep
        // the existing trigger, but execute it at most once per event loop.
        const auto geom = _p->window->getGeometry();
        _p->window->setGeometry(ftk::Box2I(0, 0, geom.w(), geom.h() + 1));
        _p->window->setGeometry(geom);
        ++_p->forceRenderCount;
        _requestUpdate("force_render");
    });
}

void TlViewport::_requestUpdate(const char* source)
{
    ++_p->updateRequestsTotal;
    if (0 == std::strcmp(source, "timer")) {
        ++_p->updateRequestsFromTimer;
    } else if (0 == std::strcmp(source, "force_render")) {
        ++_p->updateRequestsFromForceRender;
    } else if (0 == std::strcmp(source, "resize")) {
        ++_p->updateRequestsFromResize;
    } else if (0 == std::strcmp(source, "frame_view")) {
        ++_p->updateRequestsFromFrameView;
    } else if (0 == std::strcmp(source, "compare_options")) {
        ++_p->updateRequestsFromCompareOptions;
    } else if (0 == std::strcmp(source, "clear_player")) {
        ++_p->updateRequestsFromClearPlayer;
    }
    if (0 != std::strcmp(source, "timer")) {
        _p->pendingDrawUpdateKnown = false;
        _p->pendingDrawUpdate = false;
    }
    if (_p->updateQueued) {
        return;
    }
    _p->updateQueued = true;
    QTimer::singleShot(0, this, [this] {
        _p->updateQueued = false;
        update();
    });
}

bool TlViewport::_hasDrawUpdate(const std::shared_ptr<ftk::IWidget>& widget) const
{
    if (widget->isClipped())
        return false;
    if (widget->hasDrawUpdate())
        return true;
    for (const auto& child : widget->getChildren()) {
        if (_hasDrawUpdate(child))
            return true;
    }
    return false;
}

void TlViewport::_drawEvent(
    const std::shared_ptr<ftk::IWidget>& widget,
    const ftk::Box2I& drawRect,
    const ftk::DrawEvent& event)
{
    const ftk::Box2I& g = widget->getGeometry();
    if (!widget->isClipped() && g.w() > 0 && g.h() > 0) {
        event.render->setClipRect(drawRect);
        widget->drawEvent(drawRect, event);

        const ftk::Box2I childrenClip =
            ftk::intersect(widget->getChildrenClipRect(), drawRect);
        event.render->setClipRect(childrenClip);
        for (const auto& child : widget->getChildren()) {
            if (ftk::intersects(child->getGeometry(), childrenClip)) {
                _drawEvent(
                    child,
                    ftk::intersect(child->getGeometry(), childrenClip),
                    event);
            }
        }
        event.render->setClipRect(drawRect);
        widget->drawOverlayEvent(drawRect, event);
    }
}

} // namespace cgplay
