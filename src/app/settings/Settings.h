#pragma once

#include <QSettings>
#include <QStringList>

#include <memory>

namespace steplens {

struct ViewerSettings {
    int msaaSamples = 4;
    bool edgesByDefault = true;
    int degradeDelayMs = 300;
    double smallObjectCullPx = 4.0;
};
struct MeshSettings {
    double coarseCoef = 0.004;
    double fineCoef = 0.001;
    double coarseAngleDeg = 30.0;
    double fineAngleDeg = 20.0;
    bool refineAfterLoad = true;
};
struct MemorySettings {
    double softLimitRatio = 0.60;
    double hardLimitRatio = 0.80;
    bool configInvalid = false;
};

// IniFormat only. Defaults to executable-dir/StepLens.ini; fallback is %APPDATA%/StepLens
// on Windows and XDG_CONFIG_HOME/StepLens or ~/.config/StepLens on Linux.
class Settings final {
public:
    Settings();
    // Explicit paths keep tests isolated; fallbackRoot is the parent of the StepLens directory.
    Settings(const QString& executableDirectory, const QString& fallbackRoot);
    [[nodiscard]] ViewerSettings viewer() const;
    [[nodiscard]] MeshSettings mesh() const;
    [[nodiscard]] MemorySettings memory() const;
    [[nodiscard]] QStringList recentFiles() const;
    [[nodiscard]] bool addRecentFile(const QString& path);
    [[nodiscard]] bool removeRecentFile(const QString& path);
    [[nodiscard]] bool associationChosen() const;
    [[nodiscard]] bool setAssociationChosen(bool on);
    [[nodiscard]] QStringList savedAssociations() const;
    [[nodiscard]] bool setSavedAssociations(const QStringList& values);
    [[nodiscard]] QString fileName() const;
    [[nodiscard]] QSettings::Format format() const;
    static Settings& instance();

private:
    void initialize(const QString& executableDirectory, const QString& fallbackRoot);
    bool saveWithFallback();
    std::unique_ptr<QSettings> m_ini;
    QString m_fallbackPath;
};

} // namespace steplens
