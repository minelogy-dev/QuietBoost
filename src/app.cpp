#include "audio.hpp"
#include "floating.hpp"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <fstream>
#include <iomanip>

namespace qb {
enum : int { Input = 100, Output, GainEdit, GainSlider, Automatic, Connect, Refresh, ResetGain, Install, ExitApp, Config, OpenApp, Donate, ToggleFloating, Sidebar, LoginStart };
constexpr UINT TrayMessage = WM_APP + 1, StartupMessage = WM_APP + 2;
static void png(HWND window, const fs::path& path) {
    RECT bounds{}; GetClientRect(window, &bounds); int width = bounds.right, height = bounds.bottom;
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    struct PrintContext { HWND parent; HDC dc; } context{window, memory};
    EnumChildWindows(window, [](HWND child, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<PrintContext*>(parameter);
        if (GetParent(child) != context.parent || !(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) return TRUE;
        RECT area{}; GetWindowRect(child, &area); MapWindowPoints(nullptr, context.parent, reinterpret_cast<POINT*>(&area), 2);
        int saved = SaveDC(context.dc); SetViewportOrgEx(context.dc, area.left, area.top, nullptr); IntersectClipRect(context.dc, 0, 0, area.right - area.left, area.bottom - area.top);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(context.dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        wchar_t type[64]{}; GetClassNameW(child, type, 64);
        if (std::wstring(type) == L"ComboBox") {
            wchar_t text[2048]{}; GetWindowTextW(child, text, 2048);
            SelectObject(context.dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0))); SetBkMode(context.dc, TRANSPARENT); SetTextColor(context.dc, RGB(34, 52, 75));
            RECT textArea{7, 1, area.right - area.left - 24, area.bottom - area.top - 1}; DrawTextW(context.dc, text, -1, &textArea, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        RestoreDC(context.dc, saved); return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    ULONG_PTR token{}; Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token, &input, nullptr);
    {
        Gdiplus::Bitmap image(bitmap, nullptr); UINT count = 0, bytes = 0; Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> encoders(bytes); auto infos = reinterpret_cast<Gdiplus::ImageCodecInfo*>(encoders.data()); Gdiplus::GetImageEncoders(count, bytes, infos);
        bool saved = false;
        for (UINT i = 0; i < count; ++i) if (std::wstring(infos[i].MimeType) == L"image/png") { saved = image.Save(path.c_str(), &infos[i].Clsid, nullptr) == Gdiplus::Ok; break; }
        if (!saved) throw std::runtime_error("Cannot render preview");
    }
    Gdiplus::GdiplusShutdown(token); SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
}
class App {
    StateStore& store_; Settings settings_; Route route_; FloatingWindow floating_;
    HWND window_ = nullptr, input_ = nullptr, output_ = nullptr, edit_ = nullptr, slider_ = nullptr, automatic_ = nullptr, status_ = nullptr, silence_ = nullptr, detail_ = nullptr, connect_ = nullptr, refresh_ = nullptr, login_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr, number_ = nullptr;
    HBRUSH background_ = CreateSolidBrush(RGB(245, 247, 251)), white_ = CreateSolidBrush(RGB(255, 255, 255));
    std::vector<Device> virtuals_, physical_;
    bool updating_ = false, formalExit_ = false, preview_ = false, hotkey_ = false;
    bool startupLaunch_ = false, startupRetry_ = false;
    unsigned startupPoll_ = 0;
    unsigned lastReset_ = 0, poll_ = 0;
    int dpi_ = 96;
    fs::path previewPath_;
    std::wstring previewMode_;
    double previewPercent_ = 650;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    int s(int value) const { return MulDiv(value, dpi_, 96); }
    HWND control(const wchar_t* type, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id = 0) {
        HWND result = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(w), s(h), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return result;
    }
    HWND label(const std::wstring& text, int x, int y, int w, int h) { return control(L"STATIC", text, SS_LEFT, x, y, w, h); }
    void setStatus(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }
    void failure(const std::exception& error) { auto text = wide(error.what()); store_.log(text); setStatus(L"操作未完成：" + text); }
    void updateActive() {
        bool active = static_cast<bool>(route_.engine);
        EnableWindow(input_, !active); EnableWindow(output_, !active); EnableWindow(refresh_, !active);
        EnableWindow(edit_, active || preview_); EnableWindow(slider_, active || preview_);
        EnableWindow(connect_, active || (!virtuals_.empty() && !physical_.empty()));
        SetWindowTextW(connect_, active ? L"停止并恢复设备" : L"启动并切换设备");
        floating_.update(route_.gain.target() * 100, active || preview_, route_.gain.automatic());
    }
    void gainUi(double percent, bool updateEdit = true) {
        updating_ = true;
        if (updateEdit) SetWindowTextW(edit_, std::to_wstring(static_cast<int>(std::round(percent))).c_str());
        double fraction = settings_.maxGainPercent == 100 ? 0 : std::log(percent / 100) / std::log(settings_.maxGainPercent / 100.0);
        SendMessageW(slider_, TBM_SETPOS, TRUE, static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000));
        std::wostringstream text; text << std::fixed << std::setprecision(1) << L"+" << 20 * std::log10(percent / 100) << L" dB  ·  振幅 " << percent / 100 << L" 倍";
        SetWindowTextW(detail_, text.str().c_str());
        floating_.update(percent, static_cast<bool>(route_.engine) || preview_, route_.gain.automatic());
        auto tip = L"QuietBoost · " + std::to_wstring(static_cast<int>(percent)) + L"%"; wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
        if (!preview_) Shell_NotifyIconW(NIM_MODIFY, &tray_); updating_ = false;
    }
    void reset() { route_.gain.reset(); gainUi(100); }
    void setGain(double percent, bool updateEdit = true) { percent = std::clamp(std::round(percent), 100.0, static_cast<double>(settings_.maxGainPercent)); route_.gain.setPercent(percent); gainUi(percent, updateEdit); }
    void choose(HWND combo, const std::vector<Device>& devices, const std::wstring& desired) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0); int selection = 0;
        for (size_t i = 0; i < devices.size(); ++i) { SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(devices[i].name.c_str())); if (devices[i].id == desired) selection = static_cast<int>(i); }
        if (!devices.empty()) SendMessageW(combo, CB_SETCURSEL, selection, 0);
    }
    void loadDevices() {
        Endpoints endpoints; auto all = endpoints.list(); virtuals_.clear(); physical_.clear();
        for (const auto& device : all) (device.virtualDevice ? virtuals_ : physical_).push_back(device);
        std::stable_sort(virtuals_.begin(), virtuals_.end(), [](auto& a, auto& b) { return (a.name.find(L"CABLE Input") != a.name.npos) > (b.name.find(L"CABLE Input") != b.name.npos); });
        choose(input_, virtuals_, settings_.virtualId);
        choose(output_, physical_, settings_.outputId.empty() ? endpoints.current(0) : settings_.outputId);
        if (virtuals_.empty()) setStatus(L"尚未安装虚拟设备。点击“安装虚拟设备”，完成后按官方要求重启。 ");
        else if (physical_.empty()) setStatus(L"请连接实际耳机或扬声器，再刷新设备。");
        else setStatus(L"设备已就绪 · 启动后自动切换默认输出");
        updateActive();
    }
    void start() {
        if (preview_) return;
        int inputIndex = static_cast<int>(SendMessageW(input_, CB_GETCURSEL, 0, 0)), outputIndex = static_cast<int>(SendMessageW(output_, CB_GETCURSEL, 0, 0));
        if (inputIndex < 0 || outputIndex < 0 || inputIndex >= static_cast<int>(virtuals_.size()) || outputIndex >= static_cast<int>(physical_.size())) return;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT)); EnableWindow(connect_, FALSE);
        try {
            settings_.virtualId = virtuals_[inputIndex].id; settings_.outputId = physical_[outputIndex].id; store_.saveSettings(settings_);
            route_.start(settings_.virtualId, settings_.outputId); startupRetry_ = false; gainUi(100); setStatus(L"正在转发 · " + physical_[outputIndex].name);
        } catch (const std::exception& e) { failure(e); }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW)); updateActive();
    }
    bool stop() {
        auto result = route_.stop(); gainUi(100); updateActive();
        if (result.success) setStatus(L"已停止 · 已恢复设备选择");
        else { setStatus(L"设备恢复未完成，请连接实际设备后重试。"); MessageBoxW(window_, result.message.c_str(), L"QuietBoost", MB_OK | MB_ICONWARNING); }
        return result.success;
    }
    static INT_PTR CALLBACK consentProc(HWND dialog, UINT message, WPARAM w, LPARAM) {
        if (message == WM_INITDIALOG) {
            EnableWindow(GetDlgItem(dialog, IDC_CONFIRM), FALSE);
            RECT parent{}, bounds{}; GetWindowRect(GetParent(dialog), &parent); GetWindowRect(dialog, &bounds);
            SetWindowPos(dialog, nullptr, parent.left + ((parent.right - parent.left) - (bounds.right - bounds.left)) / 2, parent.top + ((parent.bottom - parent.top) - (bounds.bottom - bounds.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER); return TRUE;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(w) == IDC_CONSENT) { EnableWindow(GetDlgItem(dialog, IDC_CONFIRM), IsDlgButtonChecked(dialog, IDC_CONSENT) == BST_CHECKED); return TRUE; }
            if (LOWORD(w) == IDC_CONFIRM && IsDlgButtonChecked(dialog, IDC_CONSENT) == BST_CHECKED) { EndDialog(dialog, IDOK); return TRUE; }
            if (LOWORD(w) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        }
        if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }
    void changeAutomatic() {
        bool enabled = SendMessageW(automatic_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!enabled && DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CONSENT), window_, consentProc, 0) != IDOK) {
            SendMessageW(automatic_, BM_SETCHECK, BST_CHECKED, 0); return;
        }
        route_.gain.setAutomatic(enabled);
        floating_.update(route_.gain.target() * 100, static_cast<bool>(route_.engine) || preview_, route_.gain.automatic());
    }
    void tick() {
        if (preview_) return;
        if (startupRetry_ && !route_.engine && ++startupPoll_ % 50 == 0) {
            loadDevices(); if (!virtuals_.empty() && !physical_.empty()) start();
        }
        if (lastReset_ != route_.gain.resets()) { lastReset_ = route_.gain.resets(); gainUi(100); setStatus(L"静音超时，已恢复 100% · 需要时可手动重新增强"); }
        std::wstring text;
        if (!route_.gain.automatic()) text = L"本次运行已关闭自动恢复 · 下次启动重新开启";
        else if (route_.gain.target() > 1) { std::wostringstream value; value << L"已启用保护 · 静音 " << std::fixed << std::setprecision(1) << route_.gain.silentSeconds() << L" 秒"; text = value.str(); }
        else text = L"已启用保护 · 当前为原始增益";
        SetWindowTextW(silence_, text.c_str());
        floating_.update(route_.gain.target() * 100, static_cast<bool>(route_.engine), route_.gain.automatic());
        if (route_.engine) {
            auto error = route_.engine->error();
            if (++poll_ % 15 == 0 && error.empty()) {
                try { Endpoints endpoints; auto all = endpoints.list(); for (auto& id : {route_.virtualId, route_.outputId}) if (!std::any_of(all.begin(), all.end(), [&](auto& d) { return d.id == id; })) error = L"音频设备已断开。"; }
                catch (const std::exception& e) { error = wide(e.what()); }
            }
            if (!error.empty() || !route_.engine->running.load()) { route_.emergencyStop(); gainUi(100); updateActive(); setStatus(L"转发已停止：" + error); }
        }
        RECT meters{s(47), s(445), s(771), s(472)}; InvalidateRect(window_, &meters, FALSE);
    }
    void tray(bool add) {
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window_; tray_.uID = 1; tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray_.uCallbackMessage = TrayMessage; tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!*tray_.szTip) wcscpy_s(tray_.szTip, L"QuietBoost · 100%"); if (!preview_) Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &tray_);
    }
    void trayMenu() {
        HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, OpenApp, L"打开 QuietBoost");
        AppendMenuW(menu, MF_STRING | (floating_.visible() ? MF_CHECKED : 0), ToggleFloating, L"显示悬浮窗");
        AppendMenuW(menu, MF_STRING | (floating_.sidebar() ? MF_CHECKED : 0), Sidebar, L"切换悬浮窗 / 侧边条");
        AppendMenuW(menu, MF_STRING, ResetGain, L"恢复至 100%"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, ExitApp, L"正式退出并恢复设备");
        POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_); TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); DestroyMenu(menu); PostMessageW(window_, WM_NULL, 0, 0);
    }
    void createControls() {
        font_ = CreateFontW(-s(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        heading_ = CreateFontW(-s(32), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        number_ = CreateFontW(-s(27), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HWND heading = label(L"QuietBoost", 30, 22, 500, 43); SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), TRUE);
        label(L"原生音频增益 · 单 EXE · 无需另装运行库", 32, 72, 470, 26);
        control(L"BUTTON", L"显示 / 隐藏悬浮窗", BS_PUSHBUTTON | WS_TABSTOP, 570, 26, 220, 32, ToggleFloating);
        control(L"BUTTON", L"VB-CABLE 许可 / 捐助", BS_PUSHBUTTON | WS_TABSTOP, 570, 69, 220, 29, Donate);
        label(L"音频路径", 48, 125, 700, 25); label(L"虚拟输入", 48, 169, 90, 25); label(L"实际输出", 48, 218, 90, 25);
        input_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 148, 164, 620, 250, Input);
        output_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 148, 213, 620, 250, Output);
        label(L"输出增益", 48, 311, 300, 28);
        edit_ = control(L"EDIT", L"100", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, 558, 307, 151, 42, GainEdit); SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(number_), TRUE); SendMessageW(edit_, EM_SETLIMITTEXT, 7, 0);
        HWND percent = label(L"%", 723, 310, 45, 36); SendMessageW(percent, WM_SETFONT, reinterpret_cast<WPARAM>(number_), TRUE);
        detail_ = label(L"+0.0 dB · 振幅 1.0 倍", 48, 352, 690, 26);
        slider_ = control(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 44, 391, 728, 35, GainSlider); SendMessageW(slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        label(L"100% · 原始增益", 48, 426, 380, 24); label(std::to_wstring(settings_.maxGainPercent) + L"%", 691, 426, 78, 24);
        automatic_ = control(L"BUTTON", L"连续静音 " + std::to_wstring(settings_.silenceTimeoutMs / 1000) + L" 秒后恢复至 100%", BS_AUTOCHECKBOX | WS_TABSTOP, 49, 507, 704, 27, Automatic); SendMessageW(automatic_, BM_SETCHECK, BST_CHECKED, 0);
        silence_ = label(L"已启用保护 · 当前为原始增益", 69, 544, 690, 24);
        status_ = label(L"正在检查设备…", 32, 599, 755, 41);
        login_ = control(L"BUTTON", L"开机自启 · 登录后仅显示侧边音量条", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 641, 440, 27, LoginStart);
        label(L"Ctrl + Alt + 0 恢复增益", 493, 644, 297, 23);
        if (preview_) EnableWindow(login_, FALSE);
        else {
            try { SendMessageW(login_, BM_SETCHECK, LoginStartup(store_).enabled() ? BST_CHECKED : BST_UNCHECKED, 0); }
            catch (const std::exception& e) { store_.log(wide(e.what())); EnableWindow(login_, FALSE); }
        }
        connect_ = control(L"BUTTON", L"启动并切换设备", BS_PUSHBUTTON | WS_TABSTOP, 30, 683, 180, 34, Connect);
        refresh_ = control(L"BUTTON", L"刷新设备", BS_PUSHBUTTON | WS_TABSTOP, 220, 683, 100, 34, Refresh);
        control(L"BUTTON", L"恢复 100%", BS_PUSHBUTTON | WS_TABSTOP, 330, 683, 110, 34, ResetGain);
        control(L"BUTTON", L"安装虚拟设备", BS_PUSHBUTTON | WS_TABSTOP, 450, 683, 128, 34, Install);
        control(L"BUTTON", L"配置", BS_PUSHBUTTON | WS_TABSTOP, 588, 683, 70, 34, Config);
        control(L"BUTTON", L"正式退出", BS_PUSHBUTTON | WS_TABSTOP, 668, 683, 122, 34, ExitApp);
        floating_.create(window_, settings_.maxGainPercent, preview_, [this](double percent) { setGain(percent); }, [this] { reset(); }, [this] { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }, previewMode_ == L"--ui-preview", startupLaunch_);
        if (preview_) floating_.show(false);
        tray(true);
        if (!preview_) hotkey_ = RegisterHotKey(window_, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '0') != FALSE;
        updateActive(); SetTimer(window_, 1, 100, nullptr); PostMessageW(window_, StartupMessage, 0, 0);
    }
    void paint(HDC supplied = nullptr) {
        PAINTSTRUCT ps{}; HDC dc = supplied ? supplied : BeginPaint(window_, &ps); RECT client{}; GetClientRect(window_, &client); FillRect(dc, &client, background_);
        for (const RECT& rectangle : {RECT{s(30), s(112), s(790), s(279)}, RECT{s(30), s(295), s(790), s(481)}, RECT{s(30), s(497), s(790), s(582)}}) FillRect(dc, &rectangle, white_);
        float in = preview_ ? .02f : route_.engine && route_.gain.silentSeconds() < .3 ? route_.engine->inputPeak.load() : 0;
        float out = preview_ ? .13f : route_.engine ? route_.engine->outputPeak.load() : 0;
        auto meter = [&](int x, float peak) {
            RECT area{s(x), s(460), s(x + 251), s(470)}; HBRUSH background = CreateSolidBrush(RGB(230, 235, 242)); FillRect(dc, &area, background); DeleteObject(background);
            double db = peak > 0 ? 20 * std::log10(peak) : -80; area.right = area.left + static_cast<int>(s(251) * std::clamp((db + 80) / 80, 0.0, 1.0));
            HBRUSH bar = CreateSolidBrush(peak > .95 ? RGB(220, 141, 40) : RGB(31, 143, 111)); FillRect(dc, &area, bar); DeleteObject(bar);
        };
        meter(83, in); meter(399, out); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(60, 78, 99)); SelectObject(dc, font_);
        TextOutW(dc, s(48), s(454), L"输入", 2); TextOutW(dc, s(364), s(454), L"输出", 2);
        const wchar_t* limited = route_.engine && route_.engine->limiting.load() ? L"正在限幅" : L"限幅待命"; TextOutW(dc, s(680), s(454), limited, 4);
        if (!supplied) EndPaint(window_, &ps);
    }
    LRESULT message(UINT message, WPARAM w, LPARAM l) {
        if (message == taskbarCreated_) { tray(true); return 0; }
        switch (message) {
        case WM_CREATE: createControls(); return 0;
        case StartupMessage:
            if (preview_) {
                virtuals_.push_back({L"preview-v", L"CABLE Input (VB-Audio Virtual Cable)", true}); physical_.push_back({L"preview-p", L"扬声器 (Realtek Audio)", false});
                choose(input_, virtuals_, L""); choose(output_, physical_, L""); setGain(previewPercent_); updateActive(); SetWindowTextW(silence_, L"已启用保护 · 当前静音 0.0 秒"); setStatus(L"界面预览 · 不接入音频，不切换设备");
                if (previewMode_ == L"--ui-preview") floating_.show(true); else SetTimer(window_, 2, 350, nullptr);
            } else {
                startupRetry_ = startupLaunch_ && settings_.autoStart;
                loadDevices(); if (settings_.autoStart && !virtuals_.empty() && !physical_.empty()) start();
            }
            return 0;
        case WM_PAINT: paint(); return 0;
        case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(w)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(w); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(34, 52, 75)); RECT bounds{}; GetWindowRect(reinterpret_cast<HWND>(l), &bounds); MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
            return reinterpret_cast<LRESULT>((bounds.top >= s(112) && bounds.top < s(582)) ? white_ : background_);
        }
        case WM_HSCROLL: if (reinterpret_cast<HWND>(l) == slider_) setGain(100 * std::pow(settings_.maxGainPercent / 100.0, SendMessageW(slider_, TBM_GETPOS, 0, 0) / 1000.0)); return 0;
        case WM_COMMAND: {
            int id = LOWORD(w), notification = HIWORD(w);
            if (id == GainEdit && !updating_) {
                wchar_t text[32]{}; GetWindowTextW(edit_, text, 32); double value = _wtof(text);
                if (notification == EN_KILLFOCUS) setGain(value);
                else if (notification == EN_CHANGE && value >= 100 && value <= settings_.maxGainPercent) setGain(value, false);
                return 0;
            }
            if (notification != BN_CLICKED && id != OpenApp && id != ExitApp && id != ResetGain && id != ToggleFloating && id != Sidebar) return 0;
            switch (id) {
                case Connect: startupRetry_ = false; if (route_.engine) stop(); else start(); break;
                case Refresh: loadDevices(); break;
                case ResetGain: reset(); break;
                case Automatic: changeAutomatic(); break;
                case OpenApp: ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); break;
                case ToggleFloating: floating_.toggle(); break;
                case Sidebar: floating_.setSidebar(!floating_.sidebar()); break;
                case LoginStart:
                    if (!preview_) {
                        LoginStartup registration(store_);
                        try {
                            bool enabled = SendMessageW(login_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                            registration.setEnabled(enabled);
                            if (enabled) { settings_.autoStart = true; store_.saveSettings(settings_); }
                            setStatus(enabled ? L"已开启开机自启 · 登录后只显示侧边音量条，无需保留下载的 EXE。" : L"已关闭开机自启。");
                        } catch (const std::exception& e) { SendMessageW(login_, BM_SETCHECK, registration.enabled() ? BST_CHECKED : BST_UNCHECKED, 0); failure(e); }
                    }
                    break;
                case ExitApp: formalExit_ = true; SendMessageW(window_, WM_CLOSE, 0, 0); break;
                case Install:
                    if (route_.engine) { setStatus(L"请先停止转发，再安装或维护虚拟设备。"); break; }
                    setStatus(L"VB-CABLE 由 VB-Audio 提供，属于 donationware。安装后按官方要求重启。"); installDriver(window_, store_); break;
                case Config: ShellExecuteW(window_, L"open", L"notepad.exe", (L"\"" + store_.config.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); setStatus(L"修改配置后，正式退出并重新启动工具生效。"); break;
                case Donate: ShellExecuteW(window_, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL); break;
            }
            return 0;
        }
        case WM_TIMER:
            if (w == 1) tick();
            else if (w == 2) {
                KillTimer(window_, 2);
                if (previewMode_ == L"--render-floating-preview" || previewMode_ == L"--render-sidebar-preview" || previewMode_ == L"--render-rail-preview") {
                    floating_.setSidebar(previewMode_ != L"--render-floating-preview");
                    floating_.setCollapsed(previewMode_ == L"--render-rail-preview");
                    UpdateWindow(floating_.handle()); png(floating_.handle(), previewPath_);
                } else png(window_, previewPath_);
                formalExit_ = true; DestroyWindow(window_);
            }
            return 0;
        case WM_HOTKEY: reset(); return 0;
        case TrayMessage: if (l == WM_LBUTTONDBLCLK) { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); } else if (l == WM_RBUTTONUP) trayMenu(); return 0;
        case WM_CLOSE: if (!formalExit_) { ShowWindow(window_, SW_HIDE); return 0; } if (preview_ || stop()) DestroyWindow(window_); else formalExit_ = false; return 0;
        case WM_QUERYENDSESSION: route_.emergencyStop(); return TRUE;
        case WM_ENDSESSION: if (w) { route_.emergencyStop(); DestroyWindow(window_); } return 0;
        case WM_DESTROY: floating_.destroy(); if (hotkey_) UnregisterHotKey(window_, 1); if (!preview_) Shell_NotifyIconW(NIM_DELETE, &tray_); KillTimer(window_, 1); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); app->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
        if (!app) return DefWindowProcW(window, message, w, l);
        try { return app->message(message, w, l); } catch (const std::exception& e) { app->failure(e); return 0; }
    }
public:
    App(StateStore& store, Settings settings, fs::path preview, std::wstring previewMode = L"", bool startup = false, double previewPercent = 650) : store_(store), settings_(std::move(settings)), route_(settings_, store_), floating_(store), preview_(!preview.empty()), startupLaunch_(startup), previewPath_(std::move(preview)), previewMode_(std::move(previewMode)), previewPercent_(previewPercent) { dpi_ = static_cast<int>(GetDpiForSystem()); }
    ~App() { if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_); if (number_) DeleteObject(number_); DeleteObject(background_); DeleteObject(white_); }
    int run() {
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"QuietBoost.Native.v2"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(nullptr, IDI_APPLICATION); RegisterClassExW(&type);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT client{0, 0, s(820), s(737)}; AdjustWindowRect(&client, style, FALSE);
        int width = client.right - client.left, height = client.bottom - client.top;
        HWND window = CreateWindowExW(0, type.lpszClassName, L"QuietBoost · 原生音频增益", style, std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2), std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2), width, height, nullptr, nullptr, type.hInstance, this);
        if (!window) throw std::runtime_error("Cannot create main window"); ShowWindow(window, startupLaunch_ ? SW_HIDE : SW_SHOW); UpdateWindow(window);
        MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (floating_.handle() && (message.hwnd == floating_.handle() || IsChild(floating_.handle(), message.hwnd)) && IsDialogMessageW(floating_.handle(), &message)) continue;
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return static_cast<int>(message.wParam);
    }
};

int application(const std::vector<std::wstring>& arguments, const fs::path& stateDirectory) {
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    if (arguments.size() == 5 && arguments[0] == L"--watchdog") return watchdog(arguments);
    StateStore store(stateDirectory);
    if (arguments.size() == 1 && arguments[0] == L"--recover") return recover(store).success ? 0 : 1;
    if (arguments.size() == 2 && arguments[0] == L"--verify-driver") { auto installer = verifyDriver(store); atomicWrite(arguments[1], L"Embedded driver resources match SHA-256 manifest.\r\nInstaller Authenticode verified successfully.\r\nNo driver installed.\r\n" + installer.wstring()); return 0; }
    if (arguments.size() == 2 && arguments[0] == L"--verify-policy") {
        Endpoints endpoints; auto console = endpoints.current(0), multimedia = endpoints.current(1), communication = endpoints.current(2);
        if (console.empty() || console != multimedia) throw std::runtime_error("Policy probe requires identical ordinary playback defaults; no changes made");
        endpoints.select(console, 0); endpoints.select(multimedia, 1);
        if (endpoints.current(0) != console || endpoints.current(1) != multimedia || endpoints.current(2) != communication) throw std::runtime_error("Unexpected default selection change");
        atomicWrite(arguments[1], L"Native PolicyConfig COM calls passed using the already selected endpoint. All three default roles unchanged.\r\n"); return 0;
    }
    if (arguments.size() == 2 && arguments[0] == L"--devices") {
        Endpoints endpoints; std::wostringstream report;
        for (auto& d : endpoints.list()) report << d.name << L"\r\n" << d.id << L"\r\n";
        for (int role = 0; role < 3; ++role) report << L"Role " << role << L"=" << endpoints.current(role) << L"\r\n";
        atomicWrite(arguments[1], report.str()); return 0;
    }
    if (arguments.size() == 2 && arguments[0] == L"--audio-probe") {
        Endpoints endpoints; auto original = endpoints.current(0); if (original.empty()) throw std::runtime_error("No default output device");
        Settings settings; GainControl gain(settings); AudioEngine engine(settings, gain); engine.start(original, original, true); Sleep(1500);
        auto error = engine.error(); auto frames = engine.renderedFrames.load(), received = engine.capturedFrames.load(); engine.stop();
        if (!error.empty() || !frames || original != endpoints.current(0)) throw std::runtime_error("Native WASAPI probe failed");
        atomicWrite(arguments[1], L"Native WASAPI probe passed.\r\nRendered frames=" + std::to_wstring(frames) + L"\r\nCaptured frames=" + std::to_wstring(received) + L"\r\nOnly digital silence was rendered; default selection and volume were not modified.\r\n"); return 0;
    }
    bool preview = (arguments.size() == 2 || arguments.size() == 3) && (arguments[0] == L"--render-preview" || arguments[0] == L"--render-floating-preview" || arguments[0] == L"--render-sidebar-preview" || arguments[0] == L"--render-rail-preview" || arguments[0] == L"--ui-preview");
    double previewPercent = 650;
    if (preview && arguments.size() == 3) {
        size_t consumed = 0; previewPercent = std::stod(arguments[2], &consumed);
        if (consumed != arguments[2].size() || !std::isfinite(previewPercent) || previewPercent < 100 || previewPercent > 1000000) throw std::runtime_error("Preview gain must be 100..1000000");
    }
    bool startup = arguments.size() == 1 && arguments[0] == L"--startup";
    Handle instance(CreateMutexW(nullptr, FALSE, L"Local\\QuietBoostNative.SingleInstance.v1"));
    DWORD result = WaitForSingleObject(instance.get(), 0);
    if (!preview && result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        if (!startup) { HWND existing = FindWindowW(L"QuietBoost.Native.v2", nullptr); if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); } }
        return 0;
    }
    struct Unlock { HANDLE value; bool held; ~Unlock() { if (held) ReleaseMutex(value); } } unlock{instance.get(), result == WAIT_OBJECT_0 || result == WAIT_ABANDONED};
    bool recoveryReady = true;
    if (!preview) { auto recovered = recover(store); recoveryReady = recovered.success; if (!recovered.success) { store.log(recovered.message); if (!startup) MessageBoxW(nullptr, recovered.message.c_str(), L"上次运行的设备恢复未完成", MB_OK | MB_ICONWARNING); } }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    auto settings = preview ? Settings{} : store.loadSettings();
    if (preview) settings.maxGainPercent = std::max(settings.maxGainPercent, static_cast<int>(std::ceil(previewPercent)));
    if (!recoveryReady) settings.autoStart = false;
    App app(store, settings, preview ? fs::path(arguments[1]) : fs::path{}, preview ? arguments[0] : L"", startup, previewPercent); return app.run();
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0; LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count); std::vector<std::wstring> arguments; qb::fs::path stateDirectory;
    for (int i = 1; i < count; ++i) { if (std::wstring(raw[i]) == L"--state-dir" && i + 1 < count) stateDirectory = raw[++i]; else arguments.emplace_back(raw[i]); } LocalFree(raw);
    try { return qb::application(arguments, stateDirectory); }
    catch (const std::exception& e) {
        try { qb::StateStore store(stateDirectory); store.log(qb::wide(e.what())); } catch (...) {}
        if (arguments.empty()) MessageBoxW(nullptr, qb::wide(e.what()).c_str(), L"QuietBoost 启动失败", MB_OK | MB_ICONERROR); return 1;
    }
}
