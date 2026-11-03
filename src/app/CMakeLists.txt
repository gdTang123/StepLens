qt_add_executable(StepLens
    main.cpp
    MainWindow.h
    MainWindow.cpp)

target_link_libraries(StepLens PRIVATE steplens::core Qt6::Widgets)
target_include_directories(StepLens PRIVATE "${CMAKE_SOURCE_DIR}/src")
steplens_apply_warnings(StepLens)

set_target_properties(StepLens PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "StepLens"
    FOLDER "app")
