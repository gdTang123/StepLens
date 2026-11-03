# 安装布局对应绿色版目录 dist/StepLens/。
# windeployqt、OCCT DLL 和 VC++ 运行库由 scripts/package.py 与后续 T17 处理。

install(TARGETS StepLens
    RUNTIME DESTINATION StepLens)

install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/qt.conf"
    "${CMAKE_SOURCE_DIR}/packaging/README-使用说明.txt"
    DESTINATION StepLens)

install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/licenses/README.md"
    DESTINATION StepLens/licenses)
