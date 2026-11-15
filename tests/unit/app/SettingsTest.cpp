#include "app/settings/Settings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <QSettings>
#endif

namespace {

QString clean(const QString& path) {
    return QDir::cleanPath(path);
}

} // namespace

TEST(Settings, WritesIniBesideExecutableWhenWritableAndUsesDefaults) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    steplens::Settings settings(root.path(), root.path() + QStringLiteral("/config"));

    EXPECT_EQ(settings.format(), QSettings::IniFormat);
    EXPECT_EQ(clean(settings.fileName()), clean(root.filePath(QStringLiteral("StepLens.ini"))));
    EXPECT_DOUBLE_EQ(settings.viewer().smallObjectCullPx, 4.0);
    EXPECT_DOUBLE_EQ(settings.memory().softLimitRatio, 0.60);
    EXPECT_DOUBLE_EQ(settings.memory().hardLimitRatio, 0.80);
    EXPECT_DOUBLE_EQ(settings.mesh().coarseCoef, 0.004);

    settings.addRecentFile(QStringLiteral("part.step"));
    ASSERT_FALSE(settings.recentFiles().isEmpty());
    EXPECT_EQ(settings.recentFiles().front(), QStringLiteral("part.step"));
    EXPECT_TRUE(QFile::exists(settings.fileName()));

#ifdef _WIN32
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\StepLens"), QSettings::NativeFormat);
    EXPECT_TRUE(registry.childKeys().isEmpty());
    EXPECT_TRUE(registry.childGroups().isEmpty());
#endif
}

TEST(Settings, FallsBackWhenExecutableDirectoryCannotBeCreated) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString blocker = root.filePath(QStringLiteral("not-a-directory"));
    QFile file(blocker);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();

    steplens::Settings settings(blocker + QStringLiteral("/app"), root.filePath(QStringLiteral("config")));
    EXPECT_EQ(settings.format(), QSettings::IniFormat);
    EXPECT_EQ(clean(settings.fileName()),
              clean(root.filePath(QStringLiteral("config/StepLens/StepLens.ini"))));
}
