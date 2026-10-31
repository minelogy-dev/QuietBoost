#include "platform.hpp"
#include "floating_layout.hpp"
#include "floating.hpp"
#include "number_text.hpp"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <fstream>
#include <numeric>

namespace {
unsigned passed = 0;
std::wstring report;
void require(bool condition, const wchar_t* name) {
    if (!condition) { report += L"FAIL: " + std::wstring(name) + L"\r\n"; throw std::runtime_error("A native check failed"); }
    ++passed; report += L"PASS: " + std::wstring(name) + L"\r\n";
}
void nearValue(double actual, double expected, double epsilon, const wchar_t* name) { require(std::abs(actual - expected) < epsilon, name); }
template<class Action> void throws(Action action, const wchar_t* name) { bool caught = false; try { action(); } catch (...) { caught = true; } require(caught, name); }
void silence() {
    double now = 0; qb::Settings settings; qb::GainControl gain(settings, [&] { return now; });
    require(gain.automatic() && gain.target() == 1, L"每次启动默认开启保护且为 100%");
    gain.setPercent(2000); now = 4.999; require(!gain.tick(), L"五秒前不复位");
    now = 5; require(gain.tick(), L"没有音频回调时五秒复位"); nearValue(gain.target(), 1, 1e-10, L"复位至 100%");
    require(!gain.tick() && gain.resets() == 1, L"复位不重复计数");
    gain.setPercent(650); now = 9; gain.observe(.0001); now = 13; require(!gain.tick(), L"低声对白延长计时");
    now = 14; require(gain.tick(), L"从最后有效信号开始计时");
    gain.setPercent(800); gain.setAutomatic(false); now = 100; require(!gain.tick() && gain.target() == 8, L"关闭保护仅在当前会话保留增益");
    gain.setAutomatic(true); require(gain.target() == 1, L"重新开启后立即检查超时");
    qb::GainControl fresh(settings, [&] { return now; }); require(fresh.automatic() && fresh.target() == 1, L"新会话不继承关闭状态或当前增益");
    fresh.setPercent(300); now = 104; fresh.observe(1e-10); now = 105; require(fresh.tick(), L"阈值以下噪声不阻止复位");
    fresh.setPercent(300); now = 109; fresh.observe(std::numeric_limits<double>::quiet_NaN()); now = 110; require(fresh.tick(), L"NaN 不阻止复位");
    throws([&] { fresh.setPercent(99); }, L"拒绝低于界面范围的增益");
    throws([&] { fresh.setPercent(2001); }, L"默认上限有效");
    throws([&] { fresh.setPercent(std::numeric_limits<double>::infinity()); }, L"拒绝无限增益");
    qb::Settings extended; extended.maxGainPercent = 10000; qb::GainControl larger(extended); larger.setPercent(8000); nearValue(larger.target(), 80, 1e-10, L"配置可提高上限");
    extended.maxGainPercent = -1; throws([&] { extended.validate(); }, L"拒绝负配置");
    extended = {}; extended.silenceThresholdDb = std::numeric_limits<double>::quiet_NaN(); throws([&] { extended.validate(); }, L"拒绝非法静音阈值");
}
void dsp() {
    qb::Dsp dsp(48000); float left = .01f, right = .01f;
    for (int i = 0; i < 48000; ++i) { left = right = .01f; dsp.frame(left, right, 20); }
    nearValue(left, .2, 1e-5, L"20 倍增益符合振幅定义"); nearValue(left, right, 1e-8, L"左右增益一致");
    bool bounded = true, balanced = true; dsp.beginBlock();
    for (int i = 0; i < 48000; ++i) { left = i % 2 ? .7f : -.7f; right = left / 2; dsp.frame(left, right, 20); bounded &= std::abs(left) <= .980001f && std::abs(right) <= .980001f; balanced &= std::abs(right / left - .5) < 1e-6; }
    require(bounded, L"高信号不会超出限幅上限"); require(balanced, L"联动限幅保持声道比例"); require(dsp.limiting, L"报告限幅状态");
    left = std::numeric_limits<float>::quiet_NaN(); right = std::numeric_limits<float>::infinity(); dsp.frame(left, right, 20); require(std::isfinite(left) && std::isfinite(right), L"非法样本不会传播到输出");
    for (int i = 0; i < 48000; ++i) { left = right = .001f; dsp.frame(left, right, 1); } nearValue(left, .001, 1e-5, L"平滑回到原始增益");
    qb::Dsp extreme(48000); bounded = true;
    for (int i = 0; i < 48000; ++i) { left = right = 1; extreme.frame(left, right, 10000); bounded &= std::abs(left) <= .980001f; } require(bounded, L"超高配置增益仍受限幅约束");
}
void ring() {
    qb::Ring queue(3); require(queue.pop().left == 0, L"空缓冲输出数字静音");
    queue.push({1, 2}); queue.push({3, 4}); require(queue.size() == 2, L"正确记录缓冲帧数");
    require(queue.pop().right == 2 && queue.pop().left == 3, L"缓冲先进先出");
    for (int i = 0; i < 5; ++i) queue.push({static_cast<float>(i), 0});
    require(queue.size() == 3 && queue.pop().left == 2, L"缓冲有界并丢弃最旧数据"); queue.clear(); require(queue.size() == 0 && queue.pop().left == 0, L"清理积压恢复空缓冲");
}
class FakeEndpoints : public qb::EndpointService {
public:
    std::vector<qb::Device> available{{L"a", L"A", false}, {L"b", L"B", false}, {L"v", L"V", true}};
    std::wstring defaults[3]{L"v", L"v", L"comm"};
    unsigned writes = 0; bool failWrites = false;
    std::vector<qb::Device> list() override { return available; }
    std::wstring current(int role) override { return defaults[role]; }
    void select(const std::wstring& id, int role) override { if (failWrites) throw std::runtime_error("test"); defaults[role] = id; ++writes; }
};
void recovery() {
    qb::Snapshot snapshot{L"v", L"a", L"a", L"b"}; FakeEndpoints service;
    auto result = qb::restore(service, snapshot); require(result.success && service.defaults[0] == L"a" && service.defaults[1] == L"b", L"恢复各角色原设备");
    require(service.defaults[2] == L"comm", L"通信默认设备不受影响");
    service = {}; service.defaults[0] = service.defaults[1] = L"b"; result = qb::restore(service, snapshot); require(result.success && service.writes == 0, L"保留用户手动换设备的选择");
    service = {}; service.available.erase(service.available.begin()); result = qb::restore(service, snapshot); require(result.success && service.defaults[0] == L"b", L"原设备断开时回退实际设备");
    service = {}; service.available.erase(service.available.begin(), service.available.begin() + 2); result = qb::restore(service, snapshot); require(!result.success && service.writes == 0, L"无实际设备时不假报成功");
    service = {}; service.failWrites = true; require(!qb::restore(service, snapshot).success, L"切换失败保留恢复条件");
    service = {}; qb::restore(service, snapshot); service.writes = 0; require(qb::restore(service, snapshot).success && !service.writes, L"恢复操作幂等");
}
void config() {
    auto directory = qb::fs::path(qb::executablePath()).parent_path() / L"test-state" / std::to_wstring(GetCurrentProcessId());
    qb::StateStore store(directory); auto settings = store.loadSettings(); require(settings.maxGainPercent == 2000, L"创建默认配置");
    settings.maxGainPercent = 8000; settings.virtualId = L"virtual-id"; settings.outputId = L"physical-id"; store.saveSettings(settings); auto loaded = store.loadSettings();
    require(loaded.maxGainPercent == 8000 && loaded.virtualId == L"virtual-id" && loaded.outputId == L"physical-id", L"配置往返读取");
    require(qb::readIni(store.config, L"QuietBoost", L"AutoRestore", L"absent") == L"absent", L"配置不保存关闭保护选项");
    require(qb::readIni(store.config, L"QuietBoost", L"CurrentGain", L"absent") == L"absent", L"配置不保存当前增益");
    qb::atomicWrite(store.config, L"[QuietBoost]\r\nMaxGainPercent=5000\r\nAutoRestore=0\r\n"); loaded = store.loadSettings(); qb::GainControl gain(loaded); require(gain.automatic(), L"忽略配置内伪造的关闭保护字段");
    qb::Snapshot record{L"v", L"out", L"a", L"b", 123, 45678901234567}; store.saveSnapshot(record); auto restored = store.loadSnapshot();
    require(restored.processTime == record.processTime && restored.consoleId == L"a", L"设备恢复记录完整保存 64 位进程时间");
    qb::atomicWrite(store.config, L"[QuietBoost]\r\nMaxGainPercent=-2\r\n"); throws([&] { store.loadSettings(); }, L"无效配置禁止启动");
    qb::fs::remove(store.config); qb::fs::remove(store.journal); qb::fs::remove(directory);
}
void floatingLayout() {
    RECT work{-1920, -120, 0, 920};
    auto panel = qb::floatingBounds({-2200, -500, -1880, -304}, work, false, false);
    require(panel.left == -1920 && panel.top == -120 && panel.right == -1600, L"悬浮窗适配负坐标显示器并保持可见");
    panel = qb::floatingBounds({-20, 900, 300, 1096}, work, false, true);
    require(panel.right == 0 && panel.bottom == 920, L"悬浮窗不超出任务栏工作区");
    auto bar = qb::floatingBounds({-1000, 800, -894, 1160}, work, true, false);
    require(bar.left == -1920 && bar.bottom == 920 && bar.right - bar.left == 106, L"侧边条贴左边并约束拖动高度");
    bar = qb::floatingBounds({-1800, 20, -1694, 380}, work, true, true);
    require(bar.right == 0 && bar.top == 20, L"侧边条贴右边并保留高度");
    require(!qb::nearestRightEdge({-1800, 0, -1480, 196}, work) && qb::nearestRightEdge({-400, 0, -80, 196}, work), L"按当前显示器选择最近侧边");
    panel = qb::floatingBounds({-1800, -120, -1480, 76}, {0, 0, 1920, 1040}, false, false);
    require(panel.left == 0 && panel.top == 0, L"移除显示器后悬浮窗回到有效屏幕");
}
void floatingInteraction() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    auto directory = qb::fs::path(qb::executablePath()).parent_path() / L"test-state" / (L"floating-" + std::to_wstring(GetCurrentProcessId()));
    qb::StateStore store(directory); auto file = directory / L"window.ini";
    qb::atomicWrite(file, L"[FloatingWindow]\r\nVisible=0\r\nSidebar=0\r\nCollapsed=0\r\nX=-999999\r\nY=-999999\r\nCurrentGain=8000\r\nAutoRestore=0\r\n");
    struct Parent { HWND handle = CreateWindowExW(0, L"STATIC", L"QuietBoost UI test", WS_POPUP, 0, 0, 320, 196, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); ~Parent() { DestroyWindow(handle); } } parent;
    double requested = 0; unsigned resets = 0;
    qb::FloatingWindow floating(store);
    floating.create(parent.handle, 8000, false, [&](double value) { requested = value; }, [&] { ++resets; }, [] {});
    HWND slider = FindWindowExW(floating.handle(), nullptr, TRACKBAR_CLASSW, nullptr);
    require(!floating.visible() && slider && !IsWindowEnabled(slider), L"未转发时悬浮滑块禁用并恢复隐藏偏好");
    floating.update(650, true, true);
    require(IsWindowEnabled(slider) && requested == 0, L"主窗口同步增益不会触发悬浮窗反向调节");
    SendMessageW(slider, WM_KEYDOWN, VK_RIGHT, 0);
    require(requested > 650, L"悬浮窗向右键调高增益");
    floating.setSidebar(true);
    RECT rail{}; GetWindowRect(floating.handle(), &rail);
    require(floating.collapsed() && rail.right - rail.left == MulDiv(22, static_cast<int>(GetDpiForSystem()), 96), L"侧边条默认收成 22 DIP 窄入口");
    require(SendMessageW(floating.handle(), WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE && (GetWindowLongW(floating.handle(), GWL_EXSTYLE) & WS_EX_NOACTIVATE), L"课堂音量条不请求激活课件外窗口");
    floating.setCollapsed(false); slider = FindWindowExW(floating.handle(), nullptr, TRACKBAR_CLASSW, nullptr);
    requested = 0; SendMessageW(slider, WM_KEYDOWN, VK_UP, 0);
    require(requested > 650, L"侧边条向上键调高增益");
    floating.update(100, true, true);
    require(SendMessageW(slider, TBM_GETPOS, 0, 0) == 1000, L"自动恢复后侧边条同步到原始增益端");
    HWND reset = FindWindowExW(floating.handle(), nullptr, L"BUTTON", L"恢复 100%"); SendMessageW(reset, BM_CLICK, 0, 0);
    require(resets == 1, L"悬浮窗恢复按钮调用共用复位操作");
    require(floating.visible() && !IsWindowVisible(parent.handle), L"隐藏主窗口不会隐藏独立悬浮窗");
    floating.checkAutoCollapse(1000, true); floating.checkAutoCollapse(2000, false); floating.checkAutoCollapse(3199, false);
    require(!floating.collapsed(), L"移出不到 1.2 秒不收起");
    floating.checkAutoCollapse(3200, false); require(floating.collapsed(), L"移出 1.2 秒后自动收起");
    floating.setCollapsed(false); floating.checkAutoCollapse(4000, false); floating.checkAutoCollapse(6000, true);
    require(!floating.collapsed(), L"拖动滑块或指针回到面板时不自动收起");
    floating.show(false); floating.destroy();
    floating.create(parent.handle, 8000, false, [](double) {}, [] {}, [] {});
    require(floating.sidebar() && !floating.visible(), L"重新创建后保留侧边条形态和隐藏状态");
    require(qb::readIni(file, L"FloatingWindow", L"CurrentGain", L"absent") == L"absent" && qb::readIni(file, L"FloatingWindow", L"AutoRestore", L"absent") == L"absent", L"窗口布局文件不保留增益或关闭保护状态");
    floating.destroy();
    floating.create(parent.handle, 8000, false, [](double) {}, [] {}, [] {}, false, true);
    require(floating.visible() && floating.sidebar() && floating.collapsed(), L"登录自启覆盖旧隐藏偏好并只显示窄条");
    floating.destroy(); qb::fs::remove(file); qb::fs::remove(directory);
}
void loginStartup() {
    auto directory = qb::fs::path(qb::executablePath()).parent_path() / L"test-state" / (L"startup-" + std::to_wstring(GetCurrentProcessId()));
    qb::StateStore store(directory / L"data with 空格" / L"");
    qb::LoginStartup startup(store, directory / L"fake-startup");
    auto source = directory / L"download.exe"; qb::atomicWrite(source, L"Test bytes only; never executed.");
    require(!startup.enabled() && !qb::fs::exists(startup.linkPath()), L"没有勾选时不创建登录自启项");
    startup.setEnabled(true, source);
    require(startup.enabled() && qb::fs::exists(startup.executable()), L"自启创建指向稳定本地副本的快捷方式");
    qb::ComPtr<IShellLinkW> link; qb::check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), "Test startup shortcut");
    qb::ComPtr<IPersistFile> persisted; qb::check(link.As(&persisted), "Test shortcut persist"); qb::check(persisted->Load(startup.linkPath().c_str(), STGM_READ), "Test shortcut load");
    wchar_t arguments[32768]{}; qb::check(link->GetArguments(arguments, 32768), "Test shortcut arguments");
    int count = 0; auto parsed = CommandLineToArgvW((L"QuietBoost.exe " + std::wstring(arguments)).c_str(), &count);
    bool validArguments = parsed && count == 4 && std::wstring(parsed[1]) == L"--startup" && std::wstring(parsed[2]) == L"--state-dir" && qb::fs::path(parsed[3]) == store.root;
    LocalFree(parsed); require(validArguments, L"自启参数正确保留中文空格和尾部反斜杠路径");
    require(qb::fs::file_size(source) == qb::fs::file_size(startup.executable()), L"自启程序副本保留原始文件字节长度");
    qb::fs::remove(source); require(startup.enabled(), L"删除下载副本不影响自启快捷方式目标");
    startup.setEnabled(true, startup.executable()); require(startup.enabled(), L"从受管副本开启自启不会覆盖自身");
    startup.setEnabled(false); require(!startup.enabled() && !qb::fs::exists(startup.linkPath()), L"取消自启移除本工具快捷方式");
    startup.setEnabled(false); require(!startup.enabled(), L"重复关闭自启无需残留注册项");
    qb::atomicWrite(startup.linkPath(), L"Unrelated file");
    throws([&] { startup.setEnabled(true, startup.executable()); }, L"自启不覆盖同名无关文件");
    throws([&] { startup.setEnabled(false); }, L"关闭自启不删除同名无关文件");
    qb::fs::remove(startup.linkPath()); qb::fs::remove(startup.executable()); qb::fs::remove(startup.executable().parent_path());
    qb::fs::remove(startup.linkPath().parent_path()); qb::fs::remove(store.root); qb::fs::remove(directory);
}
void numberRendering() {
    HDC dc = CreateCompatibleDC(nullptr); bool restored = true;
    for (int dpi : {96, 144, 192}) {
        HFONT font = CreateFontW(-MulDiv(28, dpi, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ initial = GetCurrentObject(dc, OBJ_FONT); bool fits = true;
        RECT bounds{0, 0, MulDiv(80, dpi, 96), MulDiv(36, dpi, 96)};
        for (const wchar_t* value : {L"100%", L"650%", L"1000%", L"2000%", L"9999%", L"10000%", L"1000000%"}) {
            auto extent = qb::drawFittedNumber(dc, font, value, bounds, DT_CENTER);
            fits &= extent.cx > 0 && extent.cy > 0 && extent.cx <= bounds.right && extent.cy <= bounds.bottom;
            restored &= GetCurrentObject(dc, OBJ_FONT) == initial;
        }
        DeleteObject(font);
        require(fits, (L"完整百分比在侧边条范围内显示，DPI=" + std::to_wstring(dpi)).c_str());
    }
    DeleteDC(dc); require(restored, L"数字自适应绘制后恢复原 GDI 字体对象");
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool headless = std::wstring(GetCommandLineW()).find(L"--headless") != std::wstring::npos;
    auto reportPath = qb::fs::path(qb::executablePath()).parent_path() / L"test-results.txt";
    try {
        qb::ComApartment apartment; silence(); dsp(); ring(); recovery(); config(); floatingLayout(); floatingInteraction(); loginStartup(); numberRendering();
        report += L"\r\n" + std::to_wstring(passed) + L" checks passed.\r\n"; qb::atomicWrite(reportPath, report);
        if (!headless) MessageBoxW(nullptr, (std::to_wstring(passed) + L" 项检查通过。\n结果已保存到：\n" + reportPath.wstring()).c_str(), L"QuietBoost 测试", MB_OK | MB_ICONINFORMATION);
        return 0;
    } catch (const std::exception& e) {
        report += L"FAIL: " + qb::wide(e.what()); try { qb::atomicWrite(reportPath, report); } catch (...) {}
        if (!headless) MessageBoxW(nullptr, report.c_str(), L"QuietBoost 测试失败", MB_OK | MB_ICONERROR); return 1;
    }
}
