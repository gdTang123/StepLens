# core：Result、CancelToken、ProgressSink、TaskScheduler、日志、崩溃转储（T04）
steplens_add_module(core
    Version.cpp
    AppPaths.cpp
    MemoryMonitor.cpp
    ProgressSink.cpp
    LoadSession.cpp
    LoadCoordinator.cpp
    OcctLoadContract.cpp
    TaskScheduler.cpp
    Log.cpp
    LifecycleTrace.cpp
    CrashDump.cpp
    Version.h
    Result.h
    CancelToken.h
    ProgressSink.h
    LoadSession.h
    LoadCoordinator.h
    OcctLoadContract.h
    TaskScheduler.h
    Log.h
    LifecycleTrace.h
    CrashDump.h
    AppPaths.h
    MemoryMonitor.h)

if(WIN32)
    target_link_libraries(steplens_core PRIVATE dbghelp shell32 ole32 psapi)
endif()
