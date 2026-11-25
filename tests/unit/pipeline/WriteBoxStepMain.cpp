#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>

#include <iostream>

// 可追溯模型：BRepPrimAPI_MakeBox(10, 20, 30)，与加载测试同一写法。不是像素假图。
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: steplens_write_box_step <box.step>\n";
        return 2;
    }
    STEPControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (writer.Transfer(box, STEPControl_AsIs) != IFSelect_RetDone) {
        std::cerr << "STEP transfer failed\n";
        return 1;
    }
    if (writer.Write(argv[1]) != IFSelect_RetDone) {
        std::cerr << "STEP write failed\n";
        return 1;
    }
    return 0;
}
