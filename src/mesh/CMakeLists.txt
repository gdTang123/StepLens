# mesh：T07 单级 Tessellator（方案 B，c = 0.002）、MeshPolicy、MeshBatch。
# 粗/细两级是 T07+，SceneBuilder / AIS 是 T08，都不在这个库里。
# FillTriangles 在 TKV3d。TKOpenGl 只属于 vis，不进 mesh。
steplens_add_module(mesh
    MeshPolicy.cpp
    MeshPolicy.h
    MeshBatch.h
    Tessellator.cpp
    Tessellator.h)
target_link_libraries(steplens_mesh PUBLIC steplens::model TKV3d)
