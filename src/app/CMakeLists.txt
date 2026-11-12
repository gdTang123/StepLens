qt_add_executable(StepLens
    main.cpp
    MainWindow.h
    MainWindow.cpp)

target_link_libraries(StepLens PRIVATE
    steplens::core
    steplens::pipeline
    steplens::ui
    steplens::viewer
    Qt6::Widgets)
target_include_directories(StepLens PRIVATE "${CMAKE_SOURCE_DIR}/src")
# 先链 Qt，再套警告。Qt 已经注入 utf-8 / permissive / Zc:__cplusplus，这里只补 /EHsc。
steplens_apply_warnings(StepLens)
steplens_apply_exceptions(StepLens OFF)

set_target_properties(StepLens PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "StepLens"
    FOLDER "app")
