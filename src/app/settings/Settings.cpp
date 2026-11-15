#include "app/settings/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace steplens {
namespace {

bool writableDirectory(const QString& directory) {
    QDir dir;
    if (!dir.mkpath(directory))
        return false;
    const QFileInfo info(directory);
    return info.isDir() && info.isWritable();
}

} // namespace

Settings::Settings() {
#if defined(_WIN32)
    const QString fallback = qEnvironmentVariable("APPDATA");
#else
    QString fallback = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (fallback.isEmpty())
        fallback = QDir::homePath() + QStringLiteral("/.config");
#endif
    initialize(QCoreApplication::applicationDirPath(), fallback);
}

Settings::Settings(const QString& executableDirectory, const QString& fallbackRoot) {
    initialize(executableDirectory, fallbackRoot);
}

void Settings::initialize(const QString& executableDirectory, const QString& fallbackRoot) {
    QString path;
    if (writableDirectory(executableDirectory)) {
        path = QDir(executableDirectory).filePath(QStringLiteral("StepLens.ini"));
    } else {
        const QString directory = QDir(fallbackRoot).filePath(QStringLiteral("StepLens"));
        QDir().mkpath(directory);
        path = QDir(directory).filePath(QStringLiteral("StepLens.ini"));
    }
    // IniFormat with an explicit file path: never NativeFormat, never the Windows registry.
    m_ini = std::make_unique<QSettings>(path, QSettings::IniFormat);
}

ViewerSettings Settings::viewer() const {
    return {m_ini->value("viewer/msaaSamples", 4).toInt(),
            m_ini->value("viewer/edgesByDefault", true).toBool(),
            m_ini->value("viewer/degradeDelayMs", 300).toInt(),
            m_ini->value("viewer/smallObjectCullPx", 4.0).toDouble()};
}

MeshSettings Settings::mesh() const {
    return {m_ini->value("mesh/coarseCoef", 0.004).toDouble(),
            m_ini->value("mesh/fineCoef", 0.001).toDouble(),
            m_ini->value("mesh/coarseAngleDeg", 30.0).toDouble(),
            m_ini->value("mesh/fineAngleDeg", 20.0).toDouble(),
            m_ini->value("mesh/refineAfterLoad", true).toBool()};
}

MemorySettings Settings::memory() const {
    return {m_ini->value("memory/softLimitRatio", 0.60).toDouble(),
            m_ini->value("memory/hardLimitRatio", 0.80).toDouble()};
}

QStringList Settings::recentFiles() const {
    return m_ini->value("recentFiles").toStringList();
}

void Settings::addRecentFile(const QString& path) {
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 10)
        files.removeLast();
    m_ini->setValue("recentFiles", files);
    m_ini->sync();
}

QString Settings::fileName() const {
    return m_ini->fileName();
}
QSettings::Format Settings::format() const {
    return m_ini->format();
}
Settings& Settings::instance() {
    static Settings settings;
    return settings;
}

} // namespace steplens
