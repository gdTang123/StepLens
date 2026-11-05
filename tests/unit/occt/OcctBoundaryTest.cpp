#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "core/TaskScheduler.h"
#include "occtsupport/OcctBoundary.h"

#include <gtest/gtest.h>
#include <string>

// clang-format off
#include <Windows.h>
// clang-format on

TEST(OcctBoundary, NullAccessOnWorkerBecomesInternalAndMarksProcess) {
    // 接不住的访问违例不要弹出 WER 对话框，否则 CI 会停在那里。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ASSERT_FALSE(steplens::occt::processContaminated());

    steplens::Result<int> result{0};
    {
        // 工作线程入口注入信号转换，再经 OcctBoundary 触发空指针访问。
        steplens::TaskScheduler scheduler(1, [] { steplens::occt::OcctBoundary::armWorkerThread(); });
        auto future = scheduler.submit([] { return steplens::occt::OcctBoundary::probeNullPointerAccess(); });
        result = future.get();
    }

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, steplens::ErrorCode::Internal);
    EXPECT_EQ(result.error().userMessage, std::string{steplens::occt::kProcessContaminatedUserMessage});
    EXPECT_TRUE(steplens::occt::processContaminated());
}
