# viewer：T09 最小冒烟。QOpenGLWidget + Aspect_NeutralWindow + OpenGl_FrameBuffer::InitWrapper。
# STEPLENS_USE_WNT_WINDOW=ON 时改为 WNT_Window（见 docs/bench/t09-gpu-smoke-checklist.md）。
# vis（含 TKOpenGl）只 PRIVATE 链接。公开头不含 OCCT 类型，不把全量 OCCT 再 PUBLIC 链出去。
option(STEPLENS_USE_WNT_WINDOW "Bind the 3D view with WNT_Window instead of QOpenGLWidget" OFF)

steplens_add_module(viewer
    ViewerWidget.cpp
    ViewerWidget.h)

# steplens_add_module 在链接 Qt 之前套语言开关。Qt 会再注入 /utf-8、/permissive-、/Zc:__cplusplus。
# 先去掉我们加的那一组，只留这一次 /EHsc 和警告。不要再手写 /EH*。
get_target_property(_viewer_opts steplens_viewer COMPILE_OPTIONS)
if(_viewer_opts)
    list(REMOVE_ITEM _viewer_opts
        "/permissive-" "-permissive-"
        "/Zc:__cplusplus" "-Zc:__cplusplus"
        "/utf-8" "-utf-8")
    set_property(TARGET steplens_viewer PROPERTY COMPILE_OPTIONS "${_viewer_opts}")
endif()

find_package(OpenGL REQUIRED)
if(NOT OPENGL_gl_LIBRARY)
    message(FATAL_ERROR "未找到 opengl32，wglGetCurrentDC 无法链接")
endif()
target_link_libraries(steplens_viewer PUBLIC
    Qt6::OpenGLWidgets
    Qt6::Widgets
    Qt6::OpenGL)
target_link_libraries(steplens_viewer PRIVATE
    steplens::occt_vis
    "${OPENGL_gl_LIBRARY}")
if(STEPLENS_USE_WNT_WINDOW)
    target_compile_definitions(steplens_viewer PUBLIC STEPLENS_USE_WNT_WINDOW=1)
endif()
