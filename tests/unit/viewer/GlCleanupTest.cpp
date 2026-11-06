#include "viewer/GlCleanup.h"

#include <gtest/gtest.h>

TEST(GlCleanup, NoResourcesIsIdempotent) {
    const steplens::GlCleanupPlan plan =
        steplens::planGlCleanup(steplens::GlCleanupInput{false, true, false});
    EXPECT_FALSE(plan.makeCurrent);
    EXPECT_FALSE(plan.releaseResources);
    EXPECT_FALSE(plan.doneCurrent);
}

TEST(GlCleanup, ValidContextMakesCurrentThenReleases) {
    const steplens::GlCleanupPlan plan = steplens::planGlCleanup(steplens::GlCleanupInput{true, true, false});
    EXPECT_TRUE(plan.makeCurrent);
    EXPECT_TRUE(plan.releaseResources);
    EXPECT_TRUE(plan.doneCurrent);
}

TEST(GlCleanup, AlreadyCurrentDoesNotTouchThePair) {
    const steplens::GlCleanupPlan plan = steplens::planGlCleanup(steplens::GlCleanupInput{true, true, true});
    EXPECT_FALSE(plan.makeCurrent);
    EXPECT_TRUE(plan.releaseResources);
    EXPECT_FALSE(plan.doneCurrent);
}

TEST(GlCleanup, DeadContextDropsResourcesWithoutMakeCurrent) {
    const steplens::GlCleanupPlan plan =
        steplens::planGlCleanup(steplens::GlCleanupInput{true, false, false});
    EXPECT_FALSE(plan.makeCurrent);
    EXPECT_TRUE(plan.releaseResources);
    EXPECT_FALSE(plan.doneCurrent);
}
