# OCCT 7.9.3 / 8.0.1 通用工具包清单。空窗口程序还不链接这些库。
# 源码树 V8.0.1 的 src/DataExchange 下有 TKDESTEP 和 TKDEIGES。
# vcpkg 安装后的实际 .lib 文件名仍要在第一次成功配置时对照安装目录确认（架构 3.3）。

set(STEPLENS_OCCT_TOOLKITS
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
    TKService
    TKV3d
    TKOpenGl
    TKDE
    TKXSBase
    TKDESTEP
    TKDEIGES
    TKCAF
    TKLCAF
    TKCDF
    TKXCAF
    TKVCAF
)

# 以后 io / model / mesh / scene / viewer / measure 用这个函数链接 OCCT。
# 只记录依赖，不把 OCCT 拉进空窗口 exe（没有引用符号时链接器不会写入 DLL 依赖）。
function(steplens_link_occt target)
    if(NOT OpenCASCADE_INCLUDE_DIR OR NOT OpenCASCADE_LIBRARIES)
        message(FATAL_ERROR "find_package(OpenCASCADE) 没有导出包含目录或库列表")
    endif()
    target_include_directories(${target} PUBLIC "${OpenCASCADE_INCLUDE_DIR}")
    target_link_libraries(${target} PUBLIC ${OpenCASCADE_LIBRARIES})
endfunction()

function(steplens_check_occt_toolkits)
    if(NOT OpenCASCADE_LIBRARIES)
        message(FATAL_ERROR "OpenCASCADE_LIBRARIES 为空")
    endif()
    set(missing "")
    foreach(toolkit IN LISTS STEPLENS_OCCT_TOOLKITS)
        if(TARGET ${toolkit})
            continue()
        endif()
        set(found OFF)
        foreach(lib IN LISTS OpenCASCADE_LIBRARIES)
            if(lib MATCHES "(^|[/\\\\])${toolkit}\\.")
                set(found ON)
                break()
            endif()
        endforeach()
        if(NOT found)
            list(APPEND missing ${toolkit})
        endif()
    endforeach()
    if(missing)
        message(WARNING
            "这些 OCCT 工具包没有出现在 OpenCASCADE 的导出里: ${missing}。请对照 vcpkg_installed/x64-windows/lib 里的实际文件名。TKDEIGES 的安装名在架构 3.3 里标了需确认；源码树里的目录名是 TKDEIGES。")
    endif()
endfunction()
