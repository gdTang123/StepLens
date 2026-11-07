# 无头契约测试单独成可执行文件，带 occt-contract 标签。完整 MSVC 构建仍把同一份源码编进 steplens_tests。
if(STEPLENS_OCCT_CONTRACT)
    add_executable(steplens_occt_contract occt/OcctKernelContractTest.cpp)
    target_include_directories(steplens_occt_contract PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_occt_contract PRIVATE
        GTest::gtest_main
        TKDEIGES
        TKV3d
        TKMesh
        TKPrim
        TKTopAlgo)
    target_compile_definitions(steplens_occt_contract PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_occt_contract)
    steplens_apply_exceptions(steplens_occt_contract OFF)
    set_target_properties(steplens_occt_contract PROPERTIES FOLDER "tests")

    include(GoogleTest)
    gtest_discover_tests(steplens_occt_contract
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1")
    return()
endif()

add_executable(steplens_tests
    core/VersionTest.cpp
    core/ResultTest.cpp
    core/CancelTest.cpp
    core/LoadCoordinatorTest.cpp
    viewer/GlCleanupTest.cpp
    core/ProgressSinkTest.cpp
    core/ProgressSinkRaceTest.cpp
    core/TaskSchedulerTest.cpp
    core/LogTest.cpp
    core/CrashDumpTest.cpp)
target_link_libraries(steplens_tests PRIVATE steplens::core GTest::gtest_main)
target_include_directories(steplens_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(NOT STEPLENS_CORE_ONLY)
    target_sources(steplens_tests PRIVATE
        occt/OcctCompatTest.cpp
        occt/OcctKernelContractTest.cpp
        occt/OcctBoundaryTest.cpp)
    # io 把数据交换工具包拉进测试进程，用来确认这条链上没有 TKOpenGl。
    # 契约单测用到 StdPrs（TKV3d）。只链 TKV3d，不链 vis，避免把 TKOpenGl 带进测试进程。
    target_link_libraries(steplens_tests PRIVATE steplens::occtsupport steplens::io TKV3d)
    target_compile_definitions(steplens_tests PRIVATE "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
endif()
steplens_apply_warnings(steplens_tests)
steplens_apply_exceptions(steplens_tests OFF)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
