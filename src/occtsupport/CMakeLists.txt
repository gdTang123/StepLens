# occtsupport：版本兼容层。catchOcctAsResult 在头里，cpp 只为了两个 OCCT 版本都实例化一次。
steplens_add_module(occtsupport
    OcctExceptions.cpp
    OcctExceptions.h
    occt_compat.h)
target_link_libraries(steplens_occtsupport PUBLIC steplens::core steplens::occt_kernel)

# 只有这个静态库使用 /EHa。不能并进上面的目标，否则同一条编译命令里会同时出现 /EHsc。
steplens_add_module(occtboundary ASYNCHRONOUS_EXCEPTIONS
    OcctBoundary.cpp
    OcctBoundary.h)
target_link_libraries(steplens_occtboundary PUBLIC steplens::core steplens::occt_kernel)
target_link_libraries(steplens_occtsupport PUBLIC steplens::occtboundary)
