#include "core/Result.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <variant>

TEST(Result, HoldsValue) {
    const steplens::Result<int> result{7};
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 7);
}

TEST(Result, HoldsError) {
    const steplens::Result<int> result{
        steplens::Error{steplens::ErrorCode::ParseFailed, "bad step", "文件损坏"}};
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::ParseFailed);
    EXPECT_EQ(result.error().message, "bad step");
    EXPECT_EQ(result.error().userMessage, "文件损坏");
    EXPECT_THROW((void)result.value(), std::bad_variant_access);
}

TEST(CatchAsResult, MapsCancellationAllocAndException) {
    const auto cancelled = steplens::catchAsResult([]() -> int { throw steplens::CancelledException{}; });
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.error().code, steplens::ErrorCode::Cancelled);
    EXPECT_EQ(cancelled.error().userMessage, "已取消");

    const auto memory = steplens::catchAsResult([]() -> int { throw std::bad_alloc(); });
    ASSERT_FALSE(memory.ok());
    EXPECT_EQ(memory.error().code, steplens::ErrorCode::OutOfMemory);
    EXPECT_FALSE(memory.error().userMessage.empty());

    const auto failed = steplens::catchAsResult([]() -> int { throw std::runtime_error("mesh failed"); });
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(failed.error().message, "mesh failed");
    EXPECT_EQ(failed.error().userMessage, "内部错误");

    const auto ok = steplens::catchAsResult([]() { return 4; });
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), 4);
}
