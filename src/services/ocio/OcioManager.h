#pragma once
// CGPlay OcioManager.h
// OpenColorIO v2 wrapper — manages config, views, displays, exposure, gamma

#include <QObject>
#include <QImage>
#include <QString>
#include <QStringList>
#include <memory>

// TlViewport.h unconditionally includes the real
// <tlRender/Timeline/ColorOptions.h>.  We must use the same type to
// avoid a C++ ODR violation.  Fall back to a minimal stub only when
// the real header is truly absent.
#if __has_include(<tlRender/Timeline/ColorOptions.h>)
  #include <tlRender/Timeline/ColorOptions.h>
#else
  // Minimal stub matching the layout OcioManager.cpp expects
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

class OcioManager : public QObject
{
    Q_OBJECT
public:
    struct PreviewTransformSettings
    {
        bool enabled = false;
        QString configPath;
        QString input;
        QString display;
        QString view;
    };

    explicit OcioManager(QObject* parent = nullptr);
    ~OcioManager() override;

    // ── Config ────────────────────────────────────────────────────────────────
    void loadConfig(const QString& configPath);
    void loadBuiltinConfig(const QString& name = "ACES");
    void loadAces12Config();
    QString currentConfigPath() const;
    PreviewTransformSettings previewTransformSettings() const;
    static bool applyPreviewTransform(QImage& image, const PreviewTransformSettings& settings);

    // ── Current options ───────────────────────────────────────────────────────
    tl::OCIOOptions currentOptions() const;

    // ── Color space / display / view ──────────────────────────────────────────
    QStringList availableColorSpaces()  const;
    QStringList availableDisplays()     const;
    QStringList availableViews(const QString& display) const;

    QString currentInput()   const;
    QString currentDisplay() const;
    QString currentView()    const;
    float   exposure()       const;
    float   gamma()          const;

    Q_INVOKABLE bool isEnabled() const;
public Q_SLOTS:
    void setEnabled(bool enabled);

public Q_SLOTS:
    void setInput(const QString& cs);
    void setDisplay(const QString& disp);
    void setView(const QString& view);
    void setExposure(float ev);
    void setGamma(float gamma);
    // Apply the ACES scene-linear transform used by Nuke for unlabelled EXR.
    void applyExrSceneLinearDefaults();
    // Restore the user's OCIO state when leaving an EXR media item.
    void clearExrSceneLinearDefaults();

    void showSettings(QWidget* parent = nullptr);

Q_SIGNALS:
    void optionsChanged(const tl::OCIOOptions& opts);
    void displayChanged(const QString& display);
    void configLoaded(const QString& configPath);
    void enabledChanged(bool enabled);

private:
    void _rebuildOptions();

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
