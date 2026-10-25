// svpn (C++) —— Windows 系统层：hosts 托管块 / WinINET 系统代理 / 提权。
//
// 行为基准：Python `svpn/system.py`。两处刻意的差别（都在报告里写明了）：
//   1. hosts 全程按**字节**处理，不解码不重编码 —— 用户 hosts 里可能是 GBK 中文注释、
//      UTF-8 BOM、混合行尾，解码再编码是不可逆的数据损失。Python 版靠编码探测扛住了，
//      这里直接不做这层猜测。
//   2. hosts 备份每次写入前覆盖（`runtime/hosts.backup`），Python 是「只留第一份」。
#include "svpn.hpp"

#include <windows.h>
#include <shellapi.h>
#include <windns.h>
#include <wininet.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

namespace svpn {
namespace {

// MinGW-w64 的 windns.h 里没有声明它（libdnsapi.a 里符号是有的），自己补一条。
extern "C" __declspec(dllimport) BOOL WINAPI DnsFlushResolverCache(void);

const char* const kMarkBegin = "# >>> steam-vpn managed block (do not edit) >>>";
const char* const kMarkEnd = "# <<< steam-vpn managed block <<<";
const wchar_t* const kNetKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

// ------------------------------------------------------------------ 编码
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string win_err_text(DWORD err) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string text = (msg != nullptr && n > 0) ? trim(to_utf8(std::wstring(msg, n))) : std::string();
    if (msg != nullptr) LocalFree(msg);
    if (text.empty()) return fmt("错误码 %lu", static_cast<unsigned long>(err));
    return fmt("错误码 %lu (%s)", static_cast<unsigned long>(err), text.c_str());
}

// ------------------------------------------------------------------ 字节级行处理
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string strip_bytes(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string rstrip_bytes(const std::string& s) {
    std::size_t e = s.size();
    while (e > 0 && is_ws(s[e - 1])) --e;
    return s.substr(0, e);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_ws(s[i])) ++i;
        const std::size_t st = i;
        while (i < s.size() && !is_ws(s[i])) ++i;
        if (i > st) out.emplace_back(s, st, i - st);
    }
    return out;
}

// ------------------------------------------------------------------ IP / 主机名校验
// 等价 ipaddress.ip_address()：IPv4 严格（不许前导零、必须 4 段、每段 <=255），
// IPv6 按分组规则（支持 :: 压缩、末尾内嵌 IPv4、%zone）。
bool valid_ipv4(const std::string& s) {
    const std::vector<std::string> parts = split(s, '.', true);
    if (parts.size() != 4) return false;
    for (const auto& p : parts) {
        if (p.empty() || p.size() > 3) return false;
        if (p.size() > 1 && p[0] == '0') return false;   // "010.1.1.1" 非法
        int v = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        if (v > 255) return false;
    }
    return true;
}

bool valid_hex_group(const std::string& g) {
    if (g.empty() || g.size() > 4) return false;
    for (char c : g) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

bool valid_ipv6(const std::string& in) {
    std::string s = in;
    const std::size_t pct = s.find('%');
    if (pct != std::string::npos) {           // fe80::1%eth0 —— Python 3.9+ 也接受
        if (pct + 1 >= s.size()) return false;
        s.resize(pct);
    }
    if (s.empty() || s.find('%') != std::string::npos) return false;

    bool compressed = false;
    std::string left = s, right;
    const std::size_t pos = s.find("::");
    if (pos != std::string::npos) {
        compressed = true;
        if (s.find("::", pos + 1) != std::string::npos) return false;   // 只能压一次
        left = s.substr(0, pos);
        right = s.substr(pos + 2);
    }

    auto collect = [](const std::string& part, std::vector<std::string>* out) -> bool {
        if (part.empty()) return true;
        const std::vector<std::string> gs = split(part, ':', true);
        for (const auto& g : gs) {
            if (g.empty()) return false;      // "1:::2" 之类
        }
        out->insert(out->end(), gs.begin(), gs.end());
        return true;
    };

    std::vector<std::string> groups;
    if (!collect(left, &groups)) return false;
    if (!collect(right, &groups)) return false;

    int slots = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const std::string& g = groups[i];
        if (g.find('.') != std::string::npos) {
            if (i + 1 != groups.size()) return false;   // 内嵌 IPv4 只能是最后一组
            if (!valid_ipv4(g)) return false;
            slots += 2;
        } else {
            if (!valid_hex_group(g)) return false;
            slots += 1;
        }
    }
    if (compressed) return slots <= 7;     // :: 至少压缩一组
    return slots == 8;
}

bool valid_ip(const std::string& s) { return valid_ipv4(s) || valid_ipv6(s); }

// 等价 `^[A-Za-z0-9]([A-Za-z0-9._-]*[A-Za-z0-9])?$`
bool valid_host_name(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    auto ok = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    };
    if (!((h.front() >= 'A' && h.front() <= 'Z') || (h.front() >= 'a' && h.front() <= 'z') ||
          (h.front() >= '0' && h.front() <= '9'))) {
        return false;
    }
    for (std::size_t i = 0; i < h.size(); ++i) {
        if (!ok(h[i])) return false;
        const bool edge = (i == 0 || i + 1 == h.size());
        if (edge) {
            const bool alnum = (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= 'a' && h[i] <= 'z') ||
                               (h[i] >= '0' && h[i] <= '9');
            if (!alnum) return false;
        }
    }
    return true;
}

// 等价 str repr（只覆盖 IP 字符串会遇到的字符），用于 rejected 里的 `ip='1.2.3'`
std::string py_repr(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    out += '\'';
    return out;
}

// ------------------------------------------------------------------ 文件读写
struct FileBytes {
    std::string data;
    bool exists{false};
    DWORD err{0};
};

FileBytes read_file_bytes(const std::filesystem::path& p) {
    FileBytes fb;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fb.err = GetLastError();
        if (fb.err == ERROR_FILE_NOT_FOUND || fb.err == ERROR_PATH_NOT_FOUND) fb.err = 0;
        return fb;
    }
    fb.exists = true;
    char buf[8192];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr)) {
            fb.err = GetLastError();
            break;
        }
        if (got == 0) break;
        fb.data.append(buf, got);
    }
    CloseHandle(h);
    return fb;
}

bool write_bytes_plain(const std::filesystem::path& p, const std::string& data, DWORD* err) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = GetLastError();
        return false;
    }
    bool ok = true;
    std::size_t off = 0;
    while (off < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - off, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    if (ok) FlushFileBuffers(h);
    if (!ok && err) *err = GetLastError();
    CloseHandle(h);
    return ok;
}

// 原子写：同目录 hosts.tmp → MoveFileEx(REPLACE_EXISTING)。失败必须清掉 tmp。
bool write_hosts_atomic(const std::filesystem::path& target, const std::string& data, DWORD* err) {
    std::filesystem::path tmp = target.parent_path() / (target.filename().wstring() + L".tmp");
    if (!write_bytes_plain(tmp, data, err)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = GetLastError();
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// hosts 路径：默认 %SystemRoot%\System32\drivers\etc\hosts。
// SVPN_HOSTS_FILE 仅供测试/便携（头文件里的 apply_hosts 没有 hosts 路径参数，
// 测试不能去动真实 hosts —— 只能用环境变量留一个测试缝）。
std::filesystem::path hosts_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetEnvironmentVariableW(L"SVPN_HOSTS_FILE", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return std::filesystem::path(std::wstring(buf, n));

    n = GetEnvironmentVariableW(L"SystemRoot", buf, static_cast<DWORD>(std::size(buf)));
    std::wstring root = (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring(L"C:\\Windows");
    return std::filesystem::path(root) / L"System32" / L"drivers" / L"etc" / L"hosts";
}

std::string eol_of(const std::string& raw) {
    return raw.find("\r\n") != std::string::npos ? std::string("\r\n") : std::string("\n");
}

// 逐字节切行（只认 \r\n / \n，不解码）；返回「去掉托管块之后」的原文：
//   * 每行保留自己原本的行尾（混合行尾也照原样），BOM 单独摘出来最后原样放回；
//   * 尾部空行整体丢掉 —— 那是 apply 写块时塞进去的分隔行，
//     这样 clear_hosts 才能把「除托管块外一个字节都没变」做到字节级。
std::string strip_managed_block(const std::string& raw, std::string* bom_out) {
    std::string bom;
    std::size_t pos = 0;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        bom.assign(raw, 0, 3);
        pos = 3;
    }
    std::vector<std::pair<std::string, std::string>> kept;   // (行内容, 行尾)
    bool skip = false;
    while (pos < raw.size()) {
        const std::size_t nlpos = raw.find('\n', pos);
        std::string line;
        std::string eol;
        if (nlpos == std::string::npos) {
            line = raw.substr(pos);
            pos = raw.size();
        } else if (nlpos > pos && raw[nlpos - 1] == '\r') {
            line = raw.substr(pos, nlpos - 1 - pos);
            eol = "\r\n";
            pos = nlpos + 1;
        } else {
            line = raw.substr(pos, nlpos - pos);
            eol = "\n";
            pos = nlpos + 1;
        }
        const std::string s = strip_bytes(line);
        if (s == kMarkBegin) {
            skip = true;
            continue;
        }
        if (s == kMarkEnd) {
            skip = false;
            continue;
        }
        if (!skip) kept.emplace_back(std::move(line), std::move(eol));
    }
    while (!kept.empty() && strip_bytes(kept.back().first).empty()) kept.pop_back();
    std::string out;
    for (const auto& kv : kept) {
        out += kv.first;
        out += kv.second;
    }
    if (bom_out != nullptr) *bom_out = bom;
    return out;
}

std::string hosts_block(const std::map<std::string, std::string>& entries, const std::string& nl) {
    std::string out = kMarkBegin;
    for (const auto& kv : entries) {
        out += nl;
        out += kv.first;
        if (kv.first.size() < 16) out.append(16 - kv.first.size(), ' ');   // f"{ip:<16}"
        out += kv.second;
    }
    out += nl;
    out += kMarkEnd;
    return out;
}

void sanitize_entries(const std::map<std::string, std::string>& entries,
                      std::map<std::string, std::string>* clean,
                      std::vector<std::string>* rejected) {
    for (const auto& kv : entries) {
        const std::string ip = strip_bytes(kv.first);
        if (!valid_ip(ip)) {
            rejected->push_back("ip=" + py_repr(kv.first));
            continue;
        }
        std::vector<std::string> good;
        for (const std::string& raw_host : split_ws(kv.second)) {
            std::string host = strip_bytes(raw_host);
            if (host.empty() || host.size() > 253 || !valid_host_name(host)) {
                rejected->push_back(host.empty() ? "<empty>" : host);
                continue;
            }
            // 主机名位置出现 IP 一律拒绝（否则可以伪造出「一行两用」的多行语义）
            if (valid_ip(host)) {
                rejected->push_back(host);
                continue;
            }
            if (host.find(kMarkBegin) != std::string::npos ||
                host.find(kMarkEnd) != std::string::npos || host.find('#') != std::string::npos) {
                rejected->push_back(host);
                continue;
            }
            good.push_back(host);
        }
        if (!good.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < good.size(); ++i) {
                if (i != 0) joined += ' ';
                joined += good[i];
            }
            (*clean)[ip] = joined;
        }
    }
}

bool is_denied(DWORD err) { return err == ERROR_ACCESS_DENIED || err == ERROR_WRITE_PROTECT; }

// ------------------------------------------------------------------ 注册表小工具
bool reg_read_dword(HKEY k, const wchar_t* name, bool* present, DWORD* out) {
    DWORD type = 0, sz = sizeof(DWORD), val = 0;
    const LONG rc = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&val), &sz);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) {
        *present = false;
        return true;
    }
    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_DWORD || sz != sizeof(DWORD)) {
        *present = false;
        return true;
    }
    *present = true;
    *out = val;
    return true;
}

bool reg_read_sz(HKEY k, const wchar_t* name, bool* present, std::string* out) {
    DWORD type = 0, sz = 0;
    LONG rc = RegQueryValueExW(k, name, nullptr, &type, nullptr, &sz);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) {
        *present = false;
        out->clear();
        return true;
    }
    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        *present = false;
        out->clear();
        return true;
    }
    std::vector<wchar_t> buf(static_cast<std::size_t>(sz) / sizeof(wchar_t) + 2, L'\0');
    DWORD cap = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    rc = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &cap);
    if (rc != ERROR_SUCCESS) return false;
    std::size_t len = static_cast<std::size_t>(cap) / sizeof(wchar_t);
    while (len > 0 && buf[len - 1] == L'\0') --len;
    *present = true;
    *out = to_utf8(std::wstring(buf.data(), len));
    return true;
}

struct RawWininet {
    bool ok{false};
    DWORD err{0};
    bool has_enable{false}, has_server{false}, has_override{false}, has_autoconfig{false};
    DWORD enable{0};
    std::string server, override_list, autoconfig;
};

RawWininet read_raw_wininet() {
    RawWininet rw;
    HKEY k = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kNetKey, 0, KEY_READ, &k);
    if (rc != ERROR_SUCCESS) {
        rw.err = static_cast<DWORD>(rc);
        return rw;
    }
    rw.ok = reg_read_dword(k, L"ProxyEnable", &rw.has_enable, &rw.enable) &&
            reg_read_sz(k, L"ProxyServer", &rw.has_server, &rw.server) &&
            reg_read_sz(k, L"ProxyOverride", &rw.has_override, &rw.override_list) &&
            reg_read_sz(k, L"AutoConfigURL", &rw.has_autoconfig, &rw.autoconfig);
    RegCloseKey(k);
    return rw;
}

void notify_change() {
    // 不通知的话浏览器要重启才认新代理
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}

bool reg_set_sz(HKEY k, const wchar_t* name, const std::string& value) {
    const std::wstring w = to_wide(value);
    const DWORD bytes = static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), bytes) ==
           ERROR_SUCCESS;
}

bool reg_set_dword(HKEY k, const wchar_t* name, DWORD value) {
    return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                          sizeof value) == ERROR_SUCCESS;
}

}  // namespace

// ============================================================ hosts
HostsResult apply_hosts(const std::map<std::string, std::string>& ip_to_hosts, const Paths& p) {
    HostsResult res;
    std::map<std::string, std::string> clean;
    sanitize_entries(ip_to_hosts, &clean, &res.rejected);
    if (clean.empty()) {
        res.ok = false;
        res.reason = "没有合法的 hosts 条目（IP/域名校验未通过）";
        log_write("WARN", res.reason);
        return res;
    }

    const std::filesystem::path hp = hosts_path();
    const FileBytes cur = read_file_bytes(hp);
    if (!cur.exists && cur.err != 0) {
        res.reason = fmt("读 hosts 失败: %s", win_err_text(cur.err).c_str());
        log_write("WARN", res.reason);
        return res;
    }

    // 写前备份原始字节（覆盖上一份）
    if (cur.exists) {
        p.ensure_runtime();
        DWORD berr = 0;
        if (!write_bytes_plain(p.hosts_backup, cur.data, &berr)) {
            log_write("WARN", fmt("备份 hosts 失败（继续写）: %s", win_err_text(berr).c_str()));
        }
    }

    const std::string nl = eol_of(cur.data);
    std::string bom;
    const std::string body = strip_managed_block(cur.data, &bom);
    // 与 Python 同形：`nl.join(...).rstrip() + nl + nl + block + nl`
    const std::string text = bom + rstrip_bytes(body) + nl + nl + hosts_block(clean, nl) + nl;

    DWORD err = 0;
    if (!write_hosts_atomic(hp, text, &err)) {
        if (is_denied(err)) {
            res.reason = "需要管理员权限写 hosts";
            log_write("WARN", fmt("写 hosts 被拒绝: %s", win_err_text(err).c_str()));
        } else {
            res.reason = fmt("写 hosts 失败: %s", win_err_text(err).c_str());
            log_write("WARN", res.reason);
        }
        return res;
    }
    res.ok = true;
    res.entries = static_cast<int>(clean.size());
    log_write("INFO", fmt("hosts 已写入 %d 条托管记录", res.entries));
    return res;
}

HostsResult clear_hosts(const Paths& p) {
    (void)p;
    HostsResult res;
    const std::filesystem::path hp = hosts_path();
    const FileBytes cur = read_file_bytes(hp);
    if (!cur.exists && cur.err != 0) {
        res.reason = fmt("读 hosts 失败: %s", win_err_text(cur.err).c_str());
        log_write("WARN", res.reason);
        return res;
    }
    std::string bom;
    // clear 不做「补一个换行」：留下的字节（含最后一行有没有换行）原样写回
    const std::string body = strip_managed_block(cur.data, &bom);
    const std::string text = bom + body;

    DWORD err = 0;
    if (!write_hosts_atomic(hp, text, &err)) {
        if (is_denied(err)) {
            res.reason = "需要管理员权限写 hosts";
            log_write("WARN", fmt("写 hosts 被拒绝: %s", win_err_text(err).c_str()));
        } else {
            res.reason = fmt("写 hosts 失败: %s", win_err_text(err).c_str());
            log_write("WARN", res.reason);
        }
        return res;
    }
    res.ok = true;
    return res;
}

int hosts_managed_entries(const Paths& p) {
    (void)p;
    const FileBytes cur = read_file_bytes(hosts_path());
    if (!cur.exists) return 0;
    int count = 0;
    bool inside = false;
    std::size_t pos = 0;
    const std::string& raw = cur.data;
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        pos = 3;
    }
    while (pos < raw.size()) {
        const std::size_t nlpos = raw.find('\n', pos);
        std::string line;
        if (nlpos == std::string::npos) {
            line = raw.substr(pos);
            pos = raw.size();
        } else {
            const std::size_t end = (nlpos > pos && raw[nlpos - 1] == '\r') ? nlpos - 1 : nlpos;
            line = raw.substr(pos, end - pos);
            pos = nlpos + 1;
        }
        const std::string s = strip_bytes(line);
        if (s == kMarkBegin) {
            inside = true;
            continue;
        }
        if (s == kMarkEnd) {
            inside = false;
            continue;
        }
        if (!inside || s.empty() || s[0] == '#') continue;
        if (split_ws(s).size() >= 2) ++count;   // 至少「IP 主机名」
    }
    return count;
}

void flush_dns() {
    if (DnsFlushResolverCache()) {
        log_write("DEBUG", "DNS 缓存已刷新（DnsFlushResolverCache）");
        return;
    }
    // 退回 ipconfig /flushdns（静默，不弹窗）
    std::wstring cmd = L"ipconfig.exe /flushdns";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        log_write("WARN", fmt("刷新 DNS 失败: %s", win_err_text(GetLastError()).c_str()));
        return;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log_write("DEBUG", "DNS 缓存已刷新（ipconfig /flushdns）");
}

// ============================================================ wininet
WininetState read_wininet() {
    WininetState st;
    const RawWininet rw = read_raw_wininet();
    if (!rw.ok) {
        st.ok = false;
        st.reason = fmt("读注册表 Internet Settings 失败: %s", win_err_text(rw.err).c_str());
        return st;
    }
    st.ok = true;
    st.enable = rw.has_enable && rw.enable != 0;
    st.server = rw.server;
    st.override_list = rw.override_list;
    st.autoconfig = rw.autoconfig;
    return st;
}

bool backup_wininet(const Paths& p) {
    std::error_code ec;
    std::filesystem::create_directories(p.runtime, ec);
    if (std::filesystem::exists(p.wininet_backup, ec)) return true;   // 只留最开始那份真·原始值

    const RawWininet rw = read_raw_wininet();
    if (!rw.ok) {
        log_write("WARN", fmt("备份系统代理失败: %s", win_err_text(rw.err).c_str()));
        return false;
    }
    // 键名/顺序与 Python 版完全一致；缺失的值写 null（Python 里是 None）
    Json j = Json::make_obj();
    j.set("ProxyEnable", rw.has_enable ? Json::make_num(static_cast<double>(rw.enable)) : Json{});
    j.set("ProxyServer", rw.has_server ? Json::make_str(rw.server) : Json{});
    j.set("ProxyOverride", rw.has_override ? Json::make_str(rw.override_list) : Json{});
    j.set("AutoConfigURL", rw.has_autoconfig ? Json::make_str(rw.autoconfig) : Json{});

    DWORD err = 0;
    if (!write_bytes_plain(p.wininet_backup, j.dump(2), &err)) {
        log_write("WARN", fmt("写 wininet-backup.json 失败: %s", win_err_text(err).c_str()));
        return false;
    }
    log_write("INFO", "系统代理设置已备份到 runtime/wininet-backup.json");
    return true;
}

WininetState apply_wininet(const std::string& server, const std::string& bypass) {
    WininetState st;
    backup_wininet(Paths::resolve());   // 签名里没有 Paths：备份固定落 exe 目录的 runtime/

    HKEY k = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kNetKey, 0, KEY_SET_VALUE, &k);
    if (rc != ERROR_SUCCESS) {
        st.ok = false;
        st.reason = fmt("打开注册表 Internet Settings 失败: %s",
                        win_err_text(static_cast<DWORD>(rc)).c_str());
        log_write("WARN", st.reason);
        return st;
    }
    const std::string ov = bypass.empty() ? std::string("<local>") : bypass;
    const bool ok = reg_set_sz(k, L"AutoConfigURL", "") && reg_set_dword(k, L"ProxyEnable", 1) &&
                    reg_set_sz(k, L"ProxyServer", server) && reg_set_sz(k, L"ProxyOverride", ov);
    RegCloseKey(k);
    if (!ok) {
        st.ok = false;
        st.reason = fmt("写注册表失败: %s", win_err_text(GetLastError()).c_str());
        log_write("WARN", st.reason);
        return st;
    }
    notify_change();
    return read_wininet();
}

WininetState restore_wininet(const Paths& p) {
    WininetState st;
    std::error_code ec;
    if (!std::filesystem::exists(p.wininet_backup, ec)) {
        // 绝不猜默认值：没有备份就不动用户设置
        st.ok = false;
        st.reason = "no backup";
        return st;
    }
    const FileBytes fb = read_file_bytes(p.wininet_backup);
    if (!fb.exists && fb.err != 0) {
        st.ok = false;
        st.reason = fmt("读 wininet-backup.json 失败: %s", win_err_text(fb.err).c_str());
        return st;
    }
    std::string perr;
    const Json j = Json::parse(fb.data, &perr);
    if (j.type != JType::Obj) {
        st.ok = false;
        st.reason = fmt("wininet-backup.json 损坏: %s", perr.empty() ? "不是 JSON 对象" : perr.c_str());
        log_write("WARN", st.reason);
        return st;
    }
    const Json* en = j.get("ProxyEnable");
    DWORD enable = (en != nullptr && en->type == JType::Num && en->num != 0) ? 1u : 0u;
    auto str_of = [&j](const char* key) {
        const Json* v = j.get(key);
        return v != nullptr ? v->as_str("") : std::string();
    };
    const std::string server = str_of("ProxyServer");
    const std::string ov = str_of("ProxyOverride");
    const std::string autoconfig = str_of("AutoConfigURL");

    HKEY k = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, kNetKey, 0, KEY_SET_VALUE, &k);
    if (rc != ERROR_SUCCESS) {
        st.ok = false;
        st.reason = fmt("打开注册表 Internet Settings 失败: %s",
                        win_err_text(static_cast<DWORD>(rc)).c_str());
        log_write("WARN", st.reason);
        return st;
    }
    const bool ok = reg_set_dword(k, L"ProxyEnable", enable) && reg_set_sz(k, L"ProxyServer", server) &&
                    reg_set_sz(k, L"ProxyOverride", ov) &&
                    reg_set_sz(k, L"AutoConfigURL", autoconfig);
    RegCloseKey(k);
    if (!ok) {
        st.ok = false;
        st.reason = fmt("写注册表失败: %s", win_err_text(GetLastError()).c_str());
        log_write("WARN", st.reason);
        return st;
    }
    notify_change();
    return read_wininet();
}

// ============================================================ 提权
bool is_admin() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0,
                                  0, 0, 0, 0, &group)) {
        return false;
    }
    if (!CheckTokenMembership(nullptr, group, &admin)) admin = FALSE;
    FreeSid(group);
    return admin != FALSE;
}

bool elevate(const std::string& args) {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n == 0 || n >= std::size(exe)) {
        log_write("WARN", "elevate: 取自身路径失败");
        return false;
    }
    const std::wstring params = to_wide(args);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = nullptr;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) return false;   // 用户点了「否」：安静退出，不喷堆栈
        log_write("WARN", fmt("提权失败: %s", win_err_text(err).c_str()));
        return false;
    }
    bool ok = false;
    if (sei.hProcess != nullptr) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1;
        if (GetExitCodeProcess(sei.hProcess, &code)) ok = (code == 0);
        CloseHandle(sei.hProcess);
    }
    return ok;
}

}  // namespace svpn
