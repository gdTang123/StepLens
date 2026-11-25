#pragma once

#include <cmath>

namespace steplens::viewer {

class ClickGesture {
public:
    static constexpr double kThreshold = 6.0;

    void press(double x, double y) {
        m_originX = x;
        m_originY = y;
        m_dragged = false;
    }

    void move(double x, double y) {
        if (distance(x, y) > kThreshold) {
            m_dragged = true;
        }
    }

    [[nodiscard]] bool isClick(double x, double y) const {
        return !m_dragged && distance(x, y) <= kThreshold;
    }

private:
    [[nodiscard]] double distance(double x, double y) const {
        return std::hypot(x - m_originX, y - m_originY);
    }

    double m_originX = 0.0;
    double m_originY = 0.0;
    bool m_dragged = false;
};

} // namespace steplens::viewer
