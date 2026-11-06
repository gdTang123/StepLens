# io：ILoader、StepLoader、IgesLoader、ProgressStreamBuf、OcctProgressBridge（T05）
# 只链数据交换工具包。TKOpenGl 在 vis，不进 io。
steplens_add_module(io Anchor.cpp)
target_link_libraries(steplens_io PUBLIC steplens::occt_io)
