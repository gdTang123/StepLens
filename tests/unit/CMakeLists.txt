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
if(NOT STEPLENS_CORE_ONLY)
    target_sources(steplens_tests PRIVATE occt/OcctCompatTest.cpp)
    target_link_libraries(steplens_tests PRIVATE steplens::io)
    target_compile_definitions(steplens_tests PRIVATE "STEPLENS_EXPECTED_OCCT=\"${STEPLENS_EXPECTED_OCCT}\"")
endif()
steplens_apply_warnings(steplens_tests)
set_target_properties(steplens_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(steplens_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
