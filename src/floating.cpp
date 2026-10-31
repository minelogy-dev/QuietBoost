#include "floating.hpp"
#include "floating_layout.hpp"
#include <commctrl.h>
#include <windowsx.h>

namespace qb {
namespace {
enum : int { FloatSlider = 300, FloatReset, FloatMode, FloatOpen, FloatHide };
RECT workArea(RECT bounds) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) throw std::runtime_error("Cannot find floating window monitor");
    return monitor.rcWork;
}
}
FloatingWindow::~FloatingWindow() {
    destroy();
    for (HFONT font : {font_, number_, smallNumber_}) if (font) DeleteObject(font);
    if (background_) DeleteObject(background_);
    if (header_) DeleteObject(header_);
}
void FloatingWindow::create(HWND main, int maximum, bool preview, std::function<void(double)> changeGain,
                            std::function<void()> resetGain, std::function<void()> openMain, bool taskbarPreview) {
    if (window_) throw std::runtime_error("Floating window already exists");
    for (HFONT font : {font_, number_, smallNumber_}) if (font) DeleteObject(font);
    if (background_) DeleteObject(background_);
    if (header_) DeleteObject(header_);
    maximum_ = maximum; preview_ = preview; changeGain_ = std::move(changeGain); resetGain_ = std::move(resetGain); openMain_ = std::move(openMain);
    dpi_ = static_cast<int>(GetDpiForSystem());
    font_ = CreateFontW(-s(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    number_ = CreateFontW(-s(28), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    smallNumber_ = CreateFontW(-s(16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    background_ = CreateSolidBrush(RGB(245, 247, 251)); header_ = CreateSolidBrush(RGB(34, 52, 75));
    RECT parent{}; GetWindowRect(main, &parent); auto work = workArea(parent);
    int x = work.right - s(340), y = work.top + s(100); bool visible = true;
    if (!preview_) {
        auto file = store_.root / L"window.ini";
        auto integer = [&](const wchar_t* key, int fallback) {
            try {
                auto text = readIni(file, L"FloatingWindow", key, std::to_wstring(fallback)); size_t consumed = 0;
                auto value = std::stoi(text, &consumed);
                return consumed == text.size() && value >= -1000000 && value <= 1000000 ? value : fallback;
            } catch (...) { return fallback; }
        };
        x = integer(L"X", x); y = integer(L"Y", y); sidebar_ = integer(L"Sidebar", 0) == 1;
        dockRight_ = integer(L"DockRight", 1) != 0; visible = integer(L"Visible", 1) != 0;
    }
    WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"QuietBoost.Floating.v1"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassExW(&type);
    // Deliberately unowned: minimizing/hiding the main window keeps this tool visible.
    window_ = CreateWindowExW(WS_EX_TOPMOST | (preview_ && taskbarPreview ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW), type.lpszClassName, L"QuietBoost · 悬浮控制",
        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, x, y, s(320), s(196), nullptr, nullptr, type.hInstance, this);
    if (!window_) throw std::runtime_error("Cannot create floating window");
    layout(); constrain(); update(100, preview_, true);
    if (visible) ShowWindow(window_, SW_SHOWNOACTIVATE);
}
void FloatingWindow::layout() {
    if (slider_) DestroyWindow(slider_);
    slider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"输出增益", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | (sidebar_ ? TBS_VERT | TBS_REVERSED : 0),
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(FloatSlider), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    auto button = [&](HWND& handle, int id, const wchar_t* text) {
        if (!handle) handle = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SetWindowTextW(handle, text); SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    };
    button(reset_, FloatReset, L"恢复 100%"); button(mode_, FloatMode, sidebar_ ? L"展开" : L"侧边条");
    button(open_, FloatOpen, L"主窗口"); button(hide_, FloatHide, L"×");
    auto move = [&](HWND control, int x, int y, int w, int h) { MoveWindow(control, s(x), s(y), s(w), s(h), TRUE); };
    if (sidebar_) {
        move(hide_, 77, 6, 20, 22); move(slider_, 35, 100, 34, 138);
        move(reset_, 8, 247, 88, 29); move(mode_, 8, 283, 88, 29); move(open_, 8, 319, 88, 29);
    } else {
        move(hide_, 286, 6, 24, 22); move(slider_, 12, 108, 296, 32);
        move(reset_, 12, 151, 92, 31); move(mode_, 112, 151, 92, 31); move(open_, 212, 151, 96, 31);
    }
    SetWindowPos(window_, nullptr, 0, 0, s(sidebar_ ? 106 : 320), s(sidebar_ ? 360 : 196), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowTextW(window_, sidebar_ ? L"QuietBoost · 侧边条" : L"QuietBoost · 悬浮控制");
    update(percent_, active_, automatic_); InvalidateRect(window_, nullptr, TRUE);
}
void FloatingWindow::constrain(bool chooseEdge) {
    RECT current{}; GetWindowRect(window_, &current); auto work = workArea(current);
    if (sidebar_ && chooseEdge) dockRight_ = nearestRightEdge(current, work);
    auto bounds = floatingBounds(current, work, sidebar_, dockRight_);
    SetWindowPos(window_, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
}
void FloatingWindow::save() noexcept {
    if (preview_ || !window_) return;
    try {
        RECT bounds{}; GetWindowRect(window_, &bounds);
        std::wostringstream text;
        text << L"; Window layout only. Gain and protection state are never persisted.\r\n[FloatingWindow]\r\nVisible=" << (visible() ? 1 : 0)
             << L"\r\nSidebar=" << (sidebar_ ? 1 : 0) << L"\r\nDockRight=" << (dockRight_ ? 1 : 0) << L"\r\nX=" << bounds.left << L"\r\nY=" << bounds.top << L"\r\n";
        atomicWrite(store_.root / L"window.ini", text.str());
    } catch (const std::exception& e) { store_.log(L"Floating window layout: " + wide(e.what())); }
}
void FloatingWindow::update(double percent, bool active, bool automatic) {
    bool changed = percent_ != percent || active_ != active || automatic_ != automatic;
    percent_ = percent; active_ = active; automatic_ = automatic;
    if (!window_) return;
    updating_ = true;
    double fraction = maximum_ == 100 ? 0 : std::log(percent / 100) / std::log(maximum_ / 100.0);
    int position = static_cast<int>(std::round(std::clamp(fraction, 0.0, 1.0) * 1000));
    int displayed = sidebar_ ? 1000 - position : position;
    if (SendMessageW(slider_, TBM_GETPOS, 0, 0) != displayed) SendMessageW(slider_, TBM_SETPOS, TRUE, displayed);
    if ((IsWindowEnabled(slider_) != FALSE) != active) EnableWindow(slider_, active);
    updating_ = false;
    if (changed) InvalidateRect(window_, nullptr, FALSE);
}
void FloatingWindow::show(bool showWindow) {
    if (!window_) return;
    if (showWindow) constrain();
    ShowWindow(window_, showWindow ? SW_SHOWNOACTIVATE : SW_HIDE); save();
}
void FloatingWindow::toggle() { show(!visible()); }
void FloatingWindow::setSidebar(bool sidebar) {
    if (!window_) return;
    if (sidebar_ != sidebar) {
        RECT current{}; GetWindowRect(window_, &current); auto work = workArea(current);
        if (sidebar) dockRight_ = nearestRightEdge(current, work);
        sidebar_ = sidebar; layout();
        if (!sidebar_) SetWindowPos(window_, nullptr, dockRight_ ? work.right - s(332) : work.left + s(12), current.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        constrain();
    }
    show(true);
}
void FloatingWindow::destroy() {
    if (window_) { save(); DestroyWindow(window_); window_ = nullptr; }
}
void FloatingWindow::paint(HDC supplied) {
    PAINTSTRUCT ps{}; HDC dc = supplied ? supplied : BeginPaint(window_, &ps); RECT client{}; GetClientRect(window_, &client);
    FillRect(dc, &client, background_); RECT header{0, 0, client.right, s(36)}; FillRect(dc, &header, header_);
    SetBkMode(dc, TRANSPARENT); SelectObject(dc, font_); SetTextColor(dc, RGB(255, 255, 255));
    RECT title{s(12), 0, client.right - s(38), s(36)};
    DrawTextW(dc, sidebar_ ? L"QB · 拖动" : L"QuietBoost · 拖动移动", -1, &title, DT_SINGLELINE | DT_VCENTER);
    SetTextColor(dc, RGB(34, 52, 75)); SelectObject(dc, sidebar_ && percent_ >= 10000 ? smallNumber_ : number_);
    RECT number{s(12), s(44), client.right - s(12), s(80)};
    auto value = std::to_wstring(static_cast<int>(std::round(percent_))) + L"%";
    DrawTextW(dc, value.c_str(), -1, &number, DT_SINGLELINE | DT_VCENTER | (sidebar_ ? DT_CENTER : DT_LEFT));
    SelectObject(dc, font_);
    SetTextColor(dc, !active_ ? RGB(94, 108, 124) : automatic_ ? RGB(18, 122, 88) : RGB(161, 90, 18));
    RECT status{s(8), s(82), client.right - s(8), s(102)};
    const wchar_t* text = !active_ ? L"未转发" : automatic_ ? (sidebar_ ? L"保护开启" : L"正在转发 · 自动恢复已开启") : (sidebar_ ? L"保护已关" : L"正在转发 · 本次自动恢复已关闭");
    DrawTextW(dc, text, -1, &status, DT_SINGLELINE | DT_VCENTER | (sidebar_ ? DT_CENTER : DT_LEFT));
    if (!supplied) EndPaint(window_, &ps);
}
LRESULT FloatingWindow::message(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(w)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(background_);
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(window_, &point);
        if (point.y >= 0 && point.y < s(36) && point.x < s(sidebar_ ? 75 : 282)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_EXITSIZEMOVE: constrain(sidebar_); save(); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: constrain(); return 0;
    case WM_HSCROLL: case WM_VSCROLL:
        if (!updating_ && active_ && reinterpret_cast<HWND>(l) == slider_) {
            double position = static_cast<double>(SendMessageW(slider_, TBM_GETPOS, 0, 0));
            if (sidebar_) position = 1000 - position;
            changeGain_(100 * std::pow(maximum_ / 100.0, position / 1000));
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) != BN_CLICKED) return 0;
        switch (LOWORD(w)) {
        case FloatReset: resetGain_(); break;
        case FloatMode: setSidebar(!sidebar_); break;
        case FloatOpen: openMain_(); break;
        case FloatHide: show(false); break;
        }
        return 0;
    case WM_CLOSE: show(false); return 0;
    }
    return DefWindowProcW(window_, message, w, l);
}
LRESULT CALLBACK FloatingWindow::procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<FloatingWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<FloatingWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, w, l);
    try { return self->message(message, w, l); }
    catch (const std::exception& e) { self->store_.log(L"Floating window: " + wide(e.what())); return 0; }
}
}
