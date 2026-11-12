# ui：LoadingOverlay（T11）。装配树和属性面板是 T10 / T12，这里先不放。
steplens_add_module(ui
    LoadingOverlay.h
    LoadingOverlay.cpp)
target_link_libraries(steplens_ui PUBLIC Qt6::Widgets)

# steplens_add_module 在链接 Qt 之前套了语言开关。Qt 会再注入同一组，MSVC 会报 D9025。
get_target_property(_ui_opts steplens_ui COMPILE_OPTIONS)
if(_ui_opts)
    list(REMOVE_ITEM _ui_opts
        "/permissive-" "-permissive-"
        "/Zc:__cplusplus" "-Zc:__cplusplus"
        "/utf-8" "-utf-8")
    set_property(TARGET steplens_ui PROPERTY COMPILE_OPTIONS "${_ui_opts}")
endif()
