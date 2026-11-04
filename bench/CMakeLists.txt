cmake_minimum_required(VERSION 3.20)
project(steplens_spike CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  # vcpkg 的 OCCT 只有 Debug/Release。RelWithDebInfo 链接 Release。
  set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
  set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)
endif()
find_package(OpenCASCADE REQUIRED)
set(STEPLENS_EXPECTED_OCCT "" CACHE STRING "期望的 OCCT 版本（如 8.0.1 / 7.9.3），用于 test_compat 防止配错 vcpkg 清单")

add_executable(spike src/spike.cpp)
target_include_directories(spike PRIVATE ${OpenCASCADE_INCLUDE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(spike PRIVATE TKDESTEP TKDEIGES TKXCAF TKLCAF TKCAF TKMesh TKBRep TKTopAlgo TKG3d TKMath TKernel TKXSBase TKBin TKBinXCAF TKDE)

# cl.exe 的 /D 会在小数点处截断，8.0.1 会变成 8。版本串写进生成的头。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/expected_occt.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/expected_occt.h"
    @ONLY)
add_executable(test_compat compat/test_compat.cpp)
target_include_directories(test_compat PRIVATE ${OpenCASCADE_INCLUDE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/../src"
                                               "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_compat PRIVATE TKLCAF TKCDF TKernel)

if(MSVC)
  # 源码含中文注释（UTF-8 无 BOM）；RelWithDebInfo 自带 /Zi + /DEBUG，生成 PDB 供崩溃栈符号化
  target_compile_options(spike PRIVATE /utf-8 /EHsc)
  target_compile_options(test_compat PRIVATE /utf-8 /EHsc)
endif()
