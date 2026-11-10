// CGPlay OcioManager.cpp

#include "OcioManager.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QGroupBox>
#include <QSettings>
#include <QDebug>
#include <cmath>
#include <algorithm>

#ifdef CGPLAY_OCIO_ENABLED
  #include <OpenColorIO/OpenColorIO.h>
  namespace OCIO = OCIO_NAMESPACE;
#endif

namespace cgplay {

// ─── Private ─────────────────────────────────────────────────────────────────
struct OcioManager::Private
{
#ifdef CGPLAY_OCIO_ENABLED
    OCIO::ConstConfigRcPtr config;
#endif
    QString configPath;

    // Current selections
    // EXR files without an explicit color-space attribute (including Nuke
    // scene-linear renders) must enter ACES from the scene-linear role.
    // Users can still choose ACEScg explicitly in the OCIO settings dialog.
    QString input   = "scene_linear";
    QString display = "sRGB";
    QString view    = "ACES 1.0 SDR-video";
    float   exposure = 0.f;
    float   gamma    = 1.f;
    bool    enabled  = false;     // Config loaded successfully
    bool    userEnabled = false;  // Default off for standard video playback
    bool    exrOverrideActive = false;
    bool    exrOverrideSuppressed = false;
    bool    exrPreviousUserEnabled = false;
};

// ─── Constructor / Destructor ────────────────────────────────────────────────
OcioManager::OcioManager(QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    // Try loading ACES 1.2 builtin config by default
    loadAces12Config();
}

OcioManager::~OcioManager() = default;

// ─── Config loading ───────────────────────────────────────────────────────────
void OcioManager::loadConfig(const QString& configPath)
{
    QString error;
    if (!applyConfiguration({_p->userEnabled, configPath, {}, {}, {}}, &error))
        qWarning() << "[OCIO] Failed to load config:" << error;
}

bool OcioManager::applyConfiguration(const PreviewTransformSettings& settings, QString* error)
{
    if (error) error->clear();
#ifdef CGPLAY_OCIO_ENABLED
    try {
        const QString path = settings.configPath.isEmpty() ? _p->configPath : settings.configPath;
        if (path.isEmpty()) throw OCIO::Exception("OCIO config path is empty.");
        auto config = OCIO::Config::CreateFromFile(path.toStdString().c_str());
        config->validate();
        QString input = settings.input;
        if (input.isEmpty()) {
            const char* role = config->getRoleColorSpace(OCIO::ROLE_SCENE_LINEAR);
            input = role && *role ? QString::fromUtf8(role)
                : QString::fromUtf8(config->getColorSpaceNameByIndex(0));
        }
        const QString display = settings.display.isEmpty()
            ? QString::fromUtf8(config->getDefaultDisplay()) : settings.display;
        const QString view = settings.view.isEmpty()
            ? QString::fromUtf8(config->getDefaultView(display.toStdString().c_str())) : settings.view;
        if (!config->getColorSpace(input.toStdString().c_str()) || display.isEmpty() || view.isEmpty())
            throw OCIO::Exception("OCIO input, display or view is invalid.");
        // getProcessor may resolve LUT files and fail even for a valid config.
        config->getProcessor(input.toStdString().c_str(), display.toStdString().c_str(),
                             view.toStdString().c_str(), OCIO::TRANSFORM_DIR_FORWARD);
        const bool enabledChangedValue = _p->userEnabled != settings.enabled;
        const bool displayChangedValue = _p->display != display;
        _p->config = config;
        _p->configPath = path;
        _p->input = input;
        _p->display = display;
        _p->view = view;
        _p->enabled = true;
        if (_p->exrOverrideActive && enabledChangedValue) {
            _p->exrOverrideActive = false;
            _p->exrOverrideSuppressed = true;
        }
        _p->userEnabled = settings.enabled;
        Q_EMIT configLoaded(path);
        _rebuildOptions();
        if (displayChangedValue) Q_EMIT displayChanged(display);
        if (enabledChangedValue) Q_EMIT enabledChanged(settings.enabled);
        return true;
    } catch (const OCIO::Exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return false;
    }
#else
    if (error) *error = tr("OpenColorIO is not available in this build.");
    return false;
#endif
}

// ─── ACES 1.2 config (priority) ──────────────────────────────────────────────
void OcioManager::loadAces12Config()
{
#ifdef CGPLAY_OCIO_ENABLED
    // OCIO v2.3+ built-in config names — try newest ACES configs first
    const char* acesCandidates[] = {
        "ocio://studio-config-v1.0.0_aces-v1.3_ocio-v2.1",  // ACES 1.3 Studio
        "ocio://default",                                    // ACES 1.2 default (OCIO v2.1+)
        "ocio://cg-config-v2.0.0_aces-v1.3_ocio-v2.1",     // ACES 1.3 CG
        "ocio://reference-config-v2.0.0_aces-v1.2_ocio-v2.1",
    };

    for (const char* candidate : acesCandidates) {
        QString error;
        if (applyConfiguration({_p->userEnabled, QString::fromUtf8(candidate), {}, {}, {}}, &error)) return;
        qDebug() << "[OCIO] Builtin config not available:" << candidate << error;
    }

    // All candidates failed — fall back to linear/sRGB
    qWarning() << "[OCIO] No ACES builtin config found. Falling back to linear/sRGB.";

#else
    _p->enabled = false;
    _p->display = "ACES sRGB";
    _p->view    = "ACES 1.0 SDR-video";
#endif
}

void OcioManager::loadBuiltinConfig(const QString& name)
{
    const QString path = name.startsWith(QStringLiteral("ocio://")) ? name
        : name == QStringLiteral("ACES") ? QStringLiteral("ocio://default")
        : QStringLiteral("ocio://") + name.toLower();
    loadConfig(path);
}

// ─── Accessors ────────────────────────────────────────────────────────────────
tl::OCIOOptions OcioManager::currentOptions() const
{
    tl::OCIOOptions opts;
#if CGPLAY_HAS_TLRENDER
    opts.enabled  = _p->enabled && _p->userEnabled;
    // Use OCIOConfig::File because OCIO v2's CreateFromFile() supports
    // both file paths AND ocio:// URIs (for builtin configs).
    // tlRender's BuiltIn mode hardcodes "ocio://default" and ignores fileName,
    // which breaks switching between different builtin configs.
    opts.config   = tl::OCIOConfig::File;
    opts.fileName = _p->configPath.toStdString();
    opts.input    = _p->input.toStdString();
    opts.display  = _p->display.toStdString();
    opts.view     = _p->view.toStdString();
    // Note: OCIOOptions in this tlRender version does not have
    // exposure/gamma fields. ViewerWidget applies them separately via
    // DisplayOptions, and exportColorSnapshot carries the same values.
#endif
    return opts;
}

ColorTransformSnapshot OcioManager::exportColorSnapshot() const
{
    const auto options = currentOptions();
    ColorTransformSnapshot snapshot;
    snapshot.enabled = options.enabled;
    snapshot.configPath = _p->configPath;
    snapshot.input = _p->input;
    snapshot.display = _p->display;
    snapshot.view = _p->view;
#if CGPLAY_HAS_TLRENDER
    snapshot.look = QString::fromStdString(options.look);
#endif
    snapshot.exposure = _p->exposure;
    snapshot.gamma = _p->gamma;
#ifdef CGPLAY_OCIO_ENABLED
    if (snapshot.enabled && _p->config) {
        snapshot.configCacheId = QString::fromUtf8(_p->config->getCacheID());
    }
#endif
    return snapshot;
}

QString OcioManager::currentConfigPath() const { return _p->configPath; }
OcioManager::PreviewTransformSettings OcioManager::previewTransformSettings() const
{
    PreviewTransformSettings settings;
    settings.enabled = _p->enabled && _p->userEnabled;
    settings.configPath = _p->configPath;
    settings.input = _p->input;
    settings.display = _p->display;
    settings.view = _p->view;
    return settings;
}
QString OcioManager::currentInput()       const { return _p->input;   }
QString OcioManager::currentDisplay()     const { return _p->display; }
QString OcioManager::currentView()        const { return _p->view;    }
float   OcioManager::exposure()           const { return _p->exposure; }
float   OcioManager::gamma()              const { return _p->gamma;    }
bool    OcioManager::isEnabled()          const { return _p->enabled && _p->userEnabled; }

bool OcioManager::applyPreviewTransform(QImage& image, const PreviewTransformSettings& settings)
{
#ifdef CGPLAY_OCIO_ENABLED
    if (!settings.enabled || settings.configPath.isEmpty() || image.isNull()) {
        return false;
    }

    try {
        auto config = OCIO::Config::CreateFromFile(settings.configPath.toStdString().c_str());
        if (!config) {
            return false;
        }

        auto processor = config->getProcessor(
            settings.input.toStdString().c_str(),
            settings.display.toStdString().c_str(),
            settings.view.toStdString().c_str(),
            OCIO::TRANSFORM_DIR_FORWARD);
        if (!processor) {
            return false;
        }

        QImage rgba = image.convertToFormat(QImage::Format_RGBA32FPx4);
        OCIO::PackedImageDesc imgDesc(
            rgba.bits(),
            rgba.width(),
            rgba.height(),
            4,
            OCIO::BIT_DEPTH_F32,
            sizeof(float),
            4 * sizeof(float),
            rgba.bytesPerLine());
        processor->getDefaultCPUProcessor()->apply(imgDesc);
        image = rgba.convertToFormat(QImage::Format_RGB32);
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[OCIO] applyPreviewTransform failed:" << e.what();
        return false;
    }
#else
    Q_UNUSED(image);
    Q_UNUSED(settings);
    return false;
#endif
}

QStringList OcioManager::availableColorSpaces() const
{
    QStringList cs;
#ifdef CGPLAY_OCIO_ENABLED
    if (_p->config) {
        for (int i = 0; i < _p->config->getNumColorSpaces(); ++i)
            cs << QString::fromStdString(_p->config->getColorSpaceNameByIndex(i));
    }
#else
    cs << "scene_linear" << "sRGB" << "Rec.709" << "ACEScg" << "Log";
#endif
    return cs;
}

QStringList OcioManager::availableDisplays() const
{
    QStringList displays;
#ifdef CGPLAY_OCIO_ENABLED
    if (_p->config) {
        for (int i = 0; i < _p->config->getNumDisplays(); ++i)
            displays << QString::fromStdString(_p->config->getDisplay(i));
    }
#else
    displays << "sRGB" << "Rec.709" << "ACES" << "P3-DCI";
#endif
    return displays;
}

QStringList OcioManager::availableViews(const QString& display) const
{
    QStringList views;
#ifdef CGPLAY_OCIO_ENABLED
    if (_p->config) {
        std::string d = display.toStdString();
        for (int i = 0; i < _p->config->getNumViews(d.c_str()); ++i)
            views << QString::fromStdString(_p->config->getView(d.c_str(), i));
    }
#else
    if (display == "sRGB")    views << "sRGB" << "sRGB (linear)";
    else if (display == "ACES") views << "ACES 1.0 SDR-video" << "Log" << "Raw";
    else views << display;
#endif
    return views;
}

// ─── Setters ─────────────────────────────────────────────────────────────────
void OcioManager::setInput(const QString& cs)
{
    _p->input = cs;
    _rebuildOptions();
}

void OcioManager::setDisplay(const QString& disp)
{
    _p->display = disp;
    _rebuildOptions();
    Q_EMIT displayChanged(disp);
}

void OcioManager::setView(const QString& view)
{
    _p->view = view;
    _rebuildOptions();
}

void OcioManager::setExposure(float ev)
{
    if (!std::isfinite(ev)) return;
    _p->exposure = std::clamp(ev, -10.F, 10.F);
    _rebuildOptions();
}

void OcioManager::setGamma(float g)
{
    if (!std::isfinite(g)) return;
    _p->gamma = std::clamp(g, .01F, 4.F);
    _rebuildOptions();
}

void OcioManager::setEnabled(bool enabled)
{
    if (_p->exrOverrideActive && _p->userEnabled != enabled) {
        // A manual toggle while an EXR override is active is an explicit user
        // choice.  Keep the choice until the current EXR item is replaced.
        _p->exrOverrideActive = false;
        _p->exrOverrideSuppressed = true;
    }
    if (_p->userEnabled == enabled) return;
    _p->userEnabled = enabled;
    qDebug() << "[OCIO] User" << (enabled ? "enabled" : "disabled") << "OCIO";
    _rebuildOptions();
    Q_EMIT enabledChanged(enabled);
}

void OcioManager::applyExrSceneLinearDefaults()
{
    if (!_p->enabled) return;
    if (_p->exrOverrideSuppressed) return;
    // An EXR opened through the color-managed path must not inherit a stale
    // disabled session state; otherwise the selected ACES transform is never
    // applied even though the UI still shows the previous view name.
    if (!_p->exrOverrideActive) {
        _p->exrOverrideActive = true;
        _p->exrPreviousUserEnabled = _p->userEnabled;
    }
    const bool wasUserEnabled = _p->userEnabled;
    if (!wasUserEnabled) {
        _p->userEnabled = true;
    }
    // Match Nuke's installed ACES 1.2 config when available. This is the
    // exact config shown in the user's Nuke Color settings; the bundled ACES
    // 1.3 config remains the portable fallback for other machines.
    const QStringList nukeConfigCandidates = {
        QStringLiteral("C:/Program Files/Adobe/Adobe After Effects 2025/Support Files/OpenColorIO-Configs/ACES 1.2/config.ocio"),
        QStringLiteral("C:/Program Files/Adobe/Adobe After Effects 2024/Support Files/OpenColorIO-Configs/ACES 1.2/config.ocio")
    };
    for (const QString& candidate : nukeConfigCandidates) {
        if (QFileInfo::exists(candidate) && _p->configPath != candidate) {
            loadConfig(candidate);
            break;
        }
    }
    _p->input = QStringLiteral("scene_linear");
#ifdef CGPLAY_OCIO_ENABLED
    if (_p->config) {
        const char* sceneLinear = _p->config->getRoleColorSpace(OCIO::ROLE_SCENE_LINEAR);
        if (sceneLinear && *sceneLinear) {
            _p->input = QString::fromUtf8(sceneLinear);
        }
    }
#endif
    const QStringList displays = availableDisplays();
    for (const QString& display : displays) {
        if (display.contains(QStringLiteral("sRGB"), Qt::CaseInsensitive)) {
            _p->display = display;
            break;
        }
    }
    if (_p->display.isEmpty() || !availableDisplays().contains(_p->display)) {
        for (const QString& display : displays) {
            if (display.compare(QStringLiteral("ACES"), Qt::CaseInsensitive) == 0) {
                _p->display = display;
                break;
            }
        }
    }
    const QStringList views = availableViews(_p->display);
    // ACES 1.2/Nuke's sRGB (ACES) view includes the D60-to-D65 simulation.
    // Prefer that equivalent when the bundled ACES 1.3 config exposes it.
    for (const QString& view : views) {
        if (view.compare(QStringLiteral("sRGB"), Qt::CaseInsensitive) == 0) {
            _p->view = view;
            break;
        }
    }
    for (const QString& view : views) {
        if (_p->view.compare(QStringLiteral("sRGB"), Qt::CaseInsensitive) == 0) {
            break;
        }
        if (view.contains(QStringLiteral("D60"), Qt::CaseInsensitive) &&
            view.contains(QStringLiteral("sRGB"), Qt::CaseInsensitive)) {
            _p->view = view;
            break;
        }
    }
    qInfo() << "[OCIO] EXR scene-linear defaults:" << _p->input << _p->display << _p->view;
    // Force tlRender to rebuild its GPU OCIO shader even when the same
    // options were cached before the OpenGL video item was created.
    if (wasUserEnabled) {
        _p->userEnabled = false;
        _rebuildOptions();
        _p->userEnabled = true;
    }
    _rebuildOptions();
    if (!wasUserEnabled) {
        Q_EMIT enabledChanged(true);
    }
}

void OcioManager::clearExrSceneLinearDefaults()
{
    _p->exrOverrideSuppressed = false;
    if (!_p->exrOverrideActive) {
        return;
    }

    _p->exrOverrideActive = false;
    const bool restoreEnabled = _p->exrPreviousUserEnabled;
    _p->exrPreviousUserEnabled = false;
    if (_p->userEnabled == restoreEnabled) {
        return;
    }

    _p->userEnabled = restoreEnabled;
    _rebuildOptions();
    Q_EMIT enabledChanged(restoreEnabled);
}

void OcioManager::_rebuildOptions()
{
    const auto options = currentOptions();
    Q_EMIT optionsChanged(options);
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->publish(OcioOptionsChangedEvent{ options, previewTransformSettings() });
        eventBus->publish(OcioDisplayStateChangedEvent{ isEnabled(), currentDisplay() });
    }
}

// ─── Settings Dialog ──────────────────────────────────────────────────────────
void OcioManager::showSettings(QWidget* parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("OCIO 颜色设置");
    dlg.setMinimumWidth(420);

    auto* vlay = new QVBoxLayout(&dlg);

    // ── Enable/Disable toggle ───────────────────────────────────────────────
    auto* chkEnabled = new QCheckBox("启用 OCIO 色彩管理", &dlg);
    chkEnabled->setChecked(_p->userEnabled);
    chkEnabled->setToolTip("关闭后将使用线性 sRGB 显示");
    vlay->addWidget(chkEnabled);

    // ── 配置路径 ───────────────────────────────────────────────────────────
    auto* grpConfig = new QGroupBox("配置", &dlg);
    auto* glay = new QHBoxLayout(grpConfig);
    auto* lblConfig = new QLineEdit(_p->configPath, &dlg);
    lblConfig->setObjectName(QStringLiteral("ocioConfigPath"));
    lblConfig->setReadOnly(true);
    auto* btnBrowse = new QPushButton("浏览…", &dlg);
    auto* btnBuiltin = new QPushButton("内置", &dlg);
    btnBuiltin->setToolTip("恢复内置 ACES 1.2/1.3 配置");
    glay->addWidget(lblConfig, 1);
    glay->addWidget(btnBuiltin);
    glay->addWidget(btnBrowse);
    vlay->addWidget(grpConfig);

    // ── 颜色空间 / 显示 / 视图 ──────────────────────────────────────────
    auto* grpColor = new QGroupBox("色彩变换", &dlg);
    auto* cform = new QVBoxLayout(grpColor);

    auto addCombo = [&](const QString& label, const QStringList& items,
                        const QString& current) -> QComboBox* {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(label, &dlg), 0);
        auto* cb = new QComboBox(&dlg);
        cb->addItems(items);
        int idx = items.indexOf(current);
        if (idx >= 0) cb->setCurrentIndex(idx);
        row->addWidget(cb, 1);
        cform->addLayout(row);
        return cb;
    };

    auto* cbInput   = addCombo("输入颜色空间:",   availableColorSpaces(), _p->input);
    auto* cbDisplay = addCombo("显示设备:", availableDisplays(),    _p->display);
    auto* cbView    = addCombo("视图变换:",    availableViews(_p->display), _p->view);
    cbInput->setObjectName(QStringLiteral("ocioInput"));
    cbDisplay->setObjectName(QStringLiteral("ocioDisplay"));
    cbView->setObjectName(QStringLiteral("ocioView"));
#ifdef CGPLAY_OCIO_ENABLED
    auto dialogConfig = _p->config;
#endif
    const auto dialogViews = [&](const QString& display) {
        QStringList views;
#ifdef CGPLAY_OCIO_ENABLED
        if (dialogConfig) {
            const auto name = display.toStdString();
            for (int i = 0; i < dialogConfig->getNumViews(name.c_str()); ++i)
                views << QString::fromUtf8(dialogConfig->getView(name.c_str(), i));
        }
#endif
        return views;
    };

    // Update views when display changes
    connect(cbDisplay, &QComboBox::currentTextChanged, &dlg, [&](const QString& d) {
        cbView->clear();
        cbView->addItems(dialogViews(d));
    });

    vlay->addWidget(grpColor);

    // ── Exposure / Gamma ──────────────────────────────────────────────────────
    auto* grpAdj = new QGroupBox("调整", &dlg);
    auto* aform  = new QVBoxLayout(grpAdj);

    auto addSpin = [&](const QString& label, double val, double min, double max,
                        double step) -> QDoubleSpinBox* {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(label, &dlg), 0);
        auto* sp = new QDoubleSpinBox(&dlg);
        sp->setRange(min, max);
        sp->setSingleStep(step);
        sp->setValue(val);
        row->addWidget(sp, 1);
        aform->addLayout(row);
        return sp;
    };

    auto* spExposure = addSpin("曝光 (EV):", _p->exposure, -10.0, 10.0, 0.25);
    auto* spGamma    = addSpin("伽马:",          _p->gamma,    0.01,  4.0,  0.1);

    vlay->addWidget(grpAdj);

    // Enable/disable color controls based on checkbox
    auto updateEnabled = [&](bool enabled) {
        grpConfig->setEnabled(enabled);
        grpColor->setEnabled(enabled);
        grpAdj->setEnabled(enabled);
    };
    updateEnabled(chkEnabled->isChecked());
    connect(chkEnabled, &QCheckBox::toggled, &dlg, updateEnabled);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    auto* configError = new QLabel(&dlg);
    configError->setObjectName(QStringLiteral("ocioConfigError"));
    configError->setWordWrap(true);
    vlay->addWidget(configError);
    vlay->addWidget(btns);

    // Only the dialog owns this candidate. Cancel does not publish options or
    // replace the active config. Refresh names before the user confirms it.
    connect(lblConfig, &QLineEdit::textChanged, &dlg, [&](const QString& path) {
#ifdef CGPLAY_OCIO_ENABLED
        try {
            auto candidate = OCIO::Config::CreateFromFile(path.toStdString().c_str());
            candidate->validate();
            QStringList inputs, displays;
            for (int i = 0; i < candidate->getNumColorSpaces(); ++i)
                inputs << QString::fromUtf8(candidate->getColorSpaceNameByIndex(i));
            for (int i = 0; i < candidate->getNumDisplays(); ++i)
                displays << QString::fromUtf8(candidate->getDisplay(i));
            const QString oldInput = cbInput->currentText(), oldDisplay = cbDisplay->currentText();
            const QString oldView = cbView->currentText();
            dialogConfig = candidate;
            const QSignalBlocker block(cbDisplay);
            cbInput->clear(); cbInput->addItems(inputs);
            const char* role = candidate->getRoleColorSpace(OCIO::ROLE_SCENE_LINEAR);
            cbInput->setCurrentText(inputs.contains(oldInput) ? oldInput : QString::fromUtf8(role));
            cbDisplay->clear(); cbDisplay->addItems(displays);
            cbDisplay->setCurrentText(displays.contains(oldDisplay) ? oldDisplay
                : QString::fromUtf8(candidate->getDefaultDisplay()));
            const auto views = dialogViews(cbDisplay->currentText());
            cbView->clear(); cbView->addItems(views);
            cbView->setCurrentText(views.contains(oldView) ? oldView
                : QString::fromUtf8(candidate->getDefaultView(cbDisplay->currentText().toStdString().c_str())));
            configError->clear();
        } catch (const OCIO::Exception& e) {
            configError->setText(tr("Cannot load OCIO config: %1").arg(QString::fromUtf8(e.what())));
        }
#endif
    });

    // Browse external .ocio file
    connect(btnBrowse, &QPushButton::clicked, &dlg, [&] {
        QString path = QFileDialog::getOpenFileName(
            &dlg, "打开 OCIO 配置", {}, "OCIO 配置 (*.ocio);;所有文件 (*)");
        if (!path.isEmpty()) lblConfig->setText(path);
    });

    // Reset to builtin config
    connect(btnBuiltin, &QPushButton::clicked, &dlg, [&] {
        lblConfig->setText(QStringLiteral("ocio://studio-config-v1.0.0_aces-v1.3_ocio-v2.1"));
    });

    connect(btns, &QDialogButtonBox::accepted, &dlg, [&] {
        PreviewTransformSettings settings{chkEnabled->isChecked(), lblConfig->text(),
            cbInput->currentText(), cbDisplay->currentText(), cbView->currentText()};
        QString error;
        if (!applyConfiguration(settings, &error)) {
            configError->setText(tr("Cannot apply OCIO settings: %1").arg(error));
            return;
        }
        setExposure(static_cast<float>(spExposure->value()));
        setGamma(static_cast<float>(spGamma->value()));
        dlg.accept();
    });
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    dlg.exec();
}

} // namespace cgplay
