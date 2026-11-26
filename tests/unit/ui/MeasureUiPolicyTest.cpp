#include "app/MeasureUiPolicy.h"

#include <gtest/gtest.h>

TEST(MeasureUiPolicy, BlocksReadyCompletedWhileMeasureActive) {
    EXPECT_FALSE(steplens::measureModeBlocksReadyCompleted(false, false));
    EXPECT_TRUE(steplens::measureModeBlocksReadyCompleted(true, false));
    EXPECT_TRUE(steplens::measureModeBlocksReadyCompleted(false, true));
    EXPECT_TRUE(steplens::measureModeBlocksReadyCompleted(true, true));
}
