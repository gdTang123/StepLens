add_executable(steplens_tests
    core/VersionTest.cpp
    core/ResultTest.cpp
    core/CancelTest.cpp
    core/ProgressSinkTest.cpp
    core/TaskSchedulerTest.cpp
    core/LogTest.cpp
    core/CrashDumpTest.cpp)
target_link_libraries(steplens_tests PRIVATE steplens::core GTest::gtest_main)
target_include_directories(steplens_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
steplens_apply_warnings(steplens_tests)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
