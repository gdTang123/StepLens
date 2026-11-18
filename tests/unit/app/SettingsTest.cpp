#include "app/settings/Settings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <gtest/gtest.h>

#if defined(__linux__)
#include <unistd.h>
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

    ASSERT_TRUE(settings.addRecentFile(QStringLiteral("part.step")));
    ASSERT_FALSE(settings.recentFiles().isEmpty());
    EXPECT_EQ(settings.recentFiles().front(), QStringLiteral("part.step"));
    EXPECT_TRUE(QFile::exists(settings.fileName()));
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
        GTEST_SKIP() << "uid=" << static_cast<unsigned long>(getuid())
                     << " 当前进程仍可在 chmod 0555 目录创建文件";
    }
    EXPECT_FALSE(probe.isOpen()) << "uid=" << static_cast<unsigned long>(getuid());
    steplens::Settings settings(appDirectory, root.filePath(QStringLiteral("config")));
    EXPECT_EQ(clean(settings.fileName()),
              clean(root.filePath(QStringLiteral("config/StepLens/StepLens.ini"))));
    EXPECT_FALSE(probe.exists());
    EXPECT_TRUE(settings.addRecentFile(QStringLiteral("initial.step")));
    QFile fallback(settings.fileName());
    ASSERT_TRUE(fallback.open(QIODevice::ReadOnly));
    EXPECT_TRUE(QString::fromUtf8(fallback.readAll()).contains(QStringLiteral("initial.step")));
#else
    GTEST_SKIP() << "目录只读权限验证仅在 Linux 上执行";
#endif
}

TEST(Settings, RealSaveFailureMigratesAllValuesAndRecentFileToFallback) {
#if defined(__linux__)
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString appDirectory = root.filePath(QStringLiteral("app"));
    const QString fallbackIni = root.filePath(QStringLiteral("config/StepLens/StepLens.ini"));
    ASSERT_TRUE(QDir().mkpath(appDirectory));
    {
        QSettings ini(QDir(appDirectory).filePath(QStringLiteral("StepLens.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("recentFiles"), QStringList{QStringLiteral("before.step")});
        ini.setValue(QStringLiteral("viewer/msaaSamples"), 8);
        ini.setValue(QStringLiteral("mesh/coarseCoef"), 0.012);
        ini.setValue(QStringLiteral("memory/softLimitRatio"), 0.55);
        ini.sync();
        ASSERT_EQ(ini.status(), QSettings::NoError);
    }
    steplens::Settings settings(appDirectory, root.filePath(QStringLiteral("config")));
    ASSERT_EQ(settings.viewer().msaaSamples, 8);
    ASSERT_DOUBLE_EQ(settings.mesh().coarseCoef, 0.012);
    ASSERT_DOUBLE_EQ(settings.memory().softLimitRatio, 0.55);
    ASSERT_TRUE(QFile::setPermissions(appDirectory, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                                        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                                        QFileDevice::ReadOther | QFileDevice::ExeOther));
    QTemporaryFile probe(QDir(appDirectory).filePath(QStringLiteral("probe-XXXXXX")));
    ASSERT_FALSE(probe.open()) << "uid=" << static_cast<unsigned long>(getuid());
    EXPECT_TRUE(settings.addRecentFile(QStringLiteral("after-fallback.step")));
    EXPECT_EQ(clean(settings.fileName()), clean(fallbackIni));
    QSettings migrated(settings.fileName(), QSettings::IniFormat);
    migrated.sync();
    EXPECT_EQ(migrated.status(), QSettings::NoError);
    QFile disk(settings.fileName());
    ASSERT_TRUE(disk.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(disk.readAll());
    EXPECT_TRUE(contents.contains(QStringLiteral("after-fallback.step")));
    EXPECT_TRUE(contents.contains(QStringLiteral("before.step")));
    EXPECT_TRUE(contents.contains(QStringLiteral("[viewer]")));
    EXPECT_TRUE(contents.contains(QStringLiteral("msaaSamples=8")));
    EXPECT_TRUE(contents.contains(QStringLiteral("[mesh]")));
    EXPECT_TRUE(contents.contains(QStringLiteral("[memory]")));
    EXPECT_TRUE(contents.contains(QStringLiteral("softLimitRatio=0.55")));
    QFile::setPermissions(appDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                            QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                            QFileDevice::ExeOther);
#else
    GTEST_SKIP() << "真实 chmod 保存失败回退场景仅在 Linux 执行";
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
