#include "app/settings/Settings.h"

#include "core/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <cmath>

namespace steplens {
namespace {

bool writableDirectory(const QString& directory) {
    QDir dir;
    if (!dir.exists(directory))
        return false;
    QTemporaryFile probe(QDir(directory).filePath(QStringLiteral("StepLens-write-test-XXXXXX.tmp")));
    if (!probe.open())
        return false;
    return probe.remove();
}

QString absoluteFallbackDirectory(const QString& fallbackRoot) {
    QString directory = fallbackRoot;
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        directory = QDir(QDir::tempPath()).filePath(QStringLiteral("StepLens"));
    else if (!fallbackRoot.isEmpty())
        directory = QDir(directory).filePath(QStringLiteral("StepLens"));
    return QDir::cleanPath(directory);
}

std::unique_ptr<QSettings> openSettings(const QString& path) {
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    settings->sync();
    return settings;
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
    const QString fallbackDirectory = absoluteFallbackDirectory(fallbackRoot);
    m_fallbackPath = QDir(fallbackDirectory).filePath(QStringLiteral("StepLens.ini"));
    QString path;
    if (writableDirectory(executableDirectory)) {
        path = QDir(executableDirectory).filePath(QStringLiteral("StepLens.ini"));
    } else {
        QDir().mkpath(fallbackDirectory);
        path = QDir(fallbackDirectory).filePath(QStringLiteral("StepLens.ini"));
    }
    // IniFormat with an explicit file path: never NativeFormat, never the Windows registry.
    m_ini = openSettings(path);
    if (m_ini->status() != QSettings::NoError && path != m_fallbackPath) {
        QDir().mkpath(fallbackDirectory);
        m_ini = openSettings(m_fallbackPath);
    }
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
    bool softOk = false;
    bool hardOk = false;
    const double soft = m_ini->value("memory/softLimitRatio", 0.60).toDouble(&softOk);
    const double hard = m_ini->value("memory/hardLimitRatio", 0.80).toDouble(&hardOk);
    if (!softOk || !hardOk || !std::isfinite(soft) || !std::isfinite(hard) || soft <= 0.0 || hard <= 0.0 ||
        soft > 1.0 || hard > 1.0 || soft > hard) {
        Log::warn("内存比例配置非法，已回落到默认值 0.60/0.80");
        return {0.60, 0.80, true};
    }
    return {soft, hard};
}

QStringList Settings::recentFiles() const {
    return m_ini->value("recentFiles").toStringList();
}

bool Settings::addRecentFile(const QString& path) {
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 10)
        files.removeLast();
    m_ini->setValue("recentFiles", files);
    return saveWithFallback();
}

bool Settings::saveWithFallback() {
    m_ini->sync();
    if (m_ini->status() == QSettings::NoError)
        return true;

    const QStringList keys = m_ini->allKeys();
    QVariantMap values;
    for (const QString& key : keys)
        values.insert(key, m_ini->value(key));

    QDir().mkpath(QFileInfo(m_fallbackPath).absolutePath());
    m_ini = std::make_unique<QSettings>(m_fallbackPath, QSettings::IniFormat);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        m_ini->setValue(it.key(), it.value());
    m_ini->sync();
    return m_ini->status() == QSettings::NoError;
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
