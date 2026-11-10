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
        model/DocumentModelTest.cpp)
    # io 把数据交换工具包拉进测试进程，用来确认这条链上没有 TKOpenGl。
    # model 沿 io 公开传递，不另外链 vis。契约单测用到 StdPrs（TKV3d）。只链 TKV3d，不链 vis。
    target_link_libraries(steplens_tests PRIVATE steplens::occtsupport steplens::model TKV3d)
    target_compile_definitions(steplens_tests PRIVATE "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
endif()
steplens_apply_warnings(steplens_tests)
steplens_apply_exceptions(steplens_tests OFF)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
