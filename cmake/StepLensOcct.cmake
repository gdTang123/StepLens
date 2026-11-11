# OCCT 按用途拆成三个 INTERFACE 库。产品默认 7.9.3；8.0.1 只作对照。
# kernel：建模与网格。io：STEP/IGES/XCAF。vis：显示，含 TKOpenGl。
# TKOpenGl 只放在 vis。viewer 用 PRIVATE 链接 vis，不传给其它目标。
# 配置结束时 steplens_audit_occt_link() 写 ${CMAKE_BINARY_DIR}/occt-link-report.txt。
# Windows 上再用 scripts/verify_occt_link.ps1 对 exe/dll 跑 dumpbin /dependents。
# TKXCAF 自己的依赖里有 TKV3d / TKVCAF，没有 TKOpenGl。io 因此可能间接链上 TKV3d，但不得出现 TKOpenGl。

set(STEPLENS_OCCT_KERNEL_TOOLKITS
    TKernel
    TKMath
    TKG2d
    TKG3d
    TKGeomBase
    TKBRep
    TKGeomAlgo
    TKTopAlgo
    TKPrim
    TKMesh
    TKShHealing
    TKBO
    TKBool
)

set(STEPLENS_OCCT_IO_TOOLKITS
    TKDE
    TKXSBase
    TKDESTEP
    TKDEIGES
    TKCAF
    TKLCAF
    TKCDF
    TKXCAF
)

set(STEPLENS_OCCT_VIS_TOOLKITS
    TKService
    TKV3d
    TKVCAF
    TKOpenGl
)

set(STEPLENS_OCCT_TOOLKITS
    ${STEPLENS_OCCT_KERNEL_TOOLKITS}
    ${STEPLENS_OCCT_IO_TOOLKITS}
    ${STEPLENS_OCCT_VIS_TOOLKITS}
)

function(steplens_resolve_toolkit toolkit out_var)
    if(TARGET ${toolkit})
        set(${out_var} ${toolkit} PARENT_SCOPE)
        return()
    endif()
    foreach(lib IN LISTS OpenCASCADE_LIBRARIES)
        if(lib MATCHES "(^|[/\\\\])${toolkit}\\.")
            set(${out_var} "${lib}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(steplens_collect_toolkits out_var)
    set(resolved "")
    foreach(toolkit IN LISTS ARGN)
        steplens_resolve_toolkit(${toolkit} ref)
        if(NOT ref STREQUAL "")
            list(APPEND resolved ${ref})
        endif()
    endforeach()
    set(${out_var} "${resolved}" PARENT_SCOPE)
endfunction()

function(steplens_make_occt_interface target_suffix)
    set(refs ${ARGN})
    add_library(steplens_occt_${target_suffix} INTERFACE)
    add_library(steplens::occt_${target_suffix} ALIAS steplens_occt_${target_suffix})
    target_link_libraries(steplens_occt_${target_suffix} INTERFACE ${refs})
endfunction()

function(steplens_define_occt_interfaces)
    if(NOT OpenCASCADE_INCLUDE_DIR)
        message(FATAL_ERROR "find_package(OpenCASCADE) 没有导出包含目录")
    endif()
    steplens_collect_toolkits(kernel_refs ${STEPLENS_OCCT_KERNEL_TOOLKITS})
    steplens_collect_toolkits(io_refs ${STEPLENS_OCCT_IO_TOOLKITS})
    steplens_collect_toolkits(vis_refs ${STEPLENS_OCCT_VIS_TOOLKITS})
    if(kernel_refs STREQUAL "")
        message(FATAL_ERROR "OCCT kernel 工具包一个都没有解析到")
    endif()

    add_library(steplens_occt_headers INTERFACE)
    add_library(steplens::occt_headers ALIAS steplens_occt_headers)
    target_include_directories(steplens_occt_headers INTERFACE "${OpenCASCADE_INCLUDE_DIR}")

    steplens_make_occt_interface(kernel steplens::occt_headers ${kernel_refs})
    steplens_make_occt_interface(io steplens::occt_kernel ${io_refs})
    steplens_make_occt_interface(vis steplens::occt_kernel ${vis_refs})
endfunction()

function(steplens_check_occt_toolkits)
    if(NOT OpenCASCADE_LIBRARIES AND NOT TARGET TKernel)
        message(FATAL_ERROR "OpenCASCADE_LIBRARIES 为空")
    endif()
    set(missing "")
    foreach(toolkit IN LISTS STEPLENS_OCCT_TOOLKITS)
        steplens_resolve_toolkit(${toolkit} ref)
        if(ref STREQUAL "")
            list(APPEND missing ${toolkit})
        endif()
    endforeach()
    if(missing)
        message(WARNING
            "这些 OCCT 工具包没有出现在 OpenCASCADE 的导出里: ${missing}。请对照 vcpkg_installed/x64-windows/lib 里的实际文件名。TKDEIGES 的安装名在架构 3.3 里标了需确认；源码树里的目录名是 TKDEIGES。")
    endif()
endfunction()

function(steplens_peel_link_item item out_var)
    set(value "${item}")
    foreach(unused RANGE 0 4)
        if(value MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(value "${CMAKE_MATCH_1}")
        elseif(value MATCHES "^\\$<COMPILE_ONLY:(.+)>$")
            set(value "${CMAKE_MATCH_1}")
        else()
            break()
        endif()
    endforeach()
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(steplens_link_mentions_tkopengl item out_var)
    if(item MATCHES "(^|[^A-Za-z0-9_])TKOpenGl([^A-Za-z0-9_]|$)")
        set(${out_var} ON PARENT_SCOPE)
    else()
        set(${out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

# 沿 LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES 走一遍，看闭包里有没有 TKOpenGl。
function(steplens_link_closure_has_tkopengl target out_var)
    set(found OFF)
    set(pending "${target}")
    set(seen "")
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
        if(NOT TARGET "${current}")
            continue()
        endif()
        set(raw_items "")
        foreach(prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(vals "${current}" ${prop})
            if(vals)
                list(APPEND raw_items ${vals})
            endif()
        endforeach()
        foreach(item IN LISTS raw_items)
            if(item STREQUAL "" OR item STREQUAL "debug" OR item STREQUAL "optimized"
                OR item STREQUAL "general")
                continue()
            endif()
            steplens_link_mentions_tkopengl("${item}" mentioned)
            if(mentioned)
                set(found ON)
            endif()
            steplens_peel_link_item("${item}" peeled)
            steplens_link_mentions_tkopengl("${peeled}" peeled_mentioned)
            if(peeled_mentioned)
                set(found ON)
            endif()
            if(TARGET "${peeled}")
                list(APPEND pending "${peeled}")
            endif()
        endforeach()
    endwhile()
    set(${out_var} ${found} PARENT_SCOPE)
endfunction()

function(steplens_audit_occt_link)
    steplens_collect_project_targets(targets)
    set(report_path "${CMAKE_BINARY_DIR}/occt-link-report.txt")
    # scene 库本身不链 TKOpenGl。场景测试和 steplens-bench 要建视图，单独允许这两个可执行文件。
    set(allowed steplens_occt_vis steplens_viewer StepLens steplens_scene_tests steplens_bench)
    file(WRITE "${report_path}" "# TKOpenGl 链接闭包。yes 只允许出现在 vis、viewer、app。\n")
    set(viewer_has OFF)
    set(vis_has OFF)
    foreach(target IN LISTS targets)
        if(NOT target MATCHES "^(steplens_.+|StepLens)$")
            continue()
        endif()
        steplens_link_closure_has_tkopengl(${target} has_gl)
        if(has_gl)
            set(flag yes)
        else()
            set(flag no)
        endif()
        file(APPEND "${report_path}" "${target} tkopengl=${flag}\n")
        if(target STREQUAL "steplens_viewer" AND has_gl)
            set(viewer_has ON)
        endif()
        if(target STREQUAL "steplens_occt_vis" AND has_gl)
            set(vis_has ON)
        endif()
        if(has_gl)
            list(FIND allowed ${target} allowed_index)
            if(allowed_index EQUAL -1)
                message(FATAL_ERROR
                    "${target} 的链接闭包含有 TKOpenGl。TKOpenGl 只能 PRIVATE 链到 viewer（以及不得已时的 app）。报告: ${report_path}")
            endif()
        endif()
    endforeach()
    if(NOT viewer_has OR NOT vis_has)
        message(FATAL_ERROR "steplens_viewer / steplens_occt_vis 必须链上 TKOpenGl。报告: ${report_path}")
    endif()
    message(STATUS "OCCT 链接报告: ${report_path}")
endfunction()
