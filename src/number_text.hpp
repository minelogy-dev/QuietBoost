#pragma once
#include "platform.hpp"

namespace qb {
// Measure the entire percentage, including '%', using the actual selected font.
// Derive a smaller font only when needed; never change the panel or its hit areas.
inline SIZE drawFittedNumber(HDC dc, HFONT base, const std::wstring& text, RECT bounds, UINT alignment) {
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return {};
    HGDIOBJ previous = SelectObject(dc, base); HFONT temporary = nullptr; SIZE extent{};
    LOGFONTW descriptor{}; GetObjectW(base, sizeof(descriptor), &descriptor);
    int pixels = std::max(1, static_cast<int>(std::abs(descriptor.lfHeight)));
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    while ((extent.cx > width || extent.cy > height) && pixels > 1) {
        double ratio = std::min(width / static_cast<double>(std::max(1L, extent.cx)), height / static_cast<double>(std::max(1L, extent.cy)));
        pixels = std::clamp(static_cast<int>(std::floor(pixels * ratio)), 1, pixels - 1);
        if (temporary) { SelectObject(dc, base); DeleteObject(temporary); }
        descriptor.lfHeight = -pixels; temporary = CreateFontIndirectW(&descriptor);
        if (!temporary) break;
        SelectObject(dc, temporary); GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    }
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | alignment);
    SelectObject(dc, previous); if (temporary) DeleteObject(temporary);
    return extent;
}
}
