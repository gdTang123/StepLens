# pipeline：LoadPipeline。读取、建模型、三角化在后台，SceneHost::pump 只由 GUI 线程的 tick 调用。
# 不链 Qt，无头契约测试可以编同一份源码。SceneBuilder 的接入在 viewer，不在这个库里。
steplens_add_module(pipeline
    SceneHost.h
    LoadPipeline.h
    LoadPipeline.cpp)
target_link_libraries(steplens_pipeline PUBLIC steplens::mesh)
