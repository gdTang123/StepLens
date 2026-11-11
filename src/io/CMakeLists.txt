# io：ILoader、StepLoader、IgesLoader、ProgressStreamBuf、OcctProgressBridge（T05）
# 只链数据交换工具包。TKOpenGl 在 vis，不进 io。
steplens_add_module(io
    ILoader.cpp
    ILoader.h
    HeapRelease.h
    LoadInternal.cpp
    LoadInternal.h
    ProgressStreamBuf.cpp
    ProgressStreamBuf.h
    OcctProgressBridge.cpp
    OcctProgressBridge.h
    StepLoader.cpp
    StepLoader.h
    IgesLoader.cpp
    IgesLoader.h)
target_link_libraries(steplens_io PUBLIC steplens::core steplens::occtsupport steplens::occt_io)
