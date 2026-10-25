// CDN 选边：DoH 解析（WinHTTP）→ TLS 竞速淘汰 → 1MB Range 实测 → 落盘缓存。
// 行为基准：svpn/cdn.py + netutil.resolve_merged / race_tls / download_bench。
// 缓存文件与 Python 版共用 runtime/cdn-cache.json，格式必须逐字节兼容。
#include "svpn.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace svpn {
namespace {

// ============================================================ 常量
// 与 Python netutil.DOH_PROVIDERS 同源（只保留国内可达的几家）
const char* const kProviders[] = {
    "https://doh.pub/dns-query",
    "https://dns.alidns.com/dns-query",
    "https://1.12.12.12/dns-query",
    "https://223.5.5.5/dns-query",
};

// 域名 → 测速真实路径（空 = 只测握手/HEAD）
struct PathEntry {
    const char* domain;
    const char* path;
};
const PathEntry kPaths[] = {
    {"steamcdn-a.akamaihd.net", "/client/installer/SteamSetup.exe"},
    {"cdn.akamai.steamstatic.com", "/client/installer/SteamSetup.exe"},
    {"client-update.akamai.steamstatic.com", ""},
    {"shared.akamai.steamstatic.com", ""},
    {"store.akamai.steamstatic.com", ""},
    {"community.akamai.steamstatic.com", ""},
    {"media.steampowered.com", ""},
    {"avatars.steamstatic.com", ""},
};

constexpr int kBenchBytes = 1048576;          // 1MB
constexpr std::size_t kMaxCandidates = 6;     // 每域名最多探测/测速的候选数
constexpr std::size_t kMaxBench = 3;          // 实测前 3 名（其余只留握手成绩）
constexpr std::size_t kMaxTop = 5;            // 落盘保留的 top 条数
constexpr std::size_t kRefreshWorkers = 4;    // refresh 并发上限

// ============================================================ 小工具
double now_sec() {
    using clock = std::chrono::system_clock;
    const auto tp = clock::now().time_since_epoch();
    return std::chrono::duration<double>(tp).count();
}

double round_to(double v, double scale) {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * scale) / scale;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 严格 IPv4 点分四段（拒绝 "1.2.3"、前导零、越界）——DoH 的 A 记录只收这种
bool ipv4_ok(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int parts = 0;
    std::size_t i = 0;
    for (;;) {
        const std::size_t dot = s.find('.', i);
        const std::size_t end = dot == std::string::npos ? s.size() : dot;
        const std::size_t len = end - i;
        if (len == 0 || len > 3) return false;
        if (len > 1 && s[i] == '0') return false;      // 前导零：Python ipaddress 也拒
        int val = 0;
        for (std::size_t k = i; k < end; ++k) {
            const char c = s[k];
            if (c < '0' || c > '9') return false;
            val = val * 10 + (c - '0');
        }
        if (val > 255) return false;
        // 每个八位组只计一次（老实现遇到点在循环尾又 ++parts，
        // 于是 "23.1.2.3" 这种合法地址在第 3 段就被判成 5 段直接丢弃）
        if (++parts > 4) return false;
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return parts == 4;
}

// 域名只允许 DNS 合法字符：既防 JSON 注入，也省一次必然失败的 DoH 超时
bool domain_ok(const std::string& d) {
    if (d.empty() || d.size() > 253) return false;
    for (char c : d) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string host_of_url(const std::string& url) {
    std::size_t b = url.find("://");
    b = (b == std::string::npos) ? 0 : b + 3;
    std::size_t e = url.find('/', b);
    if (e == std::string::npos) e = url.size();
    return url.substr(b, e - b);
}

std::string path_of_domain(const std::string& domain) {
    for (const auto& e : kPaths) {
        if (domain == e.domain) return e.path;
    }
    return std::string();
}

// 有界并发：count 个任务，最多 workers 条线程
void run_pool(std::size_t count, std::size_t workers,
              const std::function<void(std::size_t)>& job) {
    if (count == 0) return;
    if (workers < 1) workers = 1;
    if (workers > count) workers = count;
    if (workers == 1) {
        for (std::size_t i = 0; i < count; ++i) job(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&next, count, &job]() {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= count) return;
                // 跨线程边界不许抛：任务内部再出错也只记录
                try {
                    job(i);
                } catch (...) {
                    return;
                }
            }
        });
    }
    for (auto& t : pool) t.join();
}

// ============================================================ 文件
std::string read_file(const std::filesystem::path& p) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || f == nullptr) return std::string();
    std::string out;
    char buf[8192];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        out.append(buf, n);
        if (out.size() > (8u << 20)) break;   // 8MB 上限：缓存文件不该更大
    }
    std::fclose(f);
    return out;
}

bool write_file_atomic(const std::filesystem::path& target, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::path tmp = target;
    tmp += L".tmp";
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.wstring().c_str(), L"wb") != 0 || f == nullptr) {
        log_write("WARN", fmt("缓存临时文件打不开: %s", tmp.string().c_str()));
        return false;
    }
    const std::size_t n = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
    const int flushed = std::fclose(f);
    if (n != text.size() || flushed != 0) {
        DeleteFileW(tmp.wstring().c_str());
        log_write("WARN", fmt("缓存写入不完整: %s", tmp.string().c_str()));
        return false;
    }
    // 原子替换：先落 .tmp，再 MoveFileEx(REPLACE_EXISTING)
    if (!MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.wstring().c_str());
        log_write("WARN", fmt("缓存替换失败(%lu): %s",
                              static_cast<unsigned long>(GetLastError()),
                              target.string().c_str()));
        return false;
    }
    return true;
}

// ============================================================ WinHTTP
struct HttpResponse {
    int status{0};
    std::string body;
    std::string err;
};

HttpResponse http_call(const std::string& method, const std::string& url, const std::string& body,
                       const std::string& content_type, int timeout_ms) {
    HttpResponse out;
    if (url.empty()) {
        out.err = "provider 为空";
        return out;
    }
    if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
    if (timeout_ms < 1000) timeout_ms = 1000;

    std::wstring wurl = widen(url);
    std::vector<wchar_t> ubuf(wurl.begin(), wurl.end());
    ubuf.push_back(L'\0');

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(ubuf.data(), 0, 0, &uc)) {
        out.err = fmt("provider URL 非法(%lu)", static_cast<unsigned long>(GetLastError()));
        return out;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        out.err = "provider 必须是 https";
        return out;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring target(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) target.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    HINTERNET ses = WinHttpOpen(L"svpn/1.0 (cdn)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) {
        out.err = fmt("WinHttpOpen 失败(%lu)", static_cast<unsigned long>(GetLastError()));
        return out;
    }
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET con = WinHttpConnect(ses, host.c_str(), uc.nPort, 0);
    if (con == nullptr) {
        out.err = fmt("DoH 连接失败(%lu)", static_cast<unsigned long>(GetLastError()));
        WinHttpCloseHandle(ses);
        return out;
    }

    std::wstring wmethod = widen(method);
    HINTERNET req = WinHttpOpenRequest(con, wmethod.c_str(), target.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (req == nullptr) {
        out.err = fmt("DoH 建请求失败(%lu)", static_cast<unsigned long>(GetLastError()));
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return out;
    }

    std::wstring headers = L"Accept: application/dns-json\r\nUser-Agent: svpn/1.0\r\n";
    if (!content_type.empty()) headers += widen(fmt("Content-Type: %s\r\n", content_type.c_str()));

    const DWORD body_len = static_cast<DWORD>(body.size());
    BOOL sent = WinHttpSendRequest(
        req, headers.c_str(), static_cast<DWORD>(-1),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        body_len, body_len, 0);
    if (!sent) {
        out.err = fmt("DoH 发送失败(%lu)", static_cast<unsigned long>(GetLastError()));
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        out.err = fmt("DoH 无响应(%lu)", static_cast<unsigned long>(GetLastError()));
    } else {
        DWORD code = 0;
        DWORD len = sizeof code;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX)) {
            out.status = static_cast<int>(code);
        }
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
            out.body.append(chunk.data(), got);
            if (out.body.size() > (1u << 20)) break;   // DoH 响应不该超过 1MB
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return out;
}

// ============================================================ 选边细节
// 单个边缘：握手（复用 race 阶段的耗时）+ 可选 1MB 实测
Edge bench_edge(const std::string& domain, const std::string& ip, const std::string& path,
                double probe_ms, int timeout_ms) {
    Edge e;
    e.ip = ip;
    e.ms = probe_ms;

    socket_t s = tcp_connect_ip(ip, 443, timeout_ms);
    if (s == kBadSock) {
        e.err = "CONNECT";
        return e;
    }
    const std::int64_t t0 = now_ms();
    std::string terr;
    TlsConn tls = tls_connect(s, domain, timeout_ms, &terr);
    if (!tls.ok) {
        e.err = terr.empty() ? "TLS" : terr;
        tls.close();              // 失败也由 TlsConn 收尾（释放 SChannel 状态 + 关 socket）
        return e;
    }
    e.ms = static_cast<double>(now_ms() - t0);

    if (path.empty()) {
        // 没真实路径：只发 HEAD 看状态码，200/301/302/403/404 都算活着
        const std::string req = "HEAD / HTTP/1.1\r\nHost: " + domain +
                                "\r\nUser-Agent: Valve/Steam HTTP Client 1.0\r\n"
                                "Accept: */*\r\nConnection: close\r\n\r\n";
        if (tls.send(req.data(), req.size()) != static_cast<int>(req.size())) {
            e.err = "SEND";
            tls.close();
            return e;
        }
        std::string buf;
        char tmp[4096];
        while (buf.find("\r\n\r\n") == std::string::npos) {
            const int n = tls.recv(tmp, sizeof tmp);
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
            if (buf.size() > 65536) break;
        }
        const std::size_t eol = buf.find("\r\n");
        const std::string status = buf.substr(0, eol == std::string::npos ? buf.size() : eol);
        const std::size_t sp = status.find(' ');
        int code = 0;
        if (sp != std::string::npos) code = std::atoi(status.c_str() + sp + 1);
        e.code = code;
        const bool alive = code == 200 || code == 301 || code == 302 || code == 403 || code == 404;
        e.err = alive ? "" : fmt("HTTP %d", code);
        tls.close();
        return e;
    }

    const std::string req = fmt(
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Valve/Steam HTTP Client 1.0\r\n"
        "Range: bytes=0-%d\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        path.c_str(), domain.c_str(), kBenchBytes - 1);
    const std::int64_t tb = now_ms();
    if (tls.send(req.data(), req.size()) != static_cast<int>(req.size())) {
        e.err = "SEND";
        tls.close();
        return e;
    }

    std::string buf;
    char tmp[16384];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        const int n = tls.recv(tmp, sizeof tmp);
        if (n <= 0) {
            e.err = "无响应";
            tls.close();
            return e;
        }
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.size() > 65536) break;
    }
    const std::size_t split = buf.find("\r\n\r\n");
    const std::string head = buf.substr(0, split == std::string::npos ? buf.size() : split);
    const std::size_t eol = head.find("\r\n");
    const std::string status = head.substr(0, eol == std::string::npos ? head.size() : eol);
    const std::size_t sp = status.find(' ');
    int code = 0;
    if (sp != std::string::npos) code = std::atoi(status.c_str() + sp + 1);
    e.code = code;
    if (code != 200 && code != 206) {
        e.err = fmt("HTTP %d", code);
        // 响应体没意义，但把已收到的字节数当信息丢掉也无妨
        tls.close();
        return e;
    }

    std::size_t got = split == std::string::npos ? 0 : buf.size() - (split + 4);
    while (got < static_cast<std::size_t>(kBenchBytes)) {
        const std::size_t want =
            std::min<std::size_t>(65536, static_cast<std::size_t>(kBenchBytes) - got);
        const int n = tls.recv(tmp, std::min<std::size_t>(want, sizeof tmp));
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    const double elapsed = std::max(static_cast<double>(now_ms() - tb) / 1000.0, 1e-6);
    e.mbps = (static_cast<double>(got) / 1048576.0) / elapsed;
    e.err.clear();
    tls.close();
    return e;
}

}  // namespace

// ============================================================ Edge
double Edge::score() const {
    // 越小越优：(0=有吞吐, 1e6=没吞吐) → 吞吐降序 → 握手延迟升序
    const bool fast = mbps > 0;
    const double group = fast ? 0.0 : 1.0e6;
    const double tput = fast ? -mbps * 1000.0 : 0.0;
    return group + tput + ms * 0.001;
}

// ============================================================ DoH
bool doh_query(const std::string& provider, const std::string& domain, int timeout_ms,
               std::vector<std::string>* ips, std::string* err) {
    if (ips != nullptr) ips->clear();
    if (err != nullptr) err->clear();
    auto fail = [err](const std::string& msg) {
        if (err != nullptr) *err = msg;
        return false;
    };
    if (!domain_ok(domain)) return fail("域名非法");
    if (provider.empty()) return fail("provider 为空");

    const std::string payload = "{\"name\":\"" + domain + "\",\"type\":\"A\"}";
    const int budget = timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs;

    // 必须是 GET：doh.pub 对 POST 直接回 502、alidns 回 400（实测），
    // 而两家都认 GET + ?name=&type=A（Python 版就是这么发的）。
    std::string url = provider;
    url += (url.find('?') == std::string::npos) ? '?' : '&';
    url += "name=" + domain + "&type=A";
    HttpResponse r = http_call("GET", url, std::string(), std::string(), budget);
    // 少数 provider 只认 RFC 8484 的 POST；GET 被拒时再补一次
    if (r.status == 400 || r.status == 404 || r.status == 405 || r.status == 415) {
        r = http_call("POST", provider, payload, "application/dns-json", budget);
    }
    if (r.status == 0) return fail(r.err.empty() ? std::string("DoH 请求失败") : r.err);
    if (r.status != 200) return fail(fmt("DoH HTTP %d", r.status));

    std::string jerr;
    const Json root = Json::parse(r.body, &jerr);
    if (!jerr.empty()) return fail("DoH 响应不是合法 JSON");
    if (root.type != JType::Obj) return fail("DoH 响应不是 JSON 对象");

    const Json* status = root.get("Status");
    const int dns_status = status != nullptr ? static_cast<int>(status->as_num(0)) : 0;
    if (dns_status != 0) return fail(fmt("DoH Status=%d", dns_status));

    const Json* answers = root.get("Answer");
    if (answers == nullptr || answers->is_null()) {
        std::string head = r.body.substr(0, 120);
        for (char& c : head) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        }
        return fail(fmt("DoH 无 A 记录（%zu 字节: %s）", r.body.size(), head.c_str()));
    }
    const std::vector<Json>* list = answers->as_arr();
    if (list == nullptr) return fail("Answer 不是列表");

    std::vector<std::string> out;
    for (const Json& ans : *list) {
        if (ans.type != JType::Obj) continue;
        const Json* type = ans.get("type");
        if (type == nullptr || static_cast<int>(type->as_num(0)) != 1) continue;   // 1 = A
        const Json* data = ans.get("data");
        if (data == nullptr || data->type != JType::Str) continue;
        const std::string ip = trim(data->str);
        if (!ipv4_ok(ip)) continue;                     // CNAME / 空串 / 脏数据
        if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    }
    if (out.empty()) return fail("DoH 无 A 记录");
    if (ips != nullptr) *ips = std::move(out);
    return true;
}

// ============================================================ 选边
DomainResult rank_domain(const std::string& domain, bool bench, int timeout_ms) {
    DomainResult res;
    res.domain = domain;
    res.ts = static_cast<std::int64_t>(std::time(nullptr));
    if (timeout_ms <= 0) timeout_ms = 3000;

    std::string last_err;
    std::string source = "none";
    std::vector<std::string> candidates;
    const int doh_timeout = timeout_ms + 2000;          // Python: resolve(timeout+2)

    for (const char* provider : kProviders) {
        std::vector<std::string> got;
        std::string e;
        if (!doh_query(provider, domain, doh_timeout, &got, &e) || got.empty()) {
            if (last_err.empty()) last_err = e;
            continue;
        }
        candidates = std::move(got);
        source = "DoH:" + host_of_url(provider);
        break;
    }

    // 系统解析：可能被污染，但往往给到就近节点，并进来交给 TLS 竞速淘汰
    std::vector<std::string> sys_ips;
    for (const std::string& ip : resolve_system(domain, timeout_ms)) {
        if (!ipv4_ok(ip)) continue;                      // Python 只取 AF_INET
        if (std::find(candidates.begin(), candidates.end(), ip) == candidates.end()) {
            sys_ips.push_back(ip);
        }
    }
    const bool did_doh = !candidates.empty();
    for (const std::string& ip : sys_ips) candidates.push_back(ip);
    if (did_doh && !sys_ips.empty()) {
        source += "+system";
    } else if (!did_doh && !sys_ips.empty()) {
        source = "system";
    }

    res.source = source;
    res.candidates = static_cast<int>(candidates.size());
    if (candidates.empty()) {
        log_write("WARN", fmt("CDN %s 解析失败: %s", domain.c_str(),
                              last_err.empty() ? "DoH 与系统解析都没有结果" : last_err.c_str()));
        return res;
    }

    // ---- TLS 竞速：只留证书/协议都对的边缘
    const std::size_t probe_n = std::min(kMaxCandidates, candidates.size());
    std::vector<TlsProbe> probes(probe_n);
    const int probe_timeout = std::min(timeout_ms, 3000);
    run_pool(probe_n, probe_n, [&](std::size_t i) {
        probes[i] = tls_probe(candidates[i], domain, probe_timeout);
    });
    std::vector<Edge> picks;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        if (!probes[i].ok) continue;
        Edge e;
        e.ip = candidates[i];
        e.ms = probes[i].ms;
        picks.push_back(std::move(e));
    }
    std::sort(picks.begin(), picks.end(), [](const Edge& a, const Edge& b) { return a.ms < b.ms; });
    if (picks.empty()) {
        std::string why;
        for (std::size_t i = 0; i < probes.size() && i < 2; ++i) {
            if (!why.empty()) why += "; ";
            why += candidates[i] + " -> " + (probes[i].err.empty() ? "no err" : probes[i].err);
        }
        log_write("WARN", fmt("CDN %s 候选 %zu 个，TLS 全部不可达（%s）", domain.c_str(),
                              candidates.size(), why.c_str()));
        return res;
    }

    if (!bench) {
        res.tested = static_cast<int>(picks.size());
        res.top = picks;
        if (res.top.size() > kMaxTop) res.top.resize(kMaxTop);
        res.best_ip = picks.front().ip;
        res.best_ms = round_to(picks.front().ms, 10);
        return res;
    }

    // ---- 实测：前 3 名拉 1MB，其余只保留握手成绩（排序时自然沉底）
    std::vector<Edge> edges(picks.size());
    const std::string path = path_of_domain(domain);
    const int bench_timeout = timeout_ms + 5000;        // Python: timeout + 5
    const std::size_t bench_n = std::min(kMaxBench, picks.size());
    run_pool(bench_n, bench_n, [&](std::size_t i) {
        edges[i] = bench_edge(domain, picks[i].ip, path, picks[i].ms, bench_timeout);
    });
    for (std::size_t i = bench_n; i < picks.size(); ++i) {
        edges[i] = picks[i];
        edges[i].mbps = 0;
    }
    std::stable_sort(edges.begin(), edges.end(),
                     [](const Edge& a, const Edge& b) { return a.score() < b.score(); });

    res.tested = static_cast<int>(edges.size());
    res.best_ip = edges.front().ip;
    res.best_ms = round_to(edges.front().ms, 10);
    res.best_mbps = round_to(edges.front().mbps, 100);
    res.top.assign(edges.begin(),
                   edges.begin() + static_cast<std::ptrdiff_t>(std::min(kMaxTop, edges.size())));
    return res;
}

Cache refresh(const std::vector<std::string>& domains, bool bench, int timeout_ms) {
    std::vector<std::string> names;
    if (domains.empty()) {
        names.assign(kCdnDomains, kCdnDomains + kCdnDomains_count);
    } else {
        for (const std::string& d : domains) {
            if (!domain_ok(d)) continue;
            if (std::find(names.begin(), names.end(), d) == names.end()) names.push_back(d);
        }
    }
    Cache out;
    if (names.empty()) return out;
    std::mutex mu;
    run_pool(names.size(), kRefreshWorkers, [&](std::size_t i) {
        DomainResult r = rank_domain(names[i], bench, timeout_ms);
        std::lock_guard<std::mutex> lk(mu);
        out[r.domain.empty() ? names[i] : r.domain] = std::move(r);
    });
    return out;
}

// ============================================================ 缓存
Cache load_cache(const Paths& p, double max_age_s) {
    Cache out;
    std::error_code ec;
    if (!std::filesystem::exists(p.cdn_cache, ec)) return out;
    const std::string text = read_file(p.cdn_cache);
    if (text.empty()) return out;

    std::string jerr;
    const Json root = Json::parse(text, &jerr);
    if (!jerr.empty() || root.type != JType::Obj) return out;

    const Json* upd = root.get("updated");
    const double updated = upd != nullptr ? upd->as_num(0) : 0.0;
    if (now_sec() - updated >= max_age_s) return out;      // 与 Python 一样：过期就整份作废

    const Json* doms = root.get("domains");
    if (doms == nullptr || doms->type != JType::Obj) return out;

    for (const auto& kv : doms->obj) {
        if (kv.second.type != JType::Obj) continue;        // null / 垃圾条目跳过
        const Json& d = kv.second;
        DomainResult r;
        r.domain = kv.first;
        r.source = d.get("source") != nullptr ? d.get("source")->as_str() : std::string();
        const Json* cand = d.get("candidates");
        r.candidates = cand != nullptr ? static_cast<int>(cand->as_num(0)) : 0;
        r.best_ip = d.get("best_ip") != nullptr ? d.get("best_ip")->as_str() : std::string();
        const Json* bms = d.get("best_ms");
        r.best_ms = bms != nullptr ? bms->as_num(0) : 0.0;
        const Json* bmb = d.get("best_mbps");
        r.best_mbps = bmb != nullptr ? bmb->as_num(0) : 0.0;
        const Json* tested = d.get("tested");
        r.tested = tested != nullptr ? static_cast<int>(tested->as_num(0)) : 0;
        const Json* ts = d.get("ts");
        r.ts = ts != nullptr ? static_cast<std::int64_t>(ts->as_num(0)) : 0;

        if (const Json* top = d.get("top")) {
            if (const std::vector<Json>* arr = top->as_arr()) {
                for (const Json& item : *arr) {
                    if (item.type != JType::Obj) continue;
                    Edge e;
                    e.ip = item.get("ip") != nullptr ? item.get("ip")->as_str() : std::string();
                    const Json* ms = item.get("ms");
                    e.ms = ms != nullptr ? ms->as_num(0) : 0.0;
                    const Json* mb = item.get("mbps");
                    e.mbps = mb != nullptr ? mb->as_num(0) : 0.0;
                    const Json* code = item.get("code");
                    e.code = code != nullptr ? static_cast<int>(code->as_num(0)) : 0;
                    e.err = item.get("err") != nullptr ? item.get("err")->as_str() : std::string();
                    r.top.push_back(std::move(e));
                    if (r.top.size() >= kMaxTop) break;
                }
            }
        }
        out[r.domain] = std::move(r);
    }
    return out;
}

bool save_cache(const Paths& p, const Cache& c) {
    Json root = Json::make_obj();
    // 整个秒：json.cpp 的 %.10g 对 1.7e9 只保留整数位，带小数会被四舍五入到「未来」，
    // 让 max_age=0 的判定失真；Python 的 float() 读整数没问题，格式仍兼容。
    root.set("updated", Json::make_num(std::floor(now_sec())));
    Json doms = Json::make_obj();
    for (const auto& kv : c) {
        const DomainResult& r = kv.second;
        Json d = Json::make_obj();
        d.set("domain", Json::make_str(kv.first));
        d.set("source", Json::make_str(r.source));
        d.set("candidates", Json::make_num(r.candidates));
        d.set("best_ip", Json::make_str(r.best_ip));
        d.set("best_ms", Json::make_num(round_to(r.best_ms, 10)));
        d.set("best_mbps", Json::make_num(round_to(r.best_mbps, 100)));
        d.set("tested", Json::make_num(r.tested));
        d.set("ts", Json::make_num(static_cast<double>(r.ts)));
        Json top = Json::make_arr();
        for (std::size_t i = 0; i < r.top.size() && i < kMaxTop; ++i) {
            const Edge& e = r.top[i];
            Json item = Json::make_obj();
            item.set("ip", Json::make_str(e.ip));
            item.set("ms", Json::make_num(round_to(e.ms, 10)));
            item.set("mbps", Json::make_num(round_to(e.mbps, 100)));
            item.set("code", Json::make_num(e.code));
            item.set("err", Json::make_str(e.err));
            top.push(std::move(item));
        }
        d.set("top", std::move(top));
        doms.set(kv.first, std::move(d));
    }
    root.set("domains", std::move(doms));

    if (!write_file_atomic(p.cdn_cache, root.dump(2))) return false;
    return true;
}

double cache_age_hours(const Paths& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p.cdn_cache, ec)) return -1.0;
    const std::string text = read_file(p.cdn_cache);
    if (text.empty()) return -1.0;
    std::string jerr;
    const Json root = Json::parse(text, &jerr);
    if (!jerr.empty() || root.type != JType::Obj) return -1.0;
    const Json* upd = root.get("updated");
    if (upd == nullptr) return -1.0;
    const double updated = upd->as_num(0);
    if (!(updated > 0)) return -1.0;
    const double age = (now_sec() - updated) / 3600.0;
    return age < 0.0 ? 0.0 : age;
}

std::map<std::string, std::string> best_ip_map(const Paths& p, double max_age_s) {
    std::map<std::string, std::string> out;
    for (const auto& kv : load_cache(p, max_age_s)) {
        const std::string& ip = kv.second.best_ip;
        if (!ip.empty() && is_ip(ip)) out[kv.first] = ip;   // 与 Python 一致：v4/v6 都收
    }
    return out;
}

void apply_ip_map(const Paths& p, Config& cfg, double max_age_s) {
    for (const auto& kv : best_ip_map(p, max_age_s)) cfg.ip_map[kv.first] = kv.second;
}

}  // namespace svpn
