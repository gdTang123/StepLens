#include "app/settings/Settings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StepLens", 0, KEY_READ, &key);
    if (result == ERROR_SUCCESS)
        RegCloseKey(key);
    EXPECT_EQ(result, ERROR_FILE_NOT_FOUND);
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

TEST(Settings, FallsBackWhenExecutableDirectoryExistsButCannotBeWritten) {
#if defined(__linux__)
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString appDirectory = root.filePath(QStringLiteral("app"));
    ASSERT_TRUE(QDir().mkpath(appDirectory));
    ASSERT_TRUE(QFile::setPermissions(appDirectory, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                                        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                                        QFileDevice::ReadOther | QFileDevice::ExeOther));
    QTemporaryFile probe(QDir(appDirectory).filePath(QStringLiteral("probe-XXXXXX")));
    if (probe.open()) {
        probe.remove();
        GTEST_SKIP() << "当前进程（可能为 root）仍可在 chmod 0555 目录创建文件";
    }
    steplens::Settings settings(appDirectory, root.filePath(QStringLiteral("config")));
    EXPECT_EQ(clean(settings.fileName()),
              clean(root.filePath(QStringLiteral("config/StepLens/StepLens.ini"))));
#else
    GTEST_SKIP() << "目录只读权限验证仅在 Linux 上执行";
#endif
}

TEST(Settings, InvalidMemoryRatiosFallBackToDefaults) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString iniPath = root.filePath(QStringLiteral("StepLens.ini"));
    {
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.setValue(QStringLiteral("memory/softLimitRatio"), QStringLiteral("NaN"));
        ini.setValue(QStringLiteral("memory/hardLimitRatio"), 0.8);
        ini.sync();
    }
    steplens::Settings settings(root.path(), root.filePath(QStringLiteral("config")));
    const auto limits = settings.memory();
    EXPECT_DOUBLE_EQ(limits.softLimitRatio, 0.60);
    EXPECT_DOUBLE_EQ(limits.hardLimitRatio, 0.80);
    EXPECT_TRUE(limits.configInvalid);
}

TEST(Settings, NonNumericAndInvertedMemoryRatiosFallBackToDefaults) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString iniPath = root.filePath(QStringLiteral("StepLens.ini"));
    {
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.setValue(QStringLiteral("memory/softLimitRatio"), 0.9);
        ini.setValue(QStringLiteral("memory/hardLimitRatio"), 0.8);
        ini.sync();
    }
    steplens::Settings settings(root.path(), root.filePath(QStringLiteral("config")));
    const auto limits = settings.memory();
    EXPECT_DOUBLE_EQ(limits.softLimitRatio, 0.60);
    EXPECT_DOUBLE_EQ(limits.hardLimitRatio, 0.80);
    EXPECT_TRUE(limits.configInvalid);
}

TEST(Settings, NonNumericMemoryRatiosFallBackToDefaults) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    {
        QSettings ini(root.filePath(QStringLiteral("StepLens.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("memory/softLimitRatio"), QStringLiteral("not-a-number"));
        ini.setValue(QStringLiteral("memory/hardLimitRatio"), 0.8);
        ini.sync();
    }
    steplens::Settings settings(root.path(), root.filePath(QStringLiteral("config")));
    const auto limits = settings.memory();
    EXPECT_DOUBLE_EQ(limits.softLimitRatio, 0.60);
    EXPECT_DOUBLE_EQ(limits.hardLimitRatio, 0.80);
    EXPECT_TRUE(limits.configInvalid);
}
