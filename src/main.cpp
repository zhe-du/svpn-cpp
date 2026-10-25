// svpn (C++) —— 命令行入口与编排。
// 语义对齐 Python 版 svpn/cli.py；本文件只做「组装」，具体能力在各模块里。
#include "svpn.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svpn {
namespace {

constexpr const char* kVersion = "1.1.1";

// 与 Python probe.ENDPOINTS 完全一致
struct Endpoint {
    const char* host;
    int port;
    const char* note;
};
const Endpoint kEndpoints[] = {
    {"store.steampowered.com", 443, "商店 / 购买"},
    {"login.steampowered.com", 443, "登录"},
    {"api.steampowered.com", 443, "API / 令牌"},
    {"steamcommunity.com", 443, "社区 / 创意工坊"},
    {"community.akamai.steamstatic.com", 443, "社区静态资源"},
    {"avatars.steamstatic.com", 443, "头像 / 图片"},
    {"steamcdn-a.akamaihd.net", 443, "游戏下载 CDN"},
    {"cdn.akamai.steamstatic.com", 443, "客户端更新"},
    {"help.steampowered.com", 443, "客服 / 找回"},
};

constexpr const char* kCommonProxyPorts[] = {
    "7890", "7891", "7897", "1080", "1081", "10808", "10809", "10810",
    "8889", "8080", "8118", "9910", "20171", "20172", "2080", "2081",
};

// ---------------------------------------------------------------- 小工具
[[maybe_unused]] std::string to_u8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool port_open(const std::string& host, int port, int timeout_ms = 300) {
    if (port <= 0) return false;
    socket_t s = tcp_connect_ip(host.empty() ? "127.0.0.1" : host, port, timeout_ms);
    if (s == kBadSock) return false;
    close_socket(s);
    return true;
}

bool set_autoconfig_url(const std::string& url) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring w = to_w(url);
    LONG rc = RegSetValueExW(key, L"AutoConfigURL", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(w.c_str()),
                             static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::string tail_file(const std::filesystem::path& p, int lines) {
    std::ifstream in(p);
    if (!in) return {};
    std::vector<std::string> all;
    std::string line;
    while (std::getline(in, line)) all.push_back(line);
    if (all.empty()) return {};
    int from = static_cast<int>(all.size()) - lines;
    if (from < 0) from = 0;
    std::string out;
    for (int i = from; i < static_cast<int>(all.size()); ++i) {
        out += all[static_cast<std::size_t>(i)];
        out += "\n";
    }
    return out;
}

bool read_pid(const Paths& p, DWORD* out) {
    std::ifstream in(p.pid);
    if (!in) return false;
    long long v = 0;
    if (!(in >> v) || v <= 0) return false;
    *out = static_cast<DWORD>(v);
    return true;
}

bool pid_alive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

void write_pid(const Paths& p, DWORD pid) {
    p.ensure_runtime();
    std::ofstream out(p.pid, std::ios::trunc);
    out << pid;
}

// 只删「确实是自己的」那条记录：第二个实例启动失败时若无条件删，
// 会把正在运行实例的 pid 记录抹掉，之后 `down` 就找不到它了（端口永久被占）。
void remove_pid_owned(const Paths& p, DWORD self) {
    DWORD cur = 0;
    if (self != 0 && read_pid(p, &cur) && cur != self) return;
    std::error_code ec;
    std::filesystem::remove(p.pid, ec);
}

// 把 --node URL 解析进 cfg.upstream（up 与 _serve 共用，避免两处实现漂移）
bool apply_node_url(Config& cfg, const std::string& node) {
    if (node.empty()) return true;
    if (!starts_with(node, "socks5://") && !starts_with(node, "http://")) {
        std::cout << "[!] --node 只认 socks5:// 或 http://\n";
        return false;
    }
    std::string rest = node.substr(node.find("://") + 3);
    std::string auth;
    auto at = rest.find('@');
    if (at != std::string::npos) { auth = rest.substr(0, at); rest = rest.substr(at + 1); }
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) {
        std::cout << "[!] --node 需要 host:port\n";
        return false;
    }
    cfg.upstream.kind = starts_with(node, "http://") ? "http" : "socks5";
    cfg.upstream.host = rest.substr(0, colon);
    try {
        cfg.upstream.port = std::stoi(rest.substr(colon + 1));
    } catch (...) {
        std::cout << "[!] --node 端口非法\n";
        return false;
    }
    if (cfg.upstream.port < 1 || cfg.upstream.port > 65535) {
        std::cout << "[!] --node 端口越界\n";
        return false;
    }
    auto ac = auth.find(':');
    if (ac != std::string::npos) {
        cfg.upstream.user = auth.substr(0, ac);
        cfg.upstream.pass = auth.substr(ac + 1);
    }
    return true;
}

// ---------------------------------------------------------------- 参数
struct Args {
    std::vector<std::string> pos;
    std::string flag(const std::string& name, const std::string& def = "") const {
        for (std::size_t i = 0; i + 1 < pos.size(); ++i) {
            if (pos[i] == name) return pos[i + 1];
        }
        return def;
    }
    bool has(const std::string& name) const {
        return std::find(pos.begin(), pos.end(), name) != pos.end();
    }
    int num(const std::string& name, int def) const {
        std::string v = flag(name);
        if (v.empty()) return def;
        try {
            return std::stoi(v);
        } catch (...) {
            return def;
        }
    }
    // 秒数参数：接受小数，下限 1 秒。
    // 早先全部走 std::stoi，`--timeout 0.5` 被截成 0，0 再乘 1000 传进
    // SetSocketTimeout 等于 INFINITE —— 想「缩短超时」反而变成永不超时
    // （实测一次 probe 在 unreachable 端点上跑了 64 秒）。0.5 现在按 1 秒算。
    int secs(const std::string& name, int def) const {
        std::string v = flag(name);
        if (v.empty()) return def;
        try {
            double d = std::stod(v);
            if (!(d >= 1.0)) return 1;      // 含 NaN
            if (d > 3600.0) return 3600;
            return static_cast<int>(d + 0.5);
        } catch (...) {
            return def;
        }
    }
};

// ---------------------------------------------------------------- 输出
void print_route(const Config& cfg) {
    if (cfg.cdn.enabled) {
        std::cout << fmt("[i] CDN 模式: SNI 透明转发:%d（hosts 指向 127.0.0.1，需常驻）\n",
                         cfg.cdn.sni_port);
    } else {
        std::cout << "[i] CDN 模式: hosts 直指最优 IP\n";
    }
    if (cfg.upstream.configured()) {
        std::cout << fmt("[i] 上游: %s（%s）\n", cfg.upstream.host.c_str(),
                         cfg.mode.c_str());
    } else {
        std::cout << fmt("[i] 上游: 无（纯直连，%s）\n", cfg.mode.c_str());
    }
}

// ================================================================ init
int cmd_init(const Paths& p, const Args& a) {
    bool force = a.has("--force");
    if (std::filesystem::exists(p.config) && !force) {
        std::cout << "[=] config.json 已存在（要覆盖加 --force）\n";
        return 0;
    }
    Config cfg = default_config();
    if (!save_config(p, cfg)) {
        std::cout << "[!] 写 config.json 失败\n";
        return 1;
    }
    std::cout << fmt("[+] 已写入 %s\n", p.config.string().c_str());
    std::cout << fmt("[i] SOCKS5 %s:%d / HTTP %s:%d\n", cfg.listen_host.c_str(),
                     cfg.listen_port, cfg.listen_host.c_str(), cfg.http_port);
    return 0;
}

// ================================================================ up
// 前台模式要复用 _serve 的完整启动路径（SNI 转发 / 统计周期落盘 / hosts IP 映射 /
// pid 与 effective.json），只有日志去哪不同，所以 _serve 得能收一份已解析好的 Config。
int cmd_serve(const Paths& p, const Args& a, const Config* preset = nullptr);

int cmd_up(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    if (a.has("-p")) cfg.listen_port = a.num("-p", cfg.listen_port);
    if (a.has("--port")) cfg.listen_port = a.num("--port", cfg.listen_port);
    if (a.has("-m")) cfg.mode = a.flag("-m", cfg.mode);
    if (a.has("--mode")) cfg.mode = a.flag("--mode", cfg.mode);
    if (!apply_node_url(cfg, a.flag("--node"))) return 2;
    if (cfg.listen_port < 1 || cfg.listen_port > 65535) {
        std::cout << fmt("[!] 端口越界: %d\n", cfg.listen_port);
        return 2;
    }
    if (cfg.http_port != 0 && cfg.http_port == cfg.listen_port) {
        std::cout << fmt("[!] SOCKS5 与 HTTP 不能同端口：用 --port 17899，"
                         "或改 config.json 的 http_port\n");
        return 1;
    }
    if (cfg.cdn.enabled && (cfg.cdn.sni_port == cfg.listen_port ||
                            (cfg.http_port && cfg.cdn.sni_port == cfg.http_port))) {
        std::cout << "[!] cdn.sni_port 与本机代理端口冲突（sni_port 一般必须是 443）\n";
        return 1;
    }
    if (port_open(cfg.listen_host, cfg.listen_port) ||
        (cfg.http_port && port_open(cfg.listen_host, cfg.http_port))) {
        // 两个端口都要查：只查 SOCKS5 的话，`up -b` 会先"成功"再在 HTTP 端口上
        // bind 失败，还可能把正在运行实例的 pid 记录带走。
        DWORD pid = 0;
        if (read_pid(p, &pid) && pid_alive(pid)) {
            std::cout << fmt("[=] 已在运行（pid=%lu）；改配置请先 svpn down\n", pid);
            return 0;
        }
        // 端口被占、但没有本程序的 pid 记录 → 占着它的不是 svpn。
        // 早先这里不分情况一律报「已在运行」并返回 0，于是一个被别的程序占死的端口
        // 会让调用方以为「代理已经起来了」，然后连上去全部失败 —— 又一处假成功。
        const int busy = port_open(cfg.listen_host, cfg.listen_port) ? cfg.listen_port
                                                                    : cfg.http_port;
        std::cout << fmt("[!] 端口 %d 被其他程序占用（没有 svpn 的 pid 记录）\n", busy);
        std::cout << fmt("[!] 换个端口：up -p 17899    看谁占着：netstat -ano | findstr :%d\n",
                         busy);
        return 1;
    }

    if (!a.has("-b") && !a.has("--background")) {
        // 前台模式：直接走 _serve 的同一条启动路径。
        //
        // 早先这里另写了一段精简启动（只调 serve_all 就返回），后果是前台模式：
        //   · 不起 SNI 转发线程 —— 配了 cdn.enabled 且 hosts apply 之后，客户端连的是
        //     127.0.0.1:443，前台跑等于没有人在听，下载静默全断；
        //   · 不起统计周期落盘线程 —— `status` 的「上次统计」行永远不更新；
        //   · 不做 apply_ip_map —— hosts 里的边缘 IP 不会被刷新；
        //   · 不写 pid / effective.json —— `down` 与 `status` 都看不见这个实例；
        //   · `up --system-proxy` 前台运行时根本没接管系统代理（又一处假成功）。
        // 前台与后台只能是「日志去哪、进程要不要脱离控制台」的差别，不能是两套行为。
        std::cout << fmt("[i] 前台运行 %s:%d（Ctrl+C 退出）\n", cfg.listen_host.c_str(),
                         cfg.listen_port);
        return cmd_serve(p, a, &cfg);
    }

    // 后台：以 _serve 重启自己
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" _serve --log-to-file";
    if (a.has("--system-proxy")) cmd += L" --system-proxy";
    // 子进程只读 config.json，所以 up 的命令行覆盖必须显式传下去，
    // 否则 `up -b -p 17899` 会"成功"但实际还监听旧端口。
    cmd += L" --listen-port " + std::to_wstring(cfg.listen_port);
    cmd += L" --mode " + to_w(cfg.mode);
    if (cfg.upstream.configured()) {
        std::wstring url = to_w(Node{cfg.upstream.kind, cfg.upstream.host, cfg.upstream.port,
                                     cfg.upstream.user, cfg.upstream.pass}.url());
        cmd += L" --node " + url;
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    p.ensure_runtime();
    // 关键：bInheritHandles=FALSE。句柄一律不传，子进程自己开日志文件，
    // 否则子进程会继承父进程的 stdout 管道，`up -b | tail` 这种用法永远等不到 EOF。
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, flags, nullptr,
                             p.root.wstring().c_str(), &si, &pi);
    if (!ok) {
        std::cout << fmt("[!] 启动失败: %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);

    bool ready = false;
    for (int i = 0; i < 25; ++i) {
        Sleep(120);
        DWORD code = 0;
        if (!GetExitCodeProcess(pi.hProcess, &code) || code != STILL_ACTIVE) break;
        if (port_open(cfg.listen_host, cfg.listen_port)) { ready = true; break; }
    }
    if (!ready) {
        std::string tail = tail_file(p.log, 8);
        std::cout << "[!] 后台启动失败";
        if (tail.empty()) {
            std::cout << "（日志为空：多半是端口已被占用，先 svpn down 或换 --port）\n";
        } else {
            std::cout << "，日志尾部：\n" << tail;
        }
        CloseHandle(pi.hProcess);
        return 1;
    }
    write_pid(p, pi.dwProcessId);
    CloseHandle(pi.hProcess);
    std::cout << fmt("[+] 后台已启动 pid=%lu  socks5://%s:%d  http://%s:%d\n",
                     pi.dwProcessId, cfg.listen_host.c_str(), cfg.listen_port,
                     cfg.listen_host.c_str(), cfg.http_port);
    print_route(cfg);
    return 0;
}

// ================================================================ _serve
std::atomic<bool> g_stop{false};
BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

int cmd_serve(const Paths& p, const Args& a, const Config* preset) {
    // 前台模式由 cmd_up 传进来已经处理好覆盖项的 Config；后台自启时子进程看不到
    // 父进程 argv，所以只能重新读 config.json 再靠 --listen-port/--mode/--node 覆盖。
    Config cfg = preset ? *preset : load_config(p);
    // 接收 up 透传的覆盖项（子进程看不到父进程的 argv）
    if (a.has("--listen-port")) cfg.listen_port = a.num("--listen-port", cfg.listen_port);
    if (a.has("--mode")) cfg.mode = a.flag("--mode", cfg.mode);
    if (!apply_node_url(cfg, a.flag("--node"))) return 2;
    p.ensure_runtime();
    if (a.has("--log-to-file")) {
        // 后台自启时父进程不传任何句柄（bInheritHandles=FALSE）：
        // 一来不会把父进程的 stdout 管道带进常驻进程（`up -b | tee` 会挂住），
        // 二来关闭控制台也不会把服务带走。
        FILE* fp = nullptr;
        _wfreopen_s(&fp, p.log.wstring().c_str(), L"a", stdout);
        _wfreopen_s(&fp, p.log.wstring().c_str(), L"a", stderr);
        // 重定向到文件后 stdout 默认全缓冲，常驻进程不退出就永远看不到日志
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    apply_ip_map(p, cfg, cfg.cdn.refresh_hours * 3600.0);
    if (a.has("--system-proxy")) {
        // 之前这里只是把参数传到子进程，谁也没去改 WinINET —— `up --system-proxy`
        // 实际什么都没做。现在真接管：先备份，再指向本机 SOCKS5 入口。
        backup_wininet(p);
        WininetState w = apply_wininet(fmt("127.0.0.1:%d", cfg.listen_port),
                                       cfg.browser.bypass);
        if (w.ok) {
            std::cout << fmt("[+] 系统代理已指向 127.0.0.1:%d\n", cfg.listen_port);
        } else {
            std::cout << fmt("[!] 系统代理设置失败: %s\n", w.reason.c_str());
        }
    }
    if (a.has("--log-to-file")) {
        // stderr 已经指向日志文件，就别再让 log_write 每行重开一次文件
        // （两个流抢同一个文件会互相锁住，外部 tail 直接 "resource busy"）。
        Paths lp = p;
        lp.log.clear();
        log_init(lp, true);
    } else {
        log_init(p, true);
    }
    p.ensure_runtime();
    write_pid(p, GetCurrentProcessId());
    {
        // up -b -p 的覆盖只存在于命令行，config.json 里还是旧值；
        // 写一份 effective.json，`status`/`down` 才不会按旧端口去判断。
        Json ej = Json::make_obj();
        ej.set("listen_host", Json::make_str(cfg.listen_host));
        ej.set("listen_port", Json::make_num(cfg.listen_port));
        ej.set("http_port", Json::make_num(cfg.http_port));
        ej.set("mode", Json::make_str(cfg.mode));
        ej.set("upstream_configured", Json::make_bool(cfg.upstream.configured()));
        if (cfg.upstream.configured()) {
            ej.set("upstream", Json::make_str(fmt("%s:%d", cfg.upstream.host.c_str(),
                                                  cfg.upstream.port)));
            // kind 也必须落盘：`--node http://...` 只改了内存里的 cfg，config.json 仍是旧
            // kind，status 读回后会把「节点」行显示成 socks5（链路实际走 HTTP = 假信息）
            ej.set("upstream_kind", Json::make_str(cfg.upstream.kind));
        }
        std::ofstream ef(p.runtime / "effective.json", std::ios::trunc);
        ef << ej.dump(2);
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    Stats st;
    st.started_ms = now_ms();
    std::cout << fmt("[i] svpn %s 已启动 socks5://%s:%d http://%s:%d\n", kVersion,
                     cfg.listen_host.c_str(), cfg.listen_port, cfg.listen_host.c_str(),
                     cfg.http_port);
    print_route(cfg);

    std::atomic<bool> sni_stop{false};
    std::thread sni_thread;
    if (cfg.cdn.enabled) {
        sni_thread = std::thread([&]() {
            int rc = serve_sni(cfg, st, g_stop);
            if (rc != 0) log_write("WARN", "SNI 转发未启动（端口被占用？）");
        });
    }

    std::atomic<bool> dump_stop{false};
    std::thread dumper([&]() {
        while (!dump_stop) {
            for (int i = 0; i < 50; ++i) {
                if (dump_stop) return;
                Sleep(100);
            }
            st.dump(p);
        }
    });

    int rc = serve_all(cfg, st, g_stop);
    dump_stop = true;
    if (dumper.joinable()) dumper.join();
    if (sni_thread.joinable()) sni_thread.join();
    st.dump(p);
    remove_pid_owned(p, GetCurrentProcessId());
    std::cout << "[i] 已停止\n";
    return rc;
}

// ================================================================ down
int cmd_down(const Paths& p, const Args& a) {
    DWORD pid = 0;
    if (read_pid(p, &pid) && pid_alive(pid)) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
            std::cout << fmt("[+] 已停止 pid=%lu\n", pid);
        } else {
            std::cout << fmt("[!] 无法结束 pid=%lu（权限不足？）\n", pid);
        }
    } else {
        std::cout << "[=] 没有在运行的实例\n";
        Config cfg = load_config(p);
        if (port_open(cfg.listen_host, cfg.listen_port)) {
            std::cout << fmt("[!] 但端口 %d 仍被占用（可能是上次异常退出的残留）\n",
                             cfg.listen_port);
        }
    }
    remove_pid_owned(p, 0);
    {
        std::error_code ec;
        std::filesystem::remove(p.runtime / "effective.json", ec);
    }
    if (!a.has("--keep-system")) {
        WininetState before = read_wininet();
        WininetState res = restore_wininet(p);
        if (!res.ok && res.reason == "no backup") {
            std::cout << "[!] 系统代理没动过：没有备份（可能从没接管过）\n";
            std::cout << fmt("    当前 ProxyEnable=%d %s\n", before.enable ? 1 : 0,
                             before.server.c_str());
        } else {
            std::cout << "[i] 系统代理已还原\n";
        }
    }
    if (a.has("--clear-hosts")) {
        if (hosts_managed_entries(p) > 0) {
            HostsResult r = clear_hosts(p);
            if (r.ok) {
                std::cout << "[i] hosts 已清理\n";
            } else if (is_admin()) {
                std::cout << fmt("[!] hosts 清理失败: %s\n", r.reason.c_str());
            } else {
                std::cout << "[i] 需要管理员权限，正在请求提权…\n";
                elevate("hosts clear --no-elevate");
            }
        } else {
            std::cout << "[=] hosts 里没有 svpn 记录，跳过\n";
        }
    }
    return 0;
}

// ================================================================ status
int cmd_status(const Paths& p, const Args&) {
    Config cfg = load_config(p);
    std::cout << fmt("svpn %s  ·  Steam 分流/加速\n", kVersion);
    // 运行中的实例可能用 CLI 覆盖过端口，config.json 里不是真值
    {
        std::ifstream ef(p.runtime / "effective.json");
        if (ef) {
            std::string text((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
            Json j = Json::parse(text);
            if (const Json* v = j.get("listen_port")) cfg.listen_port = static_cast<int>(v->as_num(cfg.listen_port));
            if (const Json* v = j.get("http_port")) cfg.http_port = static_cast<int>(v->as_num(cfg.http_port));
            if (const Json* v = j.get("mode")) cfg.mode = v->as_str(cfg.mode);
            // 节点也要读：`up -b --node socks5://...` 的覆盖没写进 config.json，
            // 否则 status 的「节点」那行会一直显示旧值（看着像没生效）
            if (const Json* v = j.get("upstream_configured"); v != nullptr && v->as_bool(false)) {
                if (const Json* u = j.get("upstream")) {
                    std::string spec = u->as_str();
                    auto colon = spec.rfind(':');
                    if (colon != std::string::npos) {
                        cfg.upstream.host = spec.substr(0, colon);
                        try {
                            cfg.upstream.port = std::stoi(spec.substr(colon + 1));
                        } catch (...) {
                        }
                    }
                }
                // 1.1.0 及更早的 effective.json 没有这个键：缺省就保持 config.json 的
                // kind（不清空、不报错），只有真读到值才覆盖。
                if (const Json* k = j.get("upstream_kind")) {
                    std::string kind = k->as_str();
                    if (!kind.empty()) cfg.upstream.kind = kind;
                }
            } else if (j.has("upstream_configured")) {
                cfg.upstream.host.clear();
                cfg.upstream.port = 0;
            }
        }
    }
    DWORD pid = 0;
    bool running = read_pid(p, &pid) && pid_alive(pid);
    std::cout << (running ? fmt("进程     运行中 pid=%lu\n", pid) : std::string("进程     未运行\n"));
    std::cout << fmt("端口     %s:%d  %s\n", cfg.listen_host.c_str(), cfg.listen_port,
                     port_open(cfg.listen_host, cfg.listen_port) ? "监听中" : "未监听");
    if (cfg.http_port) {
        std::cout << fmt("端口     %s:%d  %s\n", cfg.listen_host.c_str(), cfg.http_port,
                         port_open(cfg.listen_host, cfg.http_port) ? "监听中" : "未监听");
    }
    std::cout << (cfg.upstream.configured()
                      ? fmt("节点     %s:%d（%s）\n", cfg.upstream.host.c_str(),
                            cfg.upstream.port, cfg.upstream.kind.c_str())
                      : std::string("节点     未配置（纯直连）\n"));
    std::cout << fmt("模式     %s\n", cfg.mode.c_str());
    WininetState w = read_wininet();
    std::cout << fmt("系统代理 ProxyEnable=%d %s\n", w.enable ? 1 : 0, w.server.c_str());
    double age = cache_age_hours(p);
    Cache cache = load_cache(p, 1e18);
    if (age < 0) {
        std::cout << "CDN缓存  无\n";
    } else {
        std::cout << fmt("CDN缓存  %zu 个域名，%.0f 分钟前更新%s\n", cache.size(), age * 60,
                         age * 3600.0 > cfg.cdn.refresh_hours * 3600.0 ? "（已过期）" : "");
    }
    int managed = hosts_managed_entries(p);
    std::cout << (managed ? fmt("hosts    %d 条 svpn 记录\n", managed)
                          : std::string("hosts    无 svpn 记录\n"));
    std::ifstream in(p.stats);
    if (in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Json j = Json::parse(text);
        if (const Json* c = j.get("connections")) {
            std::cout << fmt("上次统计 %.0f 连接 / 下行 %.2fMB / 错误 %.0f\n",
                             c->as_num(0), j.get("down_mb") ? j.get("down_mb")->as_num(0) : 0.0,
                             j.get("errors") ? j.get("errors")->as_num(0) : 0.0);
        }
    }
    return 0;
}

// ================================================================ probe
struct ProbeRow {
    std::string host, note, verdict;
    double ms{0};
    bool direct_ok{false}, node_ok{false}, usable{false};
};

bool tls_ok_via(socket_t sock, const std::string& host, int timeout_ms, double* ms, std::string* err) {
    if (sock == kBadSock) return false;
    std::int64_t t0 = now_ms();
    TlsConn c = tls_connect(sock, host, timeout_ms, err);
    bool ok = c.ok;
    if (ok) {
        c.close();
    } else {
        close_socket(sock);
    }
    if (ms) *ms = static_cast<double>(now_ms() - t0);
    return ok;
}

ProbeRow probe_one(const Config& cfg, const Endpoint& ep, int timeout_ms) {
    ProbeRow row;
    row.host = ep.host;
    row.note = ep.note;
    const int ms = timeout_ms * 1000;
    std::string err;
    double t = 0;
    socket_t s = dial_direct(ep.host, ep.port, ms, &err);
    row.direct_ok = tls_ok_via(s, ep.host, ms, &t, &err);
    row.ms = t;
    if (cfg.upstream.configured()) {
        double t2 = 0;
        socket_t n = dial_via(Node{cfg.upstream.kind, cfg.upstream.host, cfg.upstream.port,
                                   cfg.upstream.user, cfg.upstream.pass},
                              ep.host, ep.port, ms, &err);
        row.node_ok = tls_ok_via(n, ep.host, ms, &t2, &err);
    }
    if (row.direct_ok) {
        row.verdict = "直连可用";
        row.usable = true;
    } else if (row.node_ok) {
        row.verdict = "需节点 · 可用";
        row.usable = true;
    } else if (!cfg.upstream.configured()) {
        // 没节点时，直连不通就是被阻断：文案与 Python 版一致
        row.verdict = "需节点 · SNI 阻断";
    } else {
        row.verdict = "不通 · 待排查";
    }
    return row;
}

int cmd_probe(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    // --json 的契约是「stdout 只有 JSON」：人读的表格与提示全部改走 stderr，
    // 否则 `svpn probe --json | jq` 直接报 Expecting value。
    const bool json_only = a.has("--json");
    std::ostream& human = json_only ? std::cerr : std::cout;
    int timeout = a.secs("--timeout", 6);
    std::string node_arg = a.flag("--node");
    if (node_arg == "none") {
        cfg.upstream.host.clear();
    } else if (!node_arg.empty()) {
        // `--node socks5://host:port` = 临时试这个节点，不落盘（Python 版同语义）
        if (!apply_node_url(cfg, node_arg)) return 2;
    }
    human << fmt("[i] 节点: %s\n", cfg.upstream.configured()
                                          ? (cfg.upstream.host + ":" + std::to_string(cfg.upstream.port)).c_str()
                                          : "无（只看直连）");
    std::vector<ProbeRow> rows;
    rows.reserve(std::size(kEndpoints));
    std::vector<std::thread> workers;
    std::mutex mu;
    for (const auto& ep : kEndpoints) {
        workers.emplace_back([&, ep]() {
            ProbeRow r = probe_one(cfg, ep, timeout);
            std::lock_guard<std::mutex> lk(mu);
            rows.push_back(std::move(r));
        });
    }
    for (auto& t : workers) t.join();

    // 结果列要放得下「需节点 · SNI 阻断」（CJK 按 2 列宽），14 太窄会把表格挤歪
    human << fmt("%-38s %-20s %-22s %8s\n", "域名", "结果", "备注", "耗时");
    human << std::string(92, '-') << "\n";
    int direct_ok = 0, need_node = 0, dead = 0, unusable = 0;
    for (const auto& r : rows) {
        human << fmt("%-38s %-20s %-22s %7.0fms\n", r.host.c_str(), r.verdict.c_str(),
                     r.note.c_str(), r.ms);
        // 三个桶互斥：直连可用 / 需要节点 / 真死（有节点也不通）
        // 没配节点时，直连不通只能算「需要节点」——之前把它算成「不通」，
        // 于是同一行既出现在「需节点」又出现在「不通」，还让 --lenient 失去意义。
        if (r.direct_ok) {
            ++direct_ok;
        } else if (r.node_ok || !cfg.upstream.configured()) {
            ++need_node;
            ++unusable;          // 当前确实不可用（用户还没配节点）
        } else {
            ++dead;
            ++unusable;
        }
    }
    human << fmt("\n[i] 直连可用 %d / 需节点 %d / 死端点 %d / 当前不可用 %d\n", direct_ok,
                 need_node, dead, unusable);

    Json summary = Json::make_obj();
    summary.set("ok", Json::make_bool(unusable == 0));
    Json arr = Json::make_arr();
    for (const auto& r : rows) {
        Json o = Json::make_obj();
        o.set("host", Json::make_str(r.host));
        o.set("verdict", Json::make_str(r.verdict));
        o.set("ms", Json::make_num(r.ms));
        o.set("usable", Json::make_bool(r.usable));
        arr.push(std::move(o));
    }
    summary.set("rows", std::move(arr));
    summary.set("unusable", Json::make_num(unusable));
    p.ensure_runtime();
    if (!p.probe.empty()) {
        std::ofstream out(p.probe, std::ios::trunc);
        out << summary.dump(2);
    }
    if (json_only) {
        std::cout << summary.dump(2) << "\n";
    } else {
        std::cout << "退出码 0 = 全部端点可用；1 = 有端点不可用（见 summary.unusable）\n";
    }
    if (a.has("--lenient")) return dead ? 1 : 0;
    return unusable ? 1 : 0;
}

// ================================================================ node
int cmd_node(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    // pos[0]=exe, pos[1]="node", 子命令在 pos[2]
    std::string action = a.pos.size() > 2 ? a.pos[2] : "scan";
    if (action == "clear") {
        cfg.upstream = UpstreamCfg{};
        cfg.upstream.host = "NODE_HOST";
        save_config(p, cfg);
        std::cout << "[+] 已清空节点（回到纯直连分流）\n";
        return 0;
    }
    if (action == "set") {
        std::string url = a.pos.size() > 3 ? a.pos[3] : "";
        if (url.empty()) {
            std::cout << "[!] 用法: svpn node set socks5://user:pass@host:port\n";
            return 2;
        }
        if (!starts_with(url, "socks5://") && !starts_with(url, "http://")) {
            std::cout << "[!] 只支持 socks5:// 或 http://\n";
            return 2;
        }
        std::string rest = url.substr(url.find("://") + 3);
        std::string auth;
        auto at = rest.find('@');
        if (at != std::string::npos) { auth = rest.substr(0, at); rest = rest.substr(at + 1); }
        auto colon = rest.rfind(':');
        if (colon == std::string::npos) {
            std::cout << "[!] 缺少端口\n";
            return 2;
        }
        cfg.upstream.kind = starts_with(url, "http://") ? "http" : "socks5";
        cfg.upstream.host = rest.substr(0, colon);
        try {
            cfg.upstream.port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            std::cout << "[!] 端口非法\n";
            return 2;
        }
        auto ac = auth.find(':');
        if (ac != std::string::npos) {
            cfg.upstream.user = auth.substr(0, ac);
            cfg.upstream.pass = auth.substr(ac + 1);
        }
        if (cfg.upstream.port < 1 || cfg.upstream.port > 65535) {
            std::cout << "[!] 端口越界\n";
            return 2;
        }
        // 自环保护
        bool loopback = cfg.upstream.host == "127.0.0.1" || cfg.upstream.host == "localhost";
        if (loopback && (cfg.upstream.port == cfg.listen_port || cfg.upstream.port == cfg.http_port)) {
            std::cout << "[!] 这个端口就是 svpn 自己，会形成自环\n";
            return 1;
        }
        std::string err;
        socket_t s = dial_via(Node{cfg.upstream.kind, cfg.upstream.host, cfg.upstream.port,
                                   cfg.upstream.user, cfg.upstream.pass},
                              "store.steampowered.com", 443, 6000, &err);
        if (s == kBadSock) {
            std::cout << fmt("[!] 节点不可用: %s\n", err.c_str());
            return 1;
        }
        close_socket(s);
        save_config(p, cfg);
        std::cout << fmt("[+] 已写入 config.json: %s\n",
                         Node{cfg.upstream.kind, cfg.upstream.host, cfg.upstream.port,
                              cfg.upstream.user, cfg.upstream.pass}.url().c_str());
        return 0;
    }

    // scan
    std::cout << "[i] 扫描本机常见代理端口…\n";
    std::vector<std::string> found;
    for (const char* ps : kCommonProxyPorts) {
        int port = std::atoi(ps);
        if (port == cfg.listen_port || port == cfg.http_port) continue;   // 自环保护
        if (!port_open("127.0.0.1", port, 200)) continue;
        for (const char* kind : {"socks5", "http"}) {
            std::string err;
            socket_t s = dial_via(Node{kind, "127.0.0.1", port, "", ""},
                                  "store.steampowered.com", 443, 4000, &err);
            if (s != kBadSock) {
                close_socket(s);
                found.push_back(fmt("%s://127.0.0.1:%d", kind, port));
                break;
            }
        }
    }
    if (found.empty()) {
        std::cout << "[!] 没找到可用节点（本机常见端口都不通）\n";
        return 1;
    }
    for (const auto& f : found) std::cout << fmt("  [+] %s\n", f.c_str());
    std::cout << fmt("[i] 共 %zu 个。写入: svpn node set %s\n", found.size(), found[0].c_str());
    return 0;
}

// ================================================================ cdn
int cmd_cdn(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    int timeout = a.secs("--timeout", 8);
    bool bench = !a.has("--no-bench");
    std::vector<std::string> domains;
    std::string one = a.flag("--domain");
    if (!one.empty()) {
        domains.push_back(one);
    } else {
        domains.assign(kCdnDomains, kCdnDomains + kCdnDomains_count);
    }
    Cache cache = load_cache(p, 1e18);   // 展示用：不过滤
    if (a.has("--refresh") || cache.empty()) {
        std::cout << fmt("[i] 解析 + 竞速 %zu 个域名（DoH 绕污染，TLS 握手淘汰%s）…\n",
                         domains.size(), bench ? "，1MB 实测限速" : "");
        Cache fresh = refresh(domains, bench, timeout * 1000);   // 秒 -> 毫秒
        for (auto& kv : fresh) cache[kv.first] = kv.second;
        save_cache(p, cache);
    }
    std::cout << fmt("%-38s %-17s %8s %10s  候选\n", "域名", "最优 IP", "握手", "实测");
    std::cout << std::string(84, '-') << "\n";
    double ttl = cfg.cdn.refresh_hours * 3600.0;
    for (const auto& d : domains) {
        auto it = cache.find(d);
        if (it == cache.end() || it->second.best_ip.empty()) {
            std::cout << fmt("%-38s %-17s %8s %10s  -\n", d.c_str(), "-", "-", "-");
            continue;
        }
        const DomainResult& r = it->second;
        bool stale = (static_cast<double>(time(nullptr)) - static_cast<double>(r.ts)) > ttl;
        // 没有下载路径的域名（Python 版 CDN_DOMAINS 里是 None）根本不该测吞吐，
        // 显示 "-" 而不是 "0.00MB/s"，否则会被误读成"测了但很慢"
        std::string tput = r.best_mbps > 0 ? fmt("%.2fMB/s", r.best_mbps) : "-";
        std::cout << fmt("%-38s %-17s %7.0fms %10s  %d%s\n", d.c_str(),
                         r.best_ip.c_str(), r.best_ms, tput.c_str(), r.candidates,
                         stale ? " *" : "");
    }
    return 0;
}

// ================================================================ hosts
int cmd_hosts(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    std::string action = a.pos.size() > 2 ? a.pos[2] : "apply";
    if (action == "clear") {
        if (!is_admin() && !a.has("--no-elevate")) {
            std::cout << "[i] 需要管理员权限，正在请求提权…\n";
            elevate("hosts clear --no-elevate");
            return hosts_managed_entries(p) == 0 ? 0 : 1;
        }
        HostsResult r = clear_hosts(p);
        std::cout << (r.ok ? "[+] hosts 已清理" : fmt("[!] 清理失败: %s", r.reason.c_str()))
                  << "\n";
        return r.ok ? 0 : 1;
    }

    if (cfg.cdn.enabled) {
        std::map<std::string, std::string> entries;
        std::string list;
        for (const auto& d : cfg.cdn.hosts) {
            if (!list.empty()) list += " ";
            list += d;
        }
        if (!list.empty()) entries["127.0.0.1"] = list;
        if (!is_admin() && !a.has("--no-elevate")) {
            std::cout << "[i] 写 hosts 需要管理员，正在弹 UAC…\n";
            elevate("hosts apply --no-elevate");
            return hosts_managed_entries(p) > 0 ? 0 : 1;
        }
        HostsResult r = apply_hosts(entries, p);
        if (!r.ok) {
            std::cout << fmt("[!] hosts 写入失败: %s\n", r.reason.c_str());
            return 1;
        }
        flush_dns();
        std::cout << fmt("[+] hosts 已写入 %d 个域名 -> 127.0.0.1（SNI 透明转发）\n", r.entries);
        std::cout << "[i] hosts 已指向 127.0.0.1，需 svpn up 常驻（SNI 模式）\n";
        return 0;
    }

    double ttl = cfg.cdn.refresh_hours * 3600.0;
    Cache cache = load_cache(p, ttl);
    if (cache.empty()) {
        double age = cache_age_hours(p);
        if (age < 0) {
            std::cout << "[i] 还没有 CDN 数据，先选边…\n";
        } else {
            std::cout << fmt("[!] CDN 选边结果已过期（%.1f 小时前更新，TTL %.0f 小时），"
                             "重新选边…\n", age, ttl / 3600.0);
        }
        std::vector<std::string> domains(kCdnDomains, kCdnDomains + kCdnDomains_count);
        save_cache(p, refresh(domains, true, a.secs("--timeout", 8) * 1000));
        cache = load_cache(p, ttl);
    }
    std::map<std::string, std::string> entries;
    for (const auto& kv : cache) {
        if (kv.second.best_ip.empty()) continue;
        std::string& slot = entries[kv.second.best_ip];
        if (!slot.empty()) slot += " ";
        slot += kv.first;
    }
    if (entries.empty()) {
        std::cout << "[!] 没有 CDN 选边结果，先跑 `svpn cdn --refresh`\n";
        return 1;
    }
    if (!is_admin() && !a.has("--no-elevate")) {
        std::cout << "[i] 写 hosts 需要管理员，正在弹 UAC…（拒绝就手动用管理员运行）\n";
        elevate("hosts apply --no-elevate");
        return hosts_managed_entries(p) > 0 ? 0 : 1;
    }
    HostsResult r = apply_hosts(entries, p);
    if (!r.ok) {
        std::cout << fmt("[!] hosts 写入失败: %s\n", r.reason.c_str());
        return 1;
    }
    flush_dns();
    std::cout << fmt("[+] hosts 已写入 %d 个域名 -> %d 个最优边缘 IP\n", r.entries,
                     static_cast<int>(entries.size()));
    for (const auto& kv : entries) {
        std::cout << fmt("    %-16s %s\n", kv.first.c_str(), kv.second.c_str());
    }
    return 0;
}

// ================================================================ system
int cmd_system(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    std::string action = a.pos.size() > 2 ? a.pos[2] : "show";
    if (action == "show") {
        WininetState w = read_wininet();
        std::cout << fmt("ProxyEnable   %d\nProxyServer   %s\nProxyOverride %s\n"
                         "AutoConfigURL %s\n", w.enable ? 1 : 0, w.server.c_str(),
                         w.override_list.c_str(), w.autoconfig.c_str());
        return 0;
    }
    if (action == "off") {
        WininetState r = restore_wininet(p);
        if (!r.ok && r.reason == "no backup") {
            WininetState now = read_wininet();
            std::cout << fmt("[!] 没有备份；当前 ProxyEnable=%d\n", now.enable ? 1 : 0);
            return 1;
        }
        std::cout << "[+] 系统代理已关闭（按备份还原）\n";
        return 0;
    }
    if (action == "on") {
        backup_wininet(p);
        WininetState r = apply_wininet(fmt("127.0.0.1:%d", cfg.listen_port),
                                       cfg.browser.bypass);
        std::cout << (r.ok ? fmt("[+] 系统代理已指向 127.0.0.1:%d\n", cfg.listen_port)
                           : fmt("[!] 设置失败: %s\n", r.reason.c_str()));
        return r.ok ? 0 : 1;
    }
    std::cout << "[!] 用法: svpn system [on|off|show]\n";
    return 2;
}

// ================================================================ pac
int cmd_pac(const Paths& p, const Args& a) {
    Config cfg = load_config(p);
    std::string pac = build_pac(cfg.listen_host, cfg.listen_port, cfg.proxy_suffixes,
                                cfg.direct_suffixes, true, cfg.mode);
    p.ensure_runtime();
    std::filesystem::path out = p.runtime / "svpn.pac";
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(pac.data(), static_cast<std::streamsize>(pac.size()));
    }
    std::cout << fmt("[+] 已写入 %s（%zu 字节）\n", out.string().c_str(), pac.size());
    if (a.has("--apply")) {
        backup_wininet(p);
        std::string url = "file:///" + out.string();
        std::replace(url.begin(), url.end(), '\\', '/');
        if (set_autoconfig_url(url)) {
            std::cout << "[+] 已把 PAC 写入系统代理（AutoConfigURL），浏览器重启后生效\n";
            std::cout << fmt("    %s\n", url.c_str());
        } else {
            std::cout << "[!] 写 AutoConfigURL 失败（注册表权限？）\n";
            return 1;
        }
    }
    return 0;
}

// ================================================================ steam
std::wstring find_steam() {
    const wchar_t* candidates[] = {
        L"D:\\app\\Bapp\\st\\steam.exe",
        L"C:\\Program Files (x86)\\Steam\\steam.exe",
        L"C:\\Program Files\\Steam\\steam.exe",
    };
    for (const wchar_t* c : candidates) {
        if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return {};
}

int cmd_steam(const Paths& p, const Args& a) {
    std::wstring exe = a.has("--exe") ? to_w(a.flag("--exe")) : find_steam();
    if (exe.empty()) {
        std::cout << "[i] 没找到 steam.exe（返回 2，属于正常）\n";
        return 2;
    }
    if (a.has("--system")) {
        Config cfg = load_config(p);
        backup_wininet(p);
        apply_wininet(fmt("127.0.0.1:%d", cfg.listen_port), cfg.browser.bypass);
        std::cout << fmt("[+] 系统代理已指向 127.0.0.1:%d，正在拉起 Steam…\n", cfg.listen_port);
    }
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        std::cout << fmt("[!] 启动 Steam 失败: %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::cout << "[+] 已拉起 Steam（改代理后必须重启客户端才生效）\n";
    return 0;
}

// ================================================================ doctor
int cmd_doctor(const Paths& p, const Args&) {
    std::string cfg_err;
    Config cfg = load_config(p, &cfg_err);
    int issues = 0;
    if (!cfg_err.empty()) {
        // load_config 的 err 已经带 "config.json 解析失败: " 前缀，别再套一层
        std::cout << fmt("[!] %s（当前按默认值运行）\n", cfg_err.c_str());
        ++issues;
    }
    std::cout << fmt("svpn %s  doctor\n", kVersion);
    if (!std::filesystem::exists(p.config)) {
        std::cout << "[!] 缺 config.json，跑 svpn init\n";
        ++issues;
    }
    bool socks_busy = port_open(cfg.listen_host, cfg.listen_port);
    bool http_busy = cfg.http_port && port_open(cfg.listen_host, cfg.http_port);
    DWORD live_pid = 0;
    if (socks_busy && !read_pid(p, &live_pid)) {
        // 端口被占但没有 pid 文件：可能是别的程序
        std::cout << fmt("[!] 端口 %d 被占用，但不是 svpn 自己\n", cfg.listen_port);
        ++issues;
    }
    if (http_busy && !socks_busy) {
        std::cout << fmt("[!] HTTP 端口 %d 被别的程序占着\n", cfg.http_port);
        ++issues;
    }
    if (cfg.http_port && cfg.http_port == cfg.listen_port) {
        std::cout << "[!] http_port 与 listen_port 相同\n";
        ++issues;
    }
    if (!unknown_config_keys().empty()) {
        // 拼错的键会被静默丢弃（listen_prot: 8000 这种），必须让用户看见
        const auto& unk = unknown_config_keys();
        std::string list;
        for (std::size_t i = 0; i < unk.size() && i < 5; ++i) {
            if (!list.empty()) list += ", ";
            list += unk[i];
        }
        if (unk.size() > 5) list += fmt(" …共 %zu 个", unk.size());
        std::cout << fmt("[!] config.json 里有 %zu 个未知键（拼错了？"
                         "可 svpn init --force 对照模板）: %s\n", unk.size(), list.c_str());
        ++issues;
    }
    if (cfg.cdn.enabled && cfg.cdn.sni_port != 443) {
        std::cout << "[!] cdn.sni_port 不是 443：hosts 只能表达 IP，客户端连的是 443\n";
        ++issues;
    }
    double age = cache_age_hours(p);
    if (age >= 0 && age * 3600 > cfg.cdn.refresh_hours * 3600) {
        std::cout << fmt("[!] CDN 缓存已过期（%.1f 小时），跑 svpn cdn --refresh\n", age);
        ++issues;
    }
    if (cfg.upstream.configured()) {
        std::string err;
        socket_t s = dial_via(Node{cfg.upstream.kind, cfg.upstream.host, cfg.upstream.port,
                                   cfg.upstream.user, cfg.upstream.pass},
                              "store.steampowered.com", 443, 5000, &err);
        if (s == kBadSock) {
            std::cout << fmt("[!] 节点不通: %s\n", err.c_str());
            ++issues;
        } else {
            close_socket(s);
            std::cout << "[+] 节点可用\n";
        }
    } else {
        std::cout << "[i] 未配节点（社区/头像会不可用，属预期）\n";
    }
    std::cout << "[i] 管理员: " << (is_admin() ? "是" : "否（写 hosts 需要提权）") << "\n";
    std::cout << (issues == 0 ? "[+] 没发现问题\n" : fmt("[!] %d 个问题待处理\n", issues));
    return issues == 0 ? 0 : 1;
}

void print_usage() {
    std::cout << "svpn " << kVersion << "  ·  Steam 分流/加速\n"
              << "用法: svpn <命令> [选项]\n\n"
              << "  init    初始化 config.json（--force 覆盖）\n"
              << "  up      启动代理（-b 后台 / -p 端口 / -m 模式 / --system-proxy / --node）\n"
              << "  down    停止（--keep-system / --clear-hosts）\n"
              << "  status  当前状态\n"
              << "  probe   端点可用性（--node none / --timeout / --json / --lenient）\n"
              << "  node    scan | set <url> | clear\n"
              << "  cdn     选边（--refresh / --no-bench / --domain / --timeout）\n"
              << "  hosts   apply | clear（--no-elevate）\n"
              << "  system  on | off | show\n"
              << "  pac     生成 PAC（--apply 同时写入系统代理）\n"
              << "  steam   拉起 Steam（--system / --exe）\n"
              << "  doctor  体检\n"
              << "  gui     打开图形界面（与 svpn-gui.exe 等价）\n";
}

// ================================================================ 分发
int dispatch(const std::vector<std::string>& argv) {
    Paths p = Paths::resolve();
    console_utf8();
    net_init();
    if (argv.size() < 2) {
        print_usage();
        net_shutdown();
        return 2;
    }
    const std::string cmd = argv[1];
    // 任何位置出现 -h/--help 都只打印用法：之前写成 `cmd == "-h"` 才进这个分支，
    // 于是 `svpn probe -h` 会真的去连一遍 9 个端点（还等了 3 秒超时）。
    for (const auto& x : argv) {
        if (x == "-h" || x == "--help" || x == "help") {
            print_usage();
            net_shutdown();
            return 0;
        }
    }
    Args a;
    a.pos = argv;
    int rc = 0;
    if (cmd == "-v" || cmd == "--version") {
        std::cout << kVersion << "\n";
    } else if (cmd == "init") {
        rc = cmd_init(p, a);
    } else if (cmd == "up") {
        rc = cmd_up(p, a);
    } else if (cmd == "_serve") {
        rc = cmd_serve(p, a);
    } else if (cmd == "down") {
        rc = cmd_down(p, a);
    } else if (cmd == "status") {
        rc = cmd_status(p, a);
    } else if (cmd == "probe") {
        rc = cmd_probe(p, a);
    } else if (cmd == "node") {
        rc = cmd_node(p, a);
    } else if (cmd == "cdn") {
        rc = cmd_cdn(p, a);
    } else if (cmd == "hosts") {
        rc = cmd_hosts(p, a);
    } else if (cmd == "system") {
        rc = cmd_system(p, a);
    } else if (cmd == "pac") {
        rc = cmd_pac(p, a);
    } else if (cmd == "steam") {
        rc = cmd_steam(p, a);
    } else if (cmd == "doctor") {
        rc = cmd_doctor(p, a);
    } else {
        std::cout << fmt("[!] 未知命令: %s\n", cmd.c_str());
        rc = 2;
    }
    net_shutdown();
    return rc;
}

}  // namespace
}  // namespace svpn

namespace svpn {
// 供 GUI 复用同一条命令通道，保证图形界面与命令行行为完全一致
int dispatch_cli(const std::vector<std::string>& args) { return dispatch(args); }
}  // namespace svpn

#ifdef SVPN_GUI_SUBSYSTEM
// 窗口子系统构建：没有控制台，直接进图形界面
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return svpn::gui_main(); }
#else
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(svpn::to_u8(argv[i]));
    // `svpn gui` / `svpn --gui` 打开图形界面；其余一律走命令行，保持原有语义
    if (argc >= 2) {
        const std::string sub = svpn::to_u8(argv[1]);
        if (sub == "gui" || sub == "--gui") return svpn::gui_main();
    }
    return svpn::dispatch(args);
}
#endif
