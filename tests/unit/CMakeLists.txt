# 无头契约测试单独成可执行文件，带 occt-contract 标签。完整 MSVC 构建仍把同一份源码编进 steplens_tests。

# steplens-bench 的 CLI 合同。可执行文件在 tools/bench，这里只跑它。
# gtest_discover_tests 由调用方在 include(GoogleTest) 之后执行。
function(steplens_add_bench_cli_test)
    if(NOT TARGET steplens_bench)
        message(FATAL_ERROR "steplens_bench 还没有加入构建")
    endif()
    add_executable(steplens_bench_tests bench/BenchCliTest.cpp)
    target_include_directories(steplens_bench_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/bench_cli/$<CONFIG>")
    target_link_libraries(steplens_bench_tests PRIVATE steplens::io GTest::gtest_main)
    target_compile_definitions(steplens_bench_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_bench_tests)
    steplens_apply_exceptions(steplens_bench_tests OFF)
    add_dependencies(steplens_bench_tests steplens_bench)
    set_target_properties(steplens_bench_tests PROPERTIES FOLDER "tests")
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bench_cli/$<CONFIG>/steplens_bench_exe.h"
        CONTENT "#pragma once
inline constexpr char kSteplensBenchExe[] = R\"bench($<TARGET_FILE:steplens_bench>)bench\";
inline constexpr char kSteplensSourceDir[] = R\"bench(${CMAKE_SOURCE_DIR})bench\";
")
endfunction()

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

    # T05 加载器。和契约单测同一个无头作业，7.9.3 是默认，8.0.1 只是对照。
    add_executable(steplens_io_tests io/CadLoaderTest.cpp)
    target_include_directories(steplens_io_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_io_tests PRIVATE steplens::io GTest::gtest_main)
    target_compile_definitions(steplens_io_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_io_tests)
    steplens_apply_exceptions(steplens_io_tests OFF)
    set_target_properties(steplens_io_tests PROPERTIES FOLDER "tests")
    # OCCT 的 TPrsStd_DriverTable / TCollection 静态分配会被 LeakSanitizer 记成泄漏。
    # 那不是这个加载器的分配。这里关掉泄漏检查，越界和释放后使用仍然开着。
    gtest_discover_tests(steplens_io_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")

    # T06 DocumentModel。和加载器同一个无头作业。OCCT 静态分配仍关掉泄漏检查。
    add_executable(steplens_model_tests model/DocumentModelTest.cpp)
    target_include_directories(steplens_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_model_tests PRIVATE steplens::model GTest::gtest_main)
    target_compile_definitions(steplens_model_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_model_tests)
    steplens_apply_exceptions(steplens_model_tests OFF)
    set_target_properties(steplens_model_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(steplens_model_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")

    # T07 单级网格。和 model 同一个无头作业。OCCT 静态分配仍关掉泄漏检查。
    add_executable(steplens_mesh_tests mesh/TessellatorTest.cpp)
    target_include_directories(steplens_mesh_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_mesh_tests PRIVATE steplens::mesh GTest::gtest_main)
    target_compile_definitions(steplens_mesh_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_mesh_tests)
    steplens_apply_exceptions(steplens_mesh_tests OFF)
    set_target_properties(steplens_mesh_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(steplens_mesh_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")

    # T08 场景。和 mesh 同一个无头作业。要链 TKOpenGl 才能建视图，所以不放进 steplens_tests
    # （那份可执行文件用来确认 io/mesh 没有把 TKOpenGl 带进来）。
    add_executable(steplens_scene_tests scene/SceneBuilderTest.cpp)
    target_include_directories(steplens_scene_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_scene_tests PRIVATE steplens::scene steplens::occt_vis GTest::gtest_main)
    target_compile_definitions(steplens_scene_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_scene_tests)
    steplens_apply_exceptions(steplens_scene_tests OFF)
    set_target_properties(steplens_scene_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(steplens_scene_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0;LIBGL_ALWAYS_SOFTWARE=1")

    steplens_add_bench_cli_test()
    gtest_discover_tests(steplens_bench_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 180
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0;LIBGL_ALWAYS_SOFTWARE=1")
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
        occt/OcctBoundaryTest.cpp
        io/CadLoaderTest.cpp
        model/DocumentModelTest.cpp
        mesh/TessellatorTest.cpp)
    # io 把数据交换工具包拉进测试进程，用来确认这条链上没有 TKOpenGl。
    # mesh 公开链 TKV3d（FillTriangles），不链 vis，所以 TKOpenGl 仍然进不来。
    target_link_libraries(steplens_tests PRIVATE steplens::occtsupport steplens::mesh)
    target_compile_definitions(steplens_tests PRIVATE "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")

    # 场景测试单独成可执行文件：它要链 TKOpenGl，不能并进上面这份。
    add_executable(steplens_scene_tests scene/SceneBuilderTest.cpp)
    target_include_directories(steplens_scene_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_scene_tests PRIVATE
        steplens::scene
        steplens::occt_vis
        GTest::gtest_main)
    target_compile_definitions(steplens_scene_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    steplens_apply_warnings(steplens_scene_tests)
    steplens_apply_exceptions(steplens_scene_tests OFF)
    set_target_properties(steplens_scene_tests PROPERTIES FOLDER "tests")

    steplens_add_bench_cli_test()
endif()
steplens_apply_warnings(steplens_tests)
steplens_apply_exceptions(steplens_tests OFF)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
if(NOT STEPLENS_CORE_ONLY)
    # include(GoogleTest) 在上面。提前调用 gtest_discover_tests 时 MSVC 配置会报未知命令。
    gtest_discover_tests(steplens_scene_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS unit)
    gtest_discover_tests(steplens_bench_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 180
            LABELS unit
            ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
endif()
