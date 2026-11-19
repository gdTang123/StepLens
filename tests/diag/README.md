# 1111-048 Windows 退出诊断附属文件

`static-dtor-join-gate.cpp` 是独立的无 OCCT 实验，不加入 CMake / gtest。
Windows 作业先用 `cl` 编译并运行它，再编定向 `steplens_model_tests`。

不要把本目录改成产品测试，也不要改 DocumentRelease 的谓词、门闩或超时。
