# 安装布局对应绿色版目录 dist/StepLens/。
# Qt、OCCT、FreeType 和 VC++ 运行库的收集在 scripts/package.py，不在这里复制 DLL。

install(TARGETS StepLens
    RUNTIME DESTINATION StepLens)

install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/qt.conf"
    "${CMAKE_SOURCE_DIR}/packaging/README-使用说明.txt"
    DESTINATION StepLens)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/licenses/"
    DESTINATION StepLens/licenses)
