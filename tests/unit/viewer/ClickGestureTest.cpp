#include "viewer/ClickGesture.h"

#include <gtest/gtest.h>

TEST(ClickGesture, ShortClickAllowsSmallJitter) {
    steplens::viewer::ClickGesture gesture;
    gesture.press(10.0, 10.0);
    gesture.move(14.0, 13.0);
    EXPECT_TRUE(gesture.isClick(14.0, 13.0));
}

TEST(ClickGesture, StraightDragIsNotClick) {
    steplens::viewer::ClickGesture gesture;
    gesture.press(0.0, 0.0);
    gesture.move(7.0, 0.0);
    EXPECT_FALSE(gesture.isClick(7.0, 0.0));
}

TEST(ClickGesture, DragReturningToOriginIsNotClick) {
    steplens::viewer::ClickGesture gesture;
    gesture.press(0.0, 0.0);
    gesture.move(7.0, 0.0);
    gesture.move(0.5, 0.0);
    EXPECT_FALSE(gesture.isClick(0.5, 0.0));
}
