#include "core/Version.h"

#include <gtest/gtest.h>

TEST(Version, IsSet) {
    EXPECT_FALSE(steplens::kVersion.empty());
    EXPECT_EQ(steplens::kVersion, "0.1.0");
}
