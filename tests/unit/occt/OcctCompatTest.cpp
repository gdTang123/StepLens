#include "core/Result.h"
#include "io/OcctExceptions.h"
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
    const auto fromOcct = steplens::io::catchOcctAsResult([] { throw Standard_DomainError("boom"); },
                                                          steplens::ErrorCode::TransferFailed);
    ASSERT_FALSE(fromOcct.ok());
    EXPECT_EQ(fromOcct.error().code, steplens::ErrorCode::TransferFailed);
    EXPECT_EQ(fromOcct.error().message, "boom");

    const auto fromStd = steplens::io::catchOcctAsResult([] { throw std::runtime_error("x"); });
    ASSERT_FALSE(fromStd.ok());
    EXPECT_EQ(fromStd.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(fromStd.error().message, "x");

    const auto fromOther = steplens::io::catchOcctAsResult([] { throw 42; });
    ASSERT_FALSE(fromOther.ok());
    EXPECT_EQ(fromOther.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(fromOther.error().message, "unknown exception");
}
