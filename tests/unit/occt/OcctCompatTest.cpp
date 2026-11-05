#include "core/Result.h"
#include "occtsupport/OcctExceptions.h"
#include "occtsupport/occt_compat.h"

#include <Standard_DomainError.hxx>

#include <cstring>
#include <gtest/gtest.h>
#include <stdexcept>

#ifndef STEPLENS_EXPECTED_OCCT
#define STEPLENS_EXPECTED_OCCT ""
#endif

TEST(OcctCompat, VersionMatchesManifest) {
    EXPECT_STREQ(steplens::occt::versionString(), STEPLENS_EXPECTED_OCCT);
}

TEST(OcctCompat, FailureStaysInsideResult) {
    const auto fromOcct = steplens::occt::catchOcctAsResult(
        []() -> int { throw Standard_DomainError("boom"); }, steplens::ErrorCode::TransferFailed);
    ASSERT_FALSE(fromOcct.ok());
    EXPECT_EQ(fromOcct.error().code, steplens::ErrorCode::TransferFailed);
    EXPECT_EQ(fromOcct.error().message, "boom");
    EXPECT_EQ(fromOcct.error().userMessage, "读取或计算时出错");

    const auto fromStd = steplens::occt::catchOcctAsResult([]() -> int { throw std::runtime_error("x"); });
    ASSERT_FALSE(fromStd.ok());
    EXPECT_EQ(fromStd.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(fromStd.error().message, "x");

    const auto fromOther = steplens::occt::catchOcctAsResult([]() -> int { throw 42; });
    ASSERT_FALSE(fromOther.ok());
    EXPECT_EQ(fromOther.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(fromOther.error().message, "unknown exception");
}

TEST(OcctCompat, CustomUserMessage) {
    const auto result = steplens::occt::catchOcctAsResult([]() -> int { throw Standard_DomainError("boom"); },
                                                          steplens::ErrorCode::ParseFailed, "这不是默认文案");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::ParseFailed);
    EXPECT_EQ(result.error().message, "boom");
    EXPECT_EQ(result.error().userMessage, "这不是默认文案");
}
