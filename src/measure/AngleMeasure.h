#pragma once

#include "measure/DistanceMeasure.h"

namespace steplens::measure {
struct AngleValue {
    double degrees;
};
Result<AngleValue> angleBetween(const MeasureShape& a, const MeasureShape& b);
} // namespace steplens::measure
