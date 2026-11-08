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
# 仓库布局：bench/../src/occtsupport。
# kit 布局与仓库相同：解压后的 <kit>/bench 与 <kit>/src/occtsupport。
# 清单的目录联接（mklink /J）先 REALPATH，避免 ../src 从联接路径跳出 kit 或仓库。
# 扁平兜底：头文件直接放在本目录的 occtsupport/ 下时也能找到。
get_filename_component(_steplens_bench_dir "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
set(_steplens_occt_header "${_steplens_bench_dir}/../src/occtsupport/occt_compat.h")
if(NOT EXISTS "${_steplens_occt_header}")
  set(_steplens_occt_header "${_steplens_bench_dir}/occtsupport/occt_compat.h")
endif()
if(NOT EXISTS "${_steplens_occt_header}")
  message(FATAL_ERROR
    "找不到 occtsupport/occt_compat.h。"
    "仓库和 kit 都应在 bench 上一级的 src/occtsupport/；"
    "扁平 kit 则放在 bench/occtsupport/。")
endif()
get_filename_component(_steplens_occt_dir "${_steplens_occt_header}" DIRECTORY)
get_filename_component(STEPLENS_OCCTSUPPORT_INCLUDE "${_steplens_occt_dir}" DIRECTORY)
message(STATUS "occtsupport: ${_steplens_occt_header}")

find_package(OpenCASCADE REQUIRED)
set(STEPLENS_EXPECTED_OCCT "" CACHE STRING "期望的 OCCT 版本。产品默认 7.9.3；8.0.1 只作对照。用于 test_compat 防止配错 vcpkg 清单")

add_executable(spike src/spike.cpp)
target_include_directories(spike PRIVATE ${OpenCASCADE_INCLUDE_DIR} "${STEPLENS_OCCTSUPPORT_INCLUDE}")
target_link_libraries(spike PRIVATE TKDESTEP TKDEIGES TKXCAF TKLCAF TKCAF TKMesh TKBRep TKTopAlgo TKG3d TKMath TKernel TKXSBase TKBin TKBinXCAF TKDE)

# cl.exe 的 /D 会在小数点处截断，8.0.1 会变成 8。版本串写进生成的头。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/expected_occt.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/expected_occt.h"
    @ONLY)
add_executable(test_compat compat/test_compat.cpp)
target_include_directories(test_compat PRIVATE ${OpenCASCADE_INCLUDE_DIR} "${STEPLENS_OCCTSUPPORT_INCLUDE}"
                                               "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(test_compat PRIVATE TKLCAF TKCDF TKernel)

if(MSVC)
  # 源码含中文注释（UTF-8 无 BOM）；RelWithDebInfo 自带 /Zi + /DEBUG，生成 PDB 供崩溃栈符号化。
  # /EHsc 已经在 CMake 的默认 CXX 标志里，这里再写一次会让 cl 报异常模型被覆盖。
  target_compile_options(spike PRIVATE /utf-8)
  target_compile_options(test_compat PRIVATE /utf-8)
endif()
