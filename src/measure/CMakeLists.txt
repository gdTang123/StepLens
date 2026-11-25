# measure：MeasureTool、SnapPicker、DistanceMeasure、AngleMeasure（T12）
steplens_add_module(measure DistanceMeasure.cpp AngleMeasure.cpp DistanceMeasure.h AngleMeasure.h)
target_link_libraries(steplens_measure PUBLIC steplens::occtsupport)
