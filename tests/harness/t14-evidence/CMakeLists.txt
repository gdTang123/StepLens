cmake_minimum_required(VERSION 3.21)
project(StepLensT14Evidence LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Test)
find_package(GTest CONFIG REQUIRED)
add_executable(t14_evidence
    ../../unit/app/SettingsTest.cpp
    StatusNoticeTest.cpp
    ../../../src/app/settings/Settings.cpp
    ../../../src/app/StatusNotice.cpp
    ../../../src/core/Log.cpp)
target_include_directories(t14_evidence PRIVATE ../../../src)
target_link_libraries(t14_evidence PRIVATE Qt6::Core Qt6::Widgets Qt6::Test GTest::gtest_main)
