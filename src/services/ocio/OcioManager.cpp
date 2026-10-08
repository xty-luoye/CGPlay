// CGPlay OcioManager.cpp

#include "OcioManager.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
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
    if (configPath.isEmpty()) return;

#ifdef CGPLAY_OCIO_ENABLED
    try {
        _p->config = OCIO::Config::CreateFromFile(configPath.toStdString().c_str());
        _p->configPath = configPath;
        _p->enabled    = true;

        // Reset to sensible defaults for new config
        if (_p->config->getNumDisplays() > 0) {
            _p->display = QString::fromStdString(_p->config->getDefaultDisplay());
        }
        const char* defView = _p->config->getDefaultView(_p->display.toStdString().c_str());
        _p->view = defView ? QString::fromStdString(defView) : "sRGB";

        qDebug() << "[OCIO] Loaded config:" << configPath;
        Q_EMIT configLoaded(configPath);
        _rebuildOptions();

    } catch (const OCIO::Exception& e) {
        qWarning() << "[OCIO] Failed to load config:" << e.what();
        _p->enabled = false;
    }
#else
    _p->configPath = configPath;
    _p->enabled    = false;
    qWarning() << "[OCIO] OpenColorIO not compiled in.";
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
        try {
            _p->config     = OCIO::Config::CreateFromBuiltinConfig(candidate);
            _p->configPath = QString::fromStdString(candidate);
            _p->enabled    = true;

            if (_p->config->getNumDisplays() > 0) {
                _p->display = QString::fromStdString(_p->config->getDefaultDisplay());
            }

            // ACES 1.2 default view
            const char* defView = _p->config->getDefaultView(_p->display.toStdString().c_str());
            _p->view = defView ? QString::fromStdString(defView) : "ACES 1.0 SDR-video";

            // Keep the scene-linear role as the safe default for EXR. The
            // previous automatic ACEScg selection changed Nuke scene-linear
            // EXRs' exposure and saturation; ACEScg remains user-selectable.
            _p->input = QStringLiteral("scene_linear");

            qDebug() << "[OCIO] ACES 1.2 config loaded via:" << candidate;
            Q_EMIT configLoaded(_p->configPath);
            _rebuildOptions();
            return; // Success — stop trying

        } catch (const OCIO::Exception& e) {
            qDebug() << "[OCIO] Builtin config not available:" << candidate
                     << "—" << e.what();
        }
    }

    // All candidates failed — fall back to linear/sRGB
    qWarning() << "[OCIO] No ACES builtin config found. Falling back to linear/sRGB.";
    _p->enabled = false;
    _p->display = "sRGB";
    _p->view    = "sRGB";

#else
    _p->enabled = false;
    _p->display = "ACES sRGB";
    _p->view    = "ACES 1.0 SDR-video";
#endif
}

void OcioManager::loadBuiltinConfig(const QString& name)
{
#ifdef CGPLAY_OCIO_ENABLED
    try {
        std::string builtinName;
        if (name == "ACES") {
            builtinName = "ocio://default";
        } else {
            builtinName = "ocio://" + name.toLower().toStdString();
        }
        _p->config     = OCIO::Config::CreateFromBuiltinConfig(builtinName.c_str());
        _p->configPath = QString::fromStdString(builtinName);
        _p->enabled    = true;

        if (_p->config->getNumDisplays() > 0) {
            _p->display = QString::fromStdString(_p->config->getDefaultDisplay());
        }
        const char* defView = _p->config->getDefaultView(_p->display.toStdString().c_str());
        _p->view = defView ? QString::fromStdString(defView) : "sRGB";

        qDebug() << "[OCIO] Loaded builtin config:" << name;
        _rebuildOptions();

    } catch (const OCIO::Exception& e) {
        qWarning() << "[OCIO] Builtin config not available:" << e.what()
                   << "— falling back to linear/sRGB";
        _p->enabled = false;
        _p->display = "sRGB";
        _p->view    = "sRGB";
    }
#else
    _p->enabled = false;
    _p->display = (name == "ACES") ? "ACES sRGB" : name;
    _p->view    = name;
#endif
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
    // exposure/gamma fields. These are managed via OcioManager
    // internally and applied through separate color adjustments.
#endif
    return opts;
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
    _p->exposure = ev;
    _rebuildOptions();
}

void OcioManager::setGamma(float g)
{
    _p->gamma = g;
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
    auto* lblConfig = new QLabel(_p->configPath.isEmpty() ? "(内置)" : _p->configPath, &dlg);
    lblConfig->setWordWrap(true);
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

    // Update views when display changes
    connect(cbDisplay, &QComboBox::currentTextChanged, &dlg, [&](const QString& d) {
        cbView->clear();
        cbView->addItems(availableViews(d));
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
    vlay->addWidget(btns);

    // Browse external .ocio file
    connect(btnBrowse, &QPushButton::clicked, &dlg, [&] {
        QString path = QFileDialog::getOpenFileName(
            &dlg, "打开 OCIO 配置", {}, "OCIO 配置 (*.ocio);;所有文件 (*)");
        if (!path.isEmpty()) lblConfig->setText(path);
    });

    // Reset to builtin config
    connect(btnBuiltin, &QPushButton::clicked, &dlg, [&] {
        lblConfig->setText("(内置)");
    });

    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        // Enable/disable
        setEnabled(chkEnabled->isChecked());

        // Config file
        QString cfgPath = lblConfig->text();
        if (cfgPath == "(内置)") {
            // Switch back to builtin ACES config
            if (!_p->configPath.startsWith("ocio://")) {
                loadAces12Config();
            }
        } else if (cfgPath != _p->configPath && !cfgPath.isEmpty()) {
            loadConfig(cfgPath);
        }

        // Color settings (only apply if enabled)
        if (chkEnabled->isChecked()) {
            setInput(cbInput->currentText());
            setDisplay(cbDisplay->currentText());
            setView(cbView->currentText());
            setExposure(static_cast<float>(spExposure->value()));
            setGamma(static_cast<float>(spGamma->value()));
        }
    }
}

} // namespace cgplay
