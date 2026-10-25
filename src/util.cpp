// 基础设施：字符串、日志、路径、统计。无第三方依赖。
#include "svpn.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace svpn {
namespace {
std::mutex g_log_mu;
std::filesystem::path g_log_file;
bool g_log_ready = false;
bool g_verbose = false;
}  // namespace

// ------------------------------------------------------------------ 字符串
std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> split(const std::string& s, char sep, bool keep_empty) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            std::string piece = s.substr(start, i - start);
            if (keep_empty || !piece.empty()) out.push_back(std::move(piece));
            start = i + 1;
        }
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           std::memcmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0;
}

std::string now_hms() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::int64_t now_ms() {
    return static_cast<std::int64_t>(GetTickCount64());
}

std::string fmt(const char* f, ...) {
    char stack_buf[512];
    va_list ap;
    va_start(ap, f);
    int n = std::vsnprintf(stack_buf, sizeof stack_buf, f, ap);
    va_end(ap);
    if (n < 0) return {};
    if (static_cast<std::size_t>(n) < sizeof stack_buf) return std::string(stack_buf, static_cast<std::size_t>(n));
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    va_start(ap, f);
    std::vsnprintf(&big[0], big.size(), f, ap);
    va_end(ap);
    big.resize(static_cast<std::size_t>(n));
    return big;
}

// ------------------------------------------------------------------ 控制台/日志
void console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void log_init(const Paths& p, bool verbose) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log_file = p.log;
    g_log_ready = true;
    g_verbose = verbose;
    std::error_code ec;
    std::filesystem::create_directories(p.runtime, ec);
}

void log_write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!g_verbose && std::strcmp(level, "DEBUG") == 0) return;
    if (g_log_ready && !g_log_file.empty()) {
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, g_log_file.wstring().c_str(), L"a, ccs=UTF-8") == 0 && f) {
            std::fprintf(f, "%s %-7s %s\n", now_hms().c_str(), level, msg.c_str());
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "%s %-7s %s\n", now_hms().c_str(), level, msg.c_str());
}

// ------------------------------------------------------------------ 节点/配置小工具
std::string Node::url() const {
    std::string auth;
    if (!user.empty()) {
        auth = user;
        if (!pass.empty()) auth += ":" + pass;
        auth += "@";
    }
    return kind + "://" + auth + host + ":" + std::to_string(port);
}

bool UpstreamCfg::configured() const {
    return !host.empty() && host != "NODE_HOST" && port > 0;
}

// ------------------------------------------------------------------ 路径
Paths Paths::resolve() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::filesystem::path exe(buf, buf + n);
    Paths p;
    p.root = exe.parent_path();
    p.config = p.root / "config.json";
    p.runtime = p.root / "runtime";
    p.cdn_cache = p.runtime / "cdn-cache.json";
    p.probe = p.runtime / "probe.json";
    p.log = p.runtime / "svpn.log";
    p.pid = p.runtime / "svpn.pid";
    p.stats = p.runtime / "stats.json";
    p.hosts_backup = p.runtime / "hosts.backup";
    p.wininet_backup = p.runtime / "wininet-backup.json";
    return p;
}

void Paths::ensure_runtime() const {
    std::error_code ec;
    std::filesystem::create_directories(runtime, ec);
}

// ------------------------------------------------------------------ 统计
void Stats::hit(const std::string& host) {
    std::lock_guard<std::mutex> lk(mu);
    if (host.empty()) return;
    auto it = top_hosts.find(host);
    if (it == top_hosts.end()) {
        if (top_hosts.size() >= 64) return;   // 上限：别让恶意域名把内存吃光
        top_hosts.emplace(host, 1);
    } else {
        ++it->second;
    }
}

std::string Stats::summary_line() const {
    return fmt("%llu 连接 / 上行 %.2fMB / 下行 %.2fMB / 错误 %llu",
               static_cast<unsigned long long>(conns.load()),
               static_cast<double>(up_bytes.load()) / 1048576.0,
               static_cast<double>(down_bytes.load()) / 1048576.0,
               static_cast<unsigned long long>(errors.load()));
}

void Stats::dump(const Paths& p) const {
    Json j = Json::make_obj();
    std::int64_t started = started_ms.load();
    j.set("uptime_s", Json::make_num(started ? static_cast<double>((now_ms() - started) / 1000) : 0));
    j.set("connections", Json::make_num(static_cast<double>(conns.load())));
    j.set("errors", Json::make_num(static_cast<double>(errors.load())));
    j.set("via_upstream", Json::make_num(static_cast<double>(via_upstream.load())));
    j.set("via_direct", Json::make_num(static_cast<double>(via_direct.load())));
    j.set("up_mb", Json::make_num(static_cast<double>(up_bytes.load()) / 1048576.0));
    j.set("down_mb", Json::make_num(static_cast<double>(down_bytes.load()) / 1048576.0));
    Json tops = Json::make_arr();
    {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<std::pair<std::string, std::uint64_t>> items(top_hosts.begin(), top_hosts.end());
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (items.size() > 8) items.resize(8);
        for (const auto& it : items) {
            Json pair = Json::make_arr();
            pair.push(Json::make_str(it.first));
            pair.push(Json::make_num(static_cast<double>(it.second)));
            tops.push(std::move(pair));
        }
    }
    j.set("top_hosts", std::move(tops));
    std::error_code ec;
    std::filesystem::create_directories(p.runtime, ec);
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.stats.wstring().c_str(), L"w, ccs=UTF-8") == 0 && f) {
        std::string text = j.dump(2);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

}  // namespace svpn
