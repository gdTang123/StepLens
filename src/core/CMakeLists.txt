# core：Result、CancelToken、ProgressSink、TaskScheduler、日志、崩溃转储（T04）
steplens_add_module(core
    Version.cpp
    AppPaths.cpp
    ProgressSink.cpp
    TaskScheduler.cpp
    Log.cpp
    CrashDump.cpp
    Version.h
    Result.h
    CancelToken.h
    ProgressSink.h
    TaskScheduler.h
    Log.h
    CrashDump.h
    AppPaths.h)

if(WIN32)
    target_link_libraries(steplens_core PRIVATE dbghelp)
endif()
