# 编译选项、警告级别、/utf-8、ASan。只针对本仓库代码打开 /WX。
# STEPLENS_CORE_ONLY：不找 Qt/OCCT，用 GCC/Clang 编 core 和单元测试（含 TSan）。完整程序仍只支持 MSVC。
# STEPLENS_OCCT_CONTRACT：不找 Qt，只编 OCCT 内核契约单测（Linux + conda-forge，无头，可开 ASan）。
# 异常模型不放进全局编译选项。CMake 默认的 CMAKE_CXX_FLAGS 自带 /EHsc，这里先剥掉，再由每个目标加一次。
# 同一条命令里同时出现 /EHsc 和 /EHa 时，cl 会报命令行警告（覆盖异常模型）。/EHa 只给 occtboundary。

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
    foreach(flag_var IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                             CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${flag_var})
            string(REPLACE "/EHsc" "" ${flag_var} "${${flag_var}}")
            string(REGEX REPLACE " +" " " ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
    # /MP 与 Ninja 会叠加并行度。架构 3.5 仍要求带上这个开关，预设因此保留它。
    add_compile_options(/MP)
    if(STEPLENS_ASAN)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    endif()
elseif(NOT STEPLENS_CORE_ONLY AND NOT STEPLENS_OCCT_CONTRACT)
    message(FATAL_ERROR
        "StepLens 只支持 MSVC 2022（v143）x64。当前编译器是 ${CMAKE_CXX_COMPILER_ID}。请使用 CMake 预设 msvc（Ninja Multi-Config + x64 Native Tools）。只跑 core 单测时加 -DSTEPLENS_CORE_ONLY=ON。无头 OCCT 契约单测加 -DSTEPLENS_OCCT_CONTRACT=ON。")
endif()

# Qt6::Platform 会给链接了 Qt 的目标注入 -utf-8、-permissive-、-Zc:__cplusplus。
# 这些目标再加一遍同样的开关，会和上面的全局选项一样互相覆盖。
function(steplens_target_uses_qt target out_var)
    set(uses OFF)
    if(TARGET ${target})
        get_target_property(libs ${target} LINK_LIBRARIES)
        if(libs)
            foreach(item IN LISTS libs)
                if(item MATCHES "Qt6::")
                    set(uses ON)
                endif()
            endforeach()
        endif()
    endif()
    set(${out_var} ${uses} PARENT_SCOPE)
endfunction()

function(steplens_apply_language_flags target)
    if(NOT MSVC)
        return()
    endif()
    steplens_target_uses_qt(${target} uses_qt)
    if(uses_qt)
        return()
    endif()
    target_compile_options(${target} PRIVATE /permissive- /Zc:__cplusplus /utf-8)
endfunction()

# asynchronous 为真时只加 /EHa，否则只加 /EHsc。两个不要同时加。
function(steplens_apply_exceptions target asynchronous)
    if(NOT MSVC)
        return()
    endif()
    if(asynchronous)
        target_compile_options(${target} PRIVATE /EHa)
    else()
        target_compile_options(${target} PRIVATE /EHsc)
    endif()
endfunction()

# 尖括号头（Qt、OCCT、标准库）视为外部头，不把第三方警告变成错误。
# 链接 Qt 的目标要在 target_link_libraries 之后调用，语言开关才能避开 Qt 已经注入的那一组。
function(steplens_apply_warnings target)
    steplens_apply_language_flags(${target})
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
# ASYNCHRONOUS_EXCEPTIONS：这个库的全部翻译单元用 /EHa。目前只给 occtboundary。
function(steplens_add_module module_name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "ASYNCHRONOUS_EXCEPTIONS" "" "")
    add_library(steplens_${module_name} STATIC ${ARG_UNPARSED_ARGUMENTS})
    add_library(steplens::${module_name} ALIAS steplens_${module_name})
    target_include_directories(steplens_${module_name} PUBLIC "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(steplens_${module_name} PUBLIC cxx_std_20)
    steplens_apply_warnings(steplens_${module_name})
    if(ARG_ASYNCHRONOUS_EXCEPTIONS)
        steplens_apply_exceptions(steplens_${module_name} ON)
    else()
        steplens_apply_exceptions(steplens_${module_name} OFF)
    endif()
    set_target_properties(steplens_${module_name} PROPERTIES FOLDER "modules")
endfunction()

function(steplens_collect_project_targets out_var)
    set(pending "${CMAKE_SOURCE_DIR}")
    set(targets "")
    while(pending)
        list(POP_FRONT pending dir)
        get_property(dir_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        list(APPEND targets ${dir_targets})
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirs})
    endwhile()
    set(${out_var} "${targets}" PARENT_SCOPE)
endfunction()

# 每个本仓库目标的编译选项里只能有一种异常模型，而且只能出现一次。
function(steplens_audit_exception_model)
    if(NOT MSVC)
        return()
    endif()
    foreach(flag_var IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                             CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if("${${flag_var}}" MATCHES "/EH")
            message(FATAL_ERROR "${flag_var} 仍带异常模型开关: ${${flag_var}}")
        endif()
    endforeach()

    steplens_collect_project_targets(targets)
    foreach(target IN LISTS targets)
        if(NOT target MATCHES "^(steplens_.+|StepLens)$")
            continue()
        endif()
        get_target_property(target_type ${target} TYPE)
        if(target_type STREQUAL "INTERFACE_LIBRARY" OR target_type STREQUAL "UTILITY")
            continue()
        endif()
        get_target_property(opts ${target} COMPILE_OPTIONS)
        set(eh_sc 0)
        set(eh_a 0)
        if(opts)
            foreach(opt IN LISTS opts)
                if(opt STREQUAL "/EHsc")
                    math(EXPR eh_sc "${eh_sc} + 1")
                elseif(opt STREQUAL "/EHa")
                    math(EXPR eh_a "${eh_a} + 1")
                endif()
            endforeach()
        endif()
        if(target STREQUAL "steplens_occtboundary")
            if(NOT eh_a EQUAL 1 OR NOT eh_sc EQUAL 0)
                message(FATAL_ERROR "steplens_occtboundary 必须只有一次 /EHa，实际 /EHa=${eh_a} /EHsc=${eh_sc}")
            endif()
        elseif(NOT eh_sc EQUAL 1 OR NOT eh_a EQUAL 0)
            message(FATAL_ERROR "${target} 必须只有一次 /EHsc，实际 /EHsc=${eh_sc} /EHa=${eh_a}")
        endif()
    endforeach()
endfunction()
