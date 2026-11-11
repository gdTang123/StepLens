#include "viewer/SceneHostBinding.h"

#include <gtest/gtest.h>

namespace {

TEST(SceneHostBinding, ReleaseKeepsHostAndRebindUsesNewContext) {
    steplens::SceneHostBinding slot;
    int firstContext = 0;
    int secondContext = 0;
    slot.bind(&firstContext);
    EXPECT_TRUE(slot.hostAlive);
    EXPECT_TRUE(slot.builderAlive);
    EXPECT_EQ(slot.context, &firstContext);

    const bool hostBefore = slot.hostAlive;
    slot.releaseScene();
    EXPECT_EQ(slot.hostAlive, hostBefore);
    EXPECT_TRUE(slot.hostAlive);
    EXPECT_FALSE(slot.builderAlive);
    EXPECT_EQ(slot.context, nullptr);

    slot.bind(&secondContext);
    EXPECT_TRUE(slot.hostAlive);
    EXPECT_TRUE(slot.builderAlive);
    EXPECT_EQ(slot.context, &secondContext);
    EXPECT_NE(slot.context, &firstContext);
}

} // namespace
