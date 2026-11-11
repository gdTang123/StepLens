# scene：T08 SceneBuilder、PrototypePrs、SelectionMap。
# 只在 GUI 线程碰 AIS。TKOpenGl 不进这个库，OpenGL 驱动留在 viewer。
# 不链 Qt：无头契约测试要编同一份源码。pump() 由 GUI 线程的定时器调用。
steplens_add_module(scene
    PrototypePrs.cpp
    PrototypePrs.h
    SelectionMap.cpp
    SelectionMap.h
    SceneBuilder.cpp
    SceneBuilder.h)
target_link_libraries(steplens_scene PUBLIC steplens::mesh TKV3d)
