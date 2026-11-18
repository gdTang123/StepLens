# 无头契约测试单独成可执行文件，带 occt-contract 标签。完整 MSVC 构建仍把同一份源码编进 steplens_tests。

# 非 MSVC 宿主：给 OcctBoundary.h 的 _WIN32 声明做全局 struct 标签兼容编译。
# 不是 <eh.h> / MSVC ABI 证明，见 SehTranslatorGlobalTagCheck.cpp 文件头的受控边界。
if(NOT MSVC AND TARGET steplens_occtboundary)
    add_library(steplens_seh_translator_global_tag_check OBJECT occt/SehTranslatorGlobalTagCheck.cpp)
    target_include_directories(steplens_seh_translator_global_tag_check PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(steplens_seh_translator_global_tag_check PRIVATE _WIN32)
    target_compile_features(steplens_seh_translator_global_tag_check PRIVATE cxx_std_20)
    steplens_apply_warnings(steplens_seh_translator_global_tag_check)
    set_target_properties(steplens_seh_translator_global_tag_check PROPERTIES FOLDER "tests")
endif()

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

# 最小快退子进程与合同/产品测试共用模型库。CORE_ONLY 没有 steplens::model，不能无条件加入。
if(NOT STEPLENS_CORE_ONLY)
    add_executable(steplens_release_quick_exit model/ReleaseQuickExitMain.cpp)
    target_include_directories(steplens_release_quick_exit PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_release_quick_exit PRIVATE steplens::model)
    steplens_apply_warnings(steplens_release_quick_exit)
    steplens_apply_exceptions(steplens_release_quick_exit OFF)
    set_target_properties(steplens_release_quick_exit PROPERTIES FOLDER "tests")
endif()

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
    add_executable(steplens_model_tests model/DocumentModelTest.cpp model/DocumentReleaseTest.cpp)
    add_dependencies(steplens_model_tests steplens_release_quick_exit)
    target_include_directories(steplens_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_model_tests PRIVATE steplens::model GTest::gtest_main)
    target_compile_definitions(steplens_model_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
    target_compile_definitions(steplens_model_tests PRIVATE
        "STEPLENS_RELEASE_QUICK_EXIT_EXE=\"$<TARGET_FILE:steplens_release_quick_exit>\"")
    steplens_apply_warnings(steplens_model_tests)
    steplens_apply_exceptions(steplens_model_tests OFF)
    set_target_properties(steplens_model_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(steplens_model_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")

    # T07 单级网格。和 model 同一个无头作业。OCCT 静态分配仍关掉泄漏检查。
    add_executable(steplens_mesh_tests
        mesh/TessellatorTest.cpp
        mesh/StepHeapReproTest.cpp
        mesh/MemoryReleaseTest.cpp)
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

    # T11 取消后再打开。不链 TKOpenGl，场景用假的 SceneHost。
    add_executable(steplens_pipeline_tests pipeline/LoadPipelineTest.cpp)
    target_include_directories(steplens_pipeline_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_pipeline_tests PRIVATE steplens::pipeline GTest::gtest_main)
    target_compile_definitions(steplens_pipeline_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\""
        "STEPLENS_GIT_SHA=\"${STEPLENS_GIT_SHA}\""
        "STEPLENS_BUILD_IDENTITY_FILE=\"${CMAKE_BINARY_DIR}/steplens-build-identity.txt\"")
    steplens_apply_warnings(steplens_pipeline_tests)
    steplens_apply_exceptions(steplens_pipeline_tests OFF)
    set_target_properties(steplens_pipeline_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(steplens_pipeline_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 120
            LABELS "unit;occt-contract"
            ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:detect_leaks=0")

    # QApplication 首入验证。Windows MSVC x64 上核完整 MXCSR/SEH；本机无 MSVC 时由目标 CI 跑。
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(TARGET Qt6::Widgets)
        add_executable(steplens_gui_first_entry_tests model/DocumentReleaseGuiFirstEntryTest.cpp)
        target_include_directories(steplens_gui_first_entry_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
        target_link_libraries(steplens_gui_first_entry_tests PRIVATE
            steplens::model
            Qt6::Widgets
            GTest::gtest_main)
        steplens_apply_warnings(steplens_gui_first_entry_tests)
        steplens_apply_exceptions(steplens_gui_first_entry_tests OFF)
        set_target_properties(steplens_gui_first_entry_tests PROPERTIES FOLDER "tests")
        gtest_discover_tests(steplens_gui_first_entry_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES
                LABELS "unit;gui-first-entry"
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    return()
endif()

add_executable(steplens_tests
    core/VersionTest.cpp
    core/ResultTest.cpp
    core/CancelTest.cpp
    core/LoadCoordinatorTest.cpp
    viewer/GlCleanupTest.cpp
    viewer/ClickGestureTest.cpp
    viewer/SceneHostBindingTest.cpp
    core/ProgressSinkTest.cpp
    core/ProgressSinkRaceTest.cpp
    core/TaskSchedulerTest.cpp
    core/BatchCallbackRaceTest.cpp
    core/LogTest.cpp
    core/CrashDumpTest.cpp)
target_sources(steplens_tests PRIVATE core/MemoryMonitorTest.cpp)
target_link_libraries(steplens_tests PRIVATE steplens::core GTest::gtest_main)
target_include_directories(steplens_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(NOT STEPLENS_CORE_ONLY)
    add_executable(steplens_ui_tests
        ui/AssemblyTreeModelTest.cpp
        ui/AssemblyTreeReleaseTest.cpp
        model/DocumentReleaseGuiFirstEntryTest.cpp)
    target_include_directories(steplens_ui_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_ui_tests PRIVATE steplens::ui steplens::model Qt6::Widgets Qt6::Test GTest::gtest_main)
    steplens_apply_warnings(steplens_ui_tests)
    steplens_apply_exceptions(steplens_ui_tests OFF)
    set_target_properties(steplens_ui_tests PROPERTIES FOLDER "tests")

    target_sources(steplens_tests PRIVATE
        app/SettingsTest.cpp
        ${CMAKE_SOURCE_DIR}/src/app/settings/Settings.cpp
        occt/OcctCompatTest.cpp
        occt/OcctKernelContractTest.cpp
        occt/OcctBoundaryTest.cpp
        io/CadLoaderTest.cpp
        model/DocumentModelTest.cpp
        model/DocumentReleaseTest.cpp
        mesh/TessellatorTest.cpp
        mesh/StepHeapReproTest.cpp
        mesh/MemoryReleaseTest.cpp)
    # io 把数据交换工具包拉进测试进程，用来确认这条链上没有 TKOpenGl。
    # mesh 公开链 TKV3d（FillTriangles），不链 vis，所以 TKOpenGl 仍然进不来。
    target_link_libraries(steplens_tests PRIVATE steplens::occtsupport steplens::mesh)
    target_link_libraries(steplens_tests PRIVATE Qt6::Core)
    add_dependencies(steplens_tests steplens_release_quick_exit)
    target_compile_definitions(steplens_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\""
        "STEPLENS_RELEASE_QUICK_EXIT_EXE=\"$<TARGET_FILE:steplens_release_quick_exit>\"")

    # StatusNotice uses Qt Widgets, so keep it outside steplens_tests, which links only Qt Core.
    add_executable(steplens_status_notice_tests
        ../harness/t14-evidence/StatusNoticeTest.cpp
        ${CMAKE_SOURCE_DIR}/src/app/StatusNotice.cpp)
    target_include_directories(steplens_status_notice_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_status_notice_tests PRIVATE
        Qt6::Widgets
        Qt6::Test
        GTest::gtest_main)
    steplens_apply_warnings(steplens_status_notice_tests)
    steplens_apply_exceptions(steplens_status_notice_tests OFF)
    set_target_properties(steplens_status_notice_tests PROPERTIES FOLDER "tests")

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

    add_executable(steplens_pipeline_tests pipeline/LoadPipelineTest.cpp)
    target_include_directories(steplens_pipeline_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(steplens_pipeline_tests PRIVATE steplens::pipeline GTest::gtest_main)
    target_compile_definitions(steplens_pipeline_tests PRIVATE
        "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\""
        "STEPLENS_GIT_SHA=\"${STEPLENS_GIT_SHA}\""
        "STEPLENS_BUILD_IDENTITY_FILE=\"${CMAKE_BINARY_DIR}/steplens-build-identity.txt\"")
    if(TARGET StepLens)
        file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_exit/$<CONFIG>/steplens_app_exe.h"
            CONTENT "#pragma once
inline constexpr char kStepLensExe[] = R\"exe($<TARGET_FILE:StepLens>)exe\";
")
        target_include_directories(steplens_pipeline_tests PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/app_exit/$<CONFIG>")
        target_compile_definitions(steplens_pipeline_tests PRIVATE STEPLENS_HAS_APP=1)
    endif()
    steplens_apply_warnings(steplens_pipeline_tests)
    steplens_apply_exceptions(steplens_pipeline_tests OFF)
    set_target_properties(steplens_pipeline_tests PROPERTIES FOLDER "tests")
endif()
steplens_apply_warnings(steplens_tests)
steplens_apply_exceptions(steplens_tests OFF)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
if(NOT STEPLENS_CORE_ONLY)
    gtest_discover_tests(steplens_ui_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "unit;ui")
    # include(GoogleTest) 在上面。提前调用 gtest_discover_tests 时 MSVC 配置会报未知命令。
    gtest_discover_tests(steplens_status_notice_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS unit
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    gtest_discover_tests(steplens_scene_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS unit)
    gtest_discover_tests(steplens_bench_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 180
            LABELS unit
            ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
    gtest_discover_tests(steplens_pipeline_tests
        DISCOVERY_MODE PRE_TEST
        TEST_FILTER "-AppExit.*"
        PROPERTIES
            TIMEOUT 120
            LABELS unit)
    # AppExit 不进默认 unit 预设，避免无环境时 SKIP 被当成成功。由产品冒烟步骤显式拉起。
    gtest_discover_tests(steplens_pipeline_tests
        DISCOVERY_MODE PRE_TEST
        TEST_FILTER "AppExit.*"
        PROPERTIES
            TIMEOUT 90
            LABELS app_exit)
endif()
