# steplens-bench：T03 命令行基准。不链 Qt，不出界面。
# 出图阶段要建 AIS 视图，所以这个可执行文件单独链 TKOpenGl，和场景测试一样。
add_executable(steplens_bench
    main.cpp
    MemoryProbe.cpp
    BenchRun.cpp)
set_target_properties(steplens_bench PROPERTIES
    OUTPUT_NAME "steplens-bench"
    FOLDER "tools")
target_include_directories(steplens_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(steplens_bench PRIVATE steplens::scene steplens::occt_vis)
steplens_apply_warnings(steplens_bench)
steplens_apply_exceptions(steplens_bench OFF)
