#pragma once
#include "platform.hpp"

namespace qb {
// Coordinates are screen pixels and may be negative on secondary monitors.
inline RECT floatingBounds(RECT desired, RECT work, bool sidebar, bool dockRight) {
    LONG width = std::min(desired.right - desired.left, work.right - work.left);
    LONG height = std::min(desired.bottom - desired.top, work.bottom - work.top);
    LONG x = sidebar ? (dockRight ? work.right - width : work.left) : std::clamp(desired.left, work.left, work.right - width);
    LONG y = std::clamp(desired.top, work.top, work.bottom - height);
    return {x, y, x + width, y + height};
}
inline bool nearestRightEdge(RECT window, RECT work) {
    return static_cast<long long>(window.left) + window.right >= static_cast<long long>(work.left) + work.right;
}
}
