// gui.cpp —— svpn 的 Win32 原生图形界面
//
// 设计约束：
//   1. 不引入任何第三方库（保持单 exe、零 DLL 依赖）；只用 user32/gdi32/comctl32
//   2. 所有操作都走 dispatch_cli()，与命令行完全同一条代码路径，
//      绝不给 GUI 另写一套逻辑，避免两个入口行为漂移
//   3. 长任务放工作线程，主线程只画界面；输出用自定义 streambuf 逐行回传
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "svpn.hpp"

namespace {

// ---------------------------------------------------------------- UTF
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// ---------------------------------------------------------------- 控件 ID
enum : int {
    ID_BTN_START = 1001,
    ID_BTN_STOP,
    ID_BTN_REFRESH,
    ID_BTN_PROBE,
    ID_BTN_CDN,
    ID_BTN_SYS,
    ID_BTN_INIT,
    ID_BTN_DIR,
    ID_BTN_LOG,
    ID_BTN_CLEARLOG,
    ID_STATUS,
    ID_LOG,
    ID_BOTTOM,
};

constexpr UINT WM_APP_LOG = WM_APP + 1;      // lParam = new std::string
constexpr UINT WM_APP_DONE = WM_APP + 2;     // wParam = exit code
constexpr wchar_t kClass[] = L"SvpnGuiWindow";
constexpr wchar_t kTitle[] = L"svpn 1.1.1  ·  Steam 分流/加速";

HWND g_main = nullptr;
HWND g_btn[10]{};
HWND g_status = nullptr;
HWND g_log = nullptr;
HWND g_bottom = nullptr;
HFONT g_font_ui = nullptr;
HFONT g_font_mono = nullptr;
std::atomic<bool> g_busy{false};
UINT g_dpi = 96;
std::atomic<bool> g_closing{false};

// ---------------------------------------------------------------- 输出捕获
// 把 std::cout/std::cerr 的每一行投递到 UI 线程，避免工作线程直接碰控件
class LogBuf : public std::streambuf {
public:
    explicit LogBuf(HWND target) : target_(target) {}

protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return c;
        const char ch = static_cast<char>(c);
        if (ch == '\n') {
            flush_line();
        } else if (ch != '\r') {
            line_.push_back(ch);
        }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i) overflow(static_cast<unsigned char>(s[i]));
        return n;
    }

private:
    void flush_line() {
        if (line_.empty()) return;
        if (!g_closing.load() && target_ != nullptr) {
            auto* payload = new std::string(std::move(line_));
            PostMessageW(target_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(payload));
        }
        line_.clear();
    }
    HWND target_;
    std::string line_;
};

// 按 DPI 缩放：150% 屏上若写死像素，字会偏小、间距会挤
int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

// ---------------------------------------------------------------- 控件辅助
HWND make_button(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
    return h;
}

void set_busy(bool busy, const wchar_t* hint) {
    g_busy.store(busy);
    for (HWND h : g_btn) {
        if (h != nullptr) EnableWindow(h, busy ? FALSE : TRUE);
    }
    if (g_bottom != nullptr && hint != nullptr) SetWindowTextW(g_bottom, hint);
    if (busy) {
        // 执行期间只留「停止」可用，防止用户以为界面卡死后反复点
        EnableWindow(g_btn[1], TRUE);
    }
}

void append_log(const std::string& utf8) {
    if (g_log == nullptr) return;
    std::wstring w = widen(utf8);
    w += L"\r\n";   // 多行 EDIT 必须 CRLF，单独 \n 不换行
    const int len = GetWindowTextLengthW(g_log);
    // 日志过长时截掉前半，避免控件越用越慢
    if (len > 400000) {
        SendMessageW(g_log, EM_SETSEL, 0, 160000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(g_log, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
}

std::string capture(const std::vector<std::string>& args) {
    std::ostringstream oss;
    std::streambuf* old_out = std::cout.rdbuf(oss.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(oss.rdbuf());
    svpn::dispatch_cli(args);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return oss.str();
}

void refresh_status() {
    std::string out = capture({"svpn.exe", "status"});
    // 首行是版本横幅，状态面板里不需要
    const std::size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(nl + 1);
    std::wstring w = widen(out);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    // EDIT 控件只认 CRLF：直接把 \n 塞进去会显示成一整行（实测踩过）
    std::wstring crlf;
    crlf.reserve(w.size() + 8);
    for (wchar_t ch : w) {
        if (ch == L'\n') crlf += L'\r';
        crlf += ch;
    }
    SetWindowTextW(g_status, crlf.c_str());

    // 系统代理按钮按当前注册表状态显示
    svpn::WininetState st = svpn::read_wininet();
    SetWindowTextW(g_btn[5], st.enable ? L"系统代理：开" : L"系统代理：关");
}

// ---------------------------------------------------------------- 动作
void run_async(std::vector<std::string> args, const wchar_t* hint) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    set_busy(true, hint);

    const HWND target = g_main;
    std::thread([args = std::move(args), target] {
        LogBuf buf(target);
        std::streambuf* old_out = std::cout.rdbuf(&buf);
        std::streambuf* old_err = std::cerr.rdbuf(&buf);
        int rc = 1;
        try {
            rc = svpn::dispatch_cli(args);
        } catch (...) {
            std::cout << "[!] 执行时发生异常\n";
        }
        std::cout.flush();
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        if (!g_closing.load() && target != nullptr) {
            PostMessageW(target, WM_APP_DONE, static_cast<WPARAM>(rc), 0);
        }
    }).detach();
}

void open_in_explorer(const std::wstring& path, bool select_file) {
    std::wstring arg = select_file ? L"/select,\"" + path + L"\"" : L"\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring path(buf);
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

// ---------------------------------------------------------------- 布局
void layout(int w, int h) {
    const int pad = S(12);
    const int gap = S(8);
    const int btn_h = S(32);
    // status 实际输出 9 行（进程/两个端口/节点/模式/系统代理/CDN 缓存/hosts/上次统计），
    // 原值 S(96) 只够 6 行，把「CDN缓存」和「hosts」挤到滚动区外——这两行恰恰是判断
    // 下载加速有没有生效的依据，所以按 10 行留高。
    const int status_h = S(168);
    const int bottom_h = S(24);

    const int row1_w[] = {S(110), S(90), S(100)};
    const int row2_w[] = {S(110), S(110), S(130), S(110), S(100), S(100), S(80)};

    int y = pad;
    int x = pad;
    for (int i = 0; i < 3; ++i) {
        MoveWindow(g_btn[i], x, y, row1_w[i], btn_h, TRUE);
        x += row1_w[i] + gap;
    }
    y += btn_h + gap;

    // 第二行随宽度自动折行：窗口缩到 720 时 7 个按钮排不下，硬排会被裁掉
    x = pad;
    for (int i = 0; i < 7; ++i) {
        const int bw = row2_w[i];
        if (x > pad && x + bw > w - pad) {
            x = pad;
            y += btn_h + gap;
        }
        MoveWindow(g_btn[3 + i], x, y, bw, btn_h, TRUE);
        x += bw + gap;
    }
    y += btn_h + gap + S(4);

    MoveWindow(g_status, pad, y, w - pad * 2, status_h, TRUE);
    y += status_h + gap;

    int log_h = h - y - bottom_h - pad;
    if (log_h < S(80)) log_h = S(80);
    MoveWindow(g_log, pad, y, w - pad * 2, log_h, TRUE);

    MoveWindow(g_bottom, 0, h - bottom_h, w, bottom_h, TRUE);
}

// ---------------------------------------------------------------- 窗口过程
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_btn[0] = make_button(hwnd, ID_BTN_START, L"启动代理");
            g_btn[1] = make_button(hwnd, ID_BTN_STOP, L"停止");
            g_btn[2] = make_button(hwnd, ID_BTN_REFRESH, L"刷新状态");
            g_btn[3] = make_button(hwnd, ID_BTN_PROBE, L"端点探测");
            g_btn[4] = make_button(hwnd, ID_BTN_CDN, L"CDN 选边");
            g_btn[5] = make_button(hwnd, ID_BTN_SYS, L"系统代理：关");
            g_btn[6] = make_button(hwnd, ID_BTN_INIT, L"初始化配置");
            g_btn[7] = make_button(hwnd, ID_BTN_DIR, L"打开目录");
            g_btn[8] = make_button(hwnd, ID_BTN_LOG, L"打开日志");
            g_btn[9] = make_button(hwnd, ID_BTN_CLEARLOG, L"清空日志");

            g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                           ES_AUTOVSCROLL | WS_VSCROLL,
                                       0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)),
                                       nullptr, nullptr);
            SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_mono), TRUE);

            g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                                    0, 0, 0, 0, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LOG)), nullptr,
                                    nullptr);
            SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_mono), TRUE);

            g_bottom = CreateWindowExW(0, L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BOTTOM)),
                                       nullptr, nullptr);
            SendMessageW(g_bottom, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);

            append_log("svpn 图形界面已就绪。点「启动代理」开始，或先用「刷新状态」查看当前状态。");
            refresh_status();
            return 0;
        }

        case WM_SIZE: {
            layout(LOWORD(lp), HIWORD(lp));
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = S(720);
            mmi->ptMinTrackSize.y = S(520);
            return 0;
        }

        case WM_APP_LOG: {
            auto* payload = reinterpret_cast<std::string*>(lp);
            if (payload != nullptr) {
                append_log(*payload);
                delete payload;
            }
            return 0;
        }

        case WM_APP_DONE: {
            const int rc = static_cast<int>(wp);
            set_busy(false, rc == 0 ? L"完成" : L"执行结束（有错误，见上方日志）");
            if (rc == 0) {
                SetWindowTextW(g_bottom, L"完成");
            } else {
                SetWindowTextW(g_bottom, (L"执行结束，退出码 " + std::to_wstring(rc)).c_str());
            }
            refresh_status();
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            switch (id) {
                case ID_BTN_START:
                    append_log("[i] 启动代理…");
                    run_async({"svpn.exe", "up", "-b"}, L"正在启动代理…");
                    break;
                case ID_BTN_STOP:
                    // run_async 在忙碌时直接 return，所以这里必须先打住并给话，
                    // 否则「停止」在探测/选边跑着的时候是个静默死键。
                    if (g_busy.load()) {
                        append_log("[!] 上一个任务还在执行，本次「停止」未提交；等底部显示「完成」后再点。");
                        SetWindowTextW(g_bottom, L"任务执行中，稍后再停止");
                        return 0;
                    }
                    append_log("[i] 停止代理…");
                    run_async({"svpn.exe", "down"}, L"正在停止…");
                    break;
                case ID_BTN_REFRESH:
                    refresh_status();
                    SetWindowTextW(g_bottom, L"状态已刷新");
                    break;
                case ID_BTN_PROBE:
                    append_log("[i] 开始端点探测（9 个域名，约需 5–15 秒）…");
                    run_async({"svpn.exe", "probe"}, L"正在探测端点…");
                    break;
                case ID_BTN_CDN:
                    append_log("[i] CDN 选边：解析 → 握手淘汰 → 实测限速，可能需要 20 秒以上…");
                    run_async({"svpn.exe", "cdn", "--refresh"}, L"正在选边…");
                    break;
                case ID_BTN_SYS: {
                    const bool on = svpn::read_wininet().enable;
                    append_log(on ? "[i] 关闭系统代理…" : "[i] 开启系统代理（指向本机入口）…");
                    run_async({"svpn.exe", "system", on ? "off" : "on"},
                              on ? L"正在关闭系统代理…" : L"正在开启系统代理…");
                    break;
                }
                case ID_BTN_INIT:
                    append_log("[i] 初始化配置（已存在时会拒绝覆盖）…");
                    run_async({"svpn.exe", "init"}, L"正在写配置…");
                    break;
                case ID_BTN_DIR:
                    open_in_explorer(exe_dir(), false);
                    break;
                case ID_BTN_LOG:
                    open_in_explorer(exe_dir() + L"\\runtime", false);
                    break;
                case ID_BTN_CLEARLOG:
                    SetWindowTextW(g_log, L"");
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == g_bottom) {
                SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
                SetTextColor(reinterpret_cast<HDC>(wp), RGB(60, 60, 60));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;

        case WM_DESTROY:
            g_closing.store(true);
            if (g_font_ui != nullptr) DeleteObject(g_font_ui);
            if (g_font_mono != nullptr) DeleteObject(g_font_mono);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace svpn {

int gui_main() {
    SetProcessDPIAware();

    {
        HDC screen = GetDC(nullptr);
        g_dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
        ReleaseDC(nullptr, screen);
        if (g_dpi == 0 || g_dpi > 480) g_dpi = 96;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (RegisterClassExW(&wc) == 0) return 1;

    const int ui_h = -MulDiv(10, static_cast<int>(g_dpi), 72);    // 10pt
    const int mono_h = -MulDiv(10, static_cast<int>(g_dpi), 72);
    g_font_ui = CreateFontW(ui_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_font_mono = CreateFontW(mono_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");

    const int w = S(960);
    const int h = S(680);
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, kClass, kTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                (sw - w) / 2, (sh - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (hwnd == nullptr) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace svpn
