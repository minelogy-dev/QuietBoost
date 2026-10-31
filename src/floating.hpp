#pragma once
#include "platform.hpp"
#include <functional>

namespace qb {
class FloatingWindow {
    StateStore& store_;
    HWND window_ = nullptr, slider_ = nullptr, reset_ = nullptr, mode_ = nullptr, open_ = nullptr, hide_ = nullptr;
    HFONT font_ = nullptr, number_ = nullptr, smallNumber_ = nullptr;
    HBRUSH background_ = nullptr, header_ = nullptr;
    std::function<void(double)> changeGain_;
    std::function<void()> resetGain_, openMain_;
    int dpi_ = 96, maximum_ = 2000;
    double percent_ = 100;
    bool active_ = false, automatic_ = true, sidebar_ = false, dockRight_ = true, preview_ = false, updating_ = false;
    int s(int value) const { return MulDiv(value, dpi_, 96); }
    void layout();
    void constrain(bool chooseEdge = false);
    void save() noexcept;
    void paint(HDC supplied = nullptr);
    LRESULT message(UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l);
public:
    explicit FloatingWindow(StateStore& store) : store_(store) {}
    ~FloatingWindow();
    FloatingWindow(const FloatingWindow&) = delete;
    FloatingWindow& operator=(const FloatingWindow&) = delete;
    void create(HWND main, int maximum, bool preview, std::function<void(double)> changeGain,
                std::function<void()> resetGain, std::function<void()> openMain, bool taskbarPreview = false);
    void update(double percent, bool active, bool automatic);
    void toggle();
    void show(bool visible);
    void setSidebar(bool sidebar);
    bool visible() const { return window_ && IsWindowVisible(window_); }
    bool sidebar() const { return sidebar_; }
    HWND handle() const { return window_; }
    void destroy();
};
}
