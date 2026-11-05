# 编译选项、警告级别、/utf-8、ASan。只针对本仓库代码打开 /WX。
# STEPLENS_CORE_ONLY：不找 Qt/OCCT，用 GCC/Clang 编 core 和单元测试（含 TSan）。完整程序仍只支持 MSVC。
# STEPLENS_OCCT_CONTRACT：不找 Qt，只编 OCCT 内核契约单测（Linux + conda-forge，无头，可开 ASan）。

option(STEPLENS_CORE_ONLY "Build only core and unit tests, without Qt or OCCT" OFF)
option(STEPLENS_OCCT_CONTRACT "Build headless OCCT kernel contract tests, without Qt" OFF)
option(STEPLENS_ASAN "Build with AddressSanitizer (MSVC)" OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "StepLens 只编 64 位。32 位地址空间装不下超大装配。")
endif()

if(STEPLENS_OCCT_CONTRACT AND STEPLENS_CORE_ONLY)
    message(FATAL_ERROR "STEPLENS_OCCT_CONTRACT 与 STEPLENS_CORE_ONLY 不能同时打开")
endif()

if(MSVC)
    # /MP 与 Ninja 会叠加并行度。架构 3.5 仍要求带上这个开关，预设因此保留它。
    add_compile_options(
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /MP
    )
    if(STEPLENS_ASAN)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    endif()
elseif(NOT STEPLENS_CORE_ONLY AND NOT STEPLENS_OCCT_CONTRACT)
    message(FATAL_ERROR
        "StepLens 只支持 MSVC 2022（v143）x64。当前编译器是 ${CMAKE_CXX_COMPILER_ID}。请使用 CMake 预设 msvc（Ninja Multi-Config + x64 Native Tools）。只跑 core 单测时加 -DSTEPLENS_CORE_ONLY=ON。无头 OCCT 契约单测加 -DSTEPLENS_OCCT_CONTRACT=ON。")
endif()

# 尖括号头（Qt、OCCT、标准库）视为外部头，不把第三方警告变成错误。
function(steplens_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /external:anglebrackets
            /external:W0
        )
    elseif(STEPLENS_CORE_ONLY OR STEPLENS_OCCT_CONTRACT)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# 在子目录里创建一个静态库。sources 相对该子目录。
function(steplens_add_module module_name)
    add_library(steplens_${module_name} STATIC ${ARGN})
    add_library(steplens::${module_name} ALIAS steplens_${module_name})
    target_include_directories(steplens_${module_name} PUBLIC "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(steplens_${module_name} PUBLIC cxx_std_20)
    steplens_apply_warnings(steplens_${module_name})
    set_target_properties(steplens_${module_name} PROPERTIES FOLDER "modules")
endfunction()
