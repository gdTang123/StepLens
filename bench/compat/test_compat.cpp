// 双版本兼容的最小单测（不依赖 GTest，供 T02b 冒烟；仓库里改写成 GTest）
#include "occtsupport/occt_compat.h"

#include <NCollection_Sequence.hxx>
#include <Standard_DomainError.hxx>
#include <Standard_Transient.hxx>
#include <TDF_Label.hxx>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#ifndef STEPLENS_EXPECTED_OCCT
#define STEPLENS_EXPECTED_OCCT ""
#endif
struct Dummy : Standard_Transient {};
static int fails = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                          \
            ++fails;                                                                                         \
        }                                                                                                    \
    } while (0)
static std::string boundary(void (*f)()) { // 模拟模块边界：先捕获 Standard_Failure，再捕获 std::exception
    try {
        f();
        return "ok";
    } catch (const Standard_Failure& e) {
        return std::string("occt:") + steplens::occt::failureMessage(e);
    } catch (const std::exception& e) {
        return std::string("std:") + e.what();
    }
}
int main() {
    std::printf("occt_version=%s\n", steplens::occt::versionString());
    if (std::strlen(STEPLENS_EXPECTED_OCCT))
        CHECK(std::strcmp(steplens::occt::versionString(), STEPLENS_EXPECTED_OCCT) == 0); // 防止配错清单
    CHECK(boundary([] { throw Standard_DomainError("boom"); }) == "occt:boom");
    CHECK(boundary([] { throw std::runtime_error("x"); }) == "std:x");
    opencascade::handle<Dummy> h = new Dummy(); // 两版通用的句柄写法
    CHECK(!h.IsNull());
    NCollection_Sequence<TDF_Label> seq; // 代替 8.0 中弃用的 TDF_LabelSequence（7.9.3 中两者等价）
    CHECK(seq.IsEmpty());
    std::printf(fails ? "RESULT=FAIL\n" : "RESULT=PASS\n");
    return fails ? 1 : 0;
}
