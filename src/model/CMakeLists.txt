# model：DocumentModel、AssemblyNode、PrototypeInfo（T06）
# 公开类型带 XCAF 形状和标签，所以沿 io 把数据交换工具包公开传出去。TKOpenGl 不在这条链上。
steplens_add_module(model
    DocumentModel.cpp
    DocumentModel.h
    DocumentRelease.cpp
    DocumentRelease.h)
target_link_libraries(steplens_model PUBLIC steplens::io)
