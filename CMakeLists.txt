cmake_minimum_required(VERSION 3.25)

project(StepLens
    VERSION 0.1.0
    DESCRIPTION "Internal lightweight STEP/IGES viewer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 架构 11.2：暂不使用 C++20 模块。3.25 会忽略这个变量，3.28 起生效。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# vcpkg 和官方 Qt 包只有 Debug/Release。发布构建用 RelWithDebInfo，链接它们的 Release 库。
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)

include(StepLensOptions)

# 预设 msvc / ci 默认 7.9.3，清单在 vcpkg-configs/occt-7.9.3。对照预设 msvc-occt801 覆盖成 8.0.1。
# 版本单测拿它和 OCC_VERSION_COMPLETE 比对。
set(STEPLENS_EXPECTED_OCCT "7.9.3" CACHE STRING "本构建必须对应的 OCCT 版本")

if(STEPLENS_CORE_ONLY)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
elseif(STEPLENS_OCCT_CONTRACT)
    # 无头契约测试：conda-forge 的 OpenCASCADE + GTest，不配置 Qt，不编查看器。
    # 同时编 io，让 7.9.3（默认）和 8.0.1（对照）都能编译 STEP/IGES 加载器。
    include(StepLensOcct)
    find_package(OpenCASCADE CONFIG REQUIRED)
    find_package(GTest CONFIG REQUIRED)
    steplens_define_occt_interfaces()
else()
    include(StepLensOcct)

    # LinguistTools 在 qttools；其余模块都在官方 qtbase 预编译包里。
    find_package(Qt6 6.12 REQUIRED COMPONENTS
        Core
        Gui
        Widgets
        OpenGL
        OpenGLWidgets
        Concurrent
        Test
        LinguistTools)
    qt_standard_project_setup()

    find_package(OpenCASCADE CONFIG REQUIRED)
    find_package(GTest CONFIG REQUIRED)
    steplens_check_occt_toolkits()
    steplens_define_occt_interfaces()
endif()

message(STATUS "StepLens ${PROJECT_VERSION}")
if(STEPLENS_OCCT_CONTRACT)
    message(STATUS
        "OpenCASCADE ${OpenCASCADE_MAJOR_VERSION}.${OpenCASCADE_MINOR_VERSION}.${OpenCASCADE_MAINTENANCE_VERSION} (contract)")
elseif(NOT STEPLENS_CORE_ONLY)
    message(STATUS "Qt ${Qt6_VERSION}")
    if(DEFINED OpenCASCADE_VERSION)
        message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}")
    endif()
endif()

include(CTest)

if(STEPLENS_CORE_ONLY)
    add_subdirectory(src/core)
elseif(STEPLENS_OCCT_CONTRACT)
    add_subdirectory(src/core)
    add_subdirectory(src/occtsupport)
    add_subdirectory(src/io)
else()
    add_subdirectory(src)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NOT STEPLENS_CORE_ONLY AND NOT STEPLENS_OCCT_CONTRACT)
    include(StepLensDeploy)
    steplens_audit_occt_link()
endif()
if(MSVC)
    steplens_audit_exception_model()
endif()
