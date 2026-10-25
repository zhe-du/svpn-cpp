// 本地代理内核：SOCKS5 + HTTP CONNECT + 绝对 URI 正向代理，支持上游链。
//
// 语义基准：Python 版 svpn/proxy.py（逐函数对照实现）。
// 设计要点
// - 线程 + 阻塞 socket + select 双向中继（relay_stream 由 net.cpp 提供）。
// - 分流：Router::decide 决定 direct / upstream；域名原样交给上游做远端 DNS。
// - 上游支持 socks5(含 RFC1929 认证) / http(CONNECT + Basic 认证) / direct。
// - 统计：连接数、上/下行字节、按域名聚合（Stats 由 util.cpp 提供）。
// - 线程模型：每连接一个 std::thread，detach 后靠 active 计数收敛；退出前先
//   shutdown 所有活动 socket 再等计数归零，绝不无限期挂住进程。
#include "svpn.hpp"

#ifndef WIN32_WINNT
#define WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svpn {
namespace {

constexpr std::size_t kMaxHead = 16 * 1024;      // HTTP 请求头硬上限（防内存打爆）
constexpr int kRelayIdleMs = 3600000;            // 中继空闲上限：1h，只是兜底
constexpr int kAcceptTickMs = 100;               // accept 循环检查 stop 的粒度
constexpr int kHttpHeadTimeoutMs = 15000;        // 与 Python 的 15s 对齐
constexpr int kDrainWaitMs = 5000;               // 退出时等活动连接收尾的上限

// ---------------------------------------------------------------- 小工具
struct SockGuard {
    socket_t s{kBadSock};
    SockGuard() = default;
    explicit SockGuard(socket_t v) : s(v) {}
    ~SockGuard() { reset(); }
    SockGuard(const SockGuard&) = delete;
    SockGuard& operator=(const SockGuard&) = delete;
    SockGuard(SockGuard&& o) noexcept : s(o.s) { o.s = kBadSock; }
    SockGuard& operator=(SockGuard&& o) noexcept {
        if (this != &o) {
            reset();
            s = o.s;
            o.s = kBadSock;
        }
        return *this;
    }
    void reset() {
        if (s != kBadSock) {
            close_socket(s);
            s = kBadSock;
        }
    }
    socket_t release() {
        socket_t t = s;
        s = kBadSock;
        return t;
    }
    bool ok() const { return s != kBadSock; }
};

std::string b64_encode(const std::string& in) {
    static const char kTbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    for (; i + 3 <= in.size(); i += 3) {
        std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                          (static_cast<std::uint32_t>(p[i + 1]) << 8) |
                          static_cast<std::uint32_t>(p[i + 2]);
        out.push_back(kTbl[(v >> 18) & 0x3f]);
        out.push_back(kTbl[(v >> 12) & 0x3f]);
        out.push_back(kTbl[(v >> 6) & 0x3f]);
        out.push_back(kTbl[v & 0x3f]);
    }
    if (i + 1 == in.size()) {
        std::uint32_t v = static_cast<std::uint32_t>(p[i]) << 16;
        out.push_back(kTbl[(v >> 18) & 0x3f]);
        out.push_back(kTbl[(v >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) |
                          (static_cast<std::uint32_t>(p[i + 1]) << 8);
        out.push_back(kTbl[(v >> 18) & 0x3f]);
        out.push_back(kTbl[(v >> 12) & 0x3f]);
        out.push_back(kTbl[(v >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

std::string upper_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

std::string strip_brackets(const std::string& h) {
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') return h.substr(1, h.size() - 2);
    return h;
}

void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

// 逐字节读到分隔符为止：绝不吞掉分隔符之后的字节（上游 CONNECT 应答专用）。
// 只在上游握手里用一次，逐字节的系统调用开销可以接受。
bool recv_until_strict(socket_t s, const char* delim, std::size_t cap, int timeout_ms,
                       std::string* out) {
    const std::string d(delim);
    std::string acc;
    acc.reserve(256);
    unsigned char c = 0;
    while (acc.size() < cap) {
        if (!recv_exact(s, &c, 1, timeout_ms)) return false;
        acc.push_back(static_cast<char>(c));
        if (acc.size() >= d.size() && ends_with(acc, d)) {
            *out = std::move(acc);
            return true;
        }
    }
    return false;
}

// 分块读到分隔符为止；多读出来的字节放进 leftover，由调用方原样转发（不丢 body）。
bool recv_until_chunked(socket_t s, const char* delim, std::size_t cap, int timeout_ms,
                        std::string* head, std::string* leftover) {
    const std::size_t dlen = std::strlen(delim);
    std::string acc;
    char buf[4096];
    for (;;) {
        const std::size_t before = acc.size();
        acc.resize(before + sizeof buf);
        int n = recv_some(s, &acc[before], sizeof buf, timeout_ms);
        if (n <= 0) {
            acc.resize(before);
            return false;
        }
        acc.resize(before + static_cast<std::size_t>(n));
        const std::size_t from = before > dlen ? before - dlen : 0;
        const std::size_t pos = acc.find(delim, from);
        if (pos != std::string::npos) {
            const std::size_t end = pos + dlen;
            *head = acc.substr(0, end);
            *leftover = acc.substr(end);
            return true;
        }
        if (acc.size() >= cap) return false;   // 头太大：当畸形处理
    }
}

bool wait_readable(socket_t s, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(static_cast<SOCKET>(s), &rd);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    const int r = ::select(0, &rd, nullptr, nullptr, &tv);
    return r > 0;
}

// ---------------------------------------------------------------- ip_map 注入
// dial_direct 的签名里没有 cfg（共享契约不可改），所以 ip_map 走模块级注册：
// serve_socks / serve_http / serve_all 打开监听时把 cfg.ip_map 整体拷进来，
// dial_direct 只在拨号瞬间读一次（读锁），因此运行期热更新也天然生效。
std::mutex g_ip_map_mu;
std::map<std::string, std::string> g_ip_map;

void publish_ip_map(const std::map<std::string, std::string>& m) {
    std::lock_guard<std::mutex> lk(g_ip_map_mu);
    g_ip_map = m;
}

std::string lookup_ip_map(const std::string& host) {
    if (host.empty()) return {};
    std::lock_guard<std::mutex> lk(g_ip_map_mu);
    auto it = g_ip_map.find(lower(host));
    if (it == g_ip_map.end()) return {};
    return it->second;
}

// ---------------------------------------------------------------- 上游可用性
bool upstream_usable(const Node& n) {
    if (n.port <= 0 || n.host.empty()) return false;
    const std::string kind = lower(n.kind);
    if (kind != "socks5" && kind != "http") return false;
    const std::string h = upper_ascii(n.host);
    // 只排真正的占位符；127.0.0.1 是本机客户端（Clash/v2rayN/sing-box）常态
    return h != "NODE_HOST" && h != "NODE" && h != "HOST" && h != "EXAMPLE.COM" &&
           h != "CHANGE_ME";
}

Node to_node(const UpstreamCfg& u) {
    Node n;
    n.kind = u.kind.empty() ? std::string("socks5") : lower(u.kind);
    n.host = u.host;
    n.port = u.port;
    n.user = u.user;
    n.pass = u.pass;
    return n;
}

// SOCKS5 目标地址编码：v4=0x01 / v6=0x04 / 域名=0x03。
// 必须区分 v4/v6：只认「数字+点」会让 ::1 掉进域名分支，上游拿 ::1 当域名解析必然失败。
struct AddrEncoding {
    std::uint8_t atyp{0x03};
    std::vector<unsigned char> bytes;
};

AddrEncoding encode_addr(const std::string& host_in) {
    const std::string h = strip_brackets(host_in);
    AddrEncoding out;
    unsigned char buf[16];
    std::memset(buf, 0, sizeof buf);
    if (!h.empty() && ::inet_pton(AF_INET, h.c_str(), buf) == 1) {
        out.atyp = 0x01;
        out.bytes.assign(buf, buf + 4);
        return out;
    }
    if (!h.empty() && ::inet_pton(AF_INET6, h.c_str(), buf) == 1) {
        out.atyp = 0x04;
        out.bytes.assign(buf, buf + 16);
        return out;
    }
    // 域名：非 ASCII 直接按 UTF-8 发（Python 用 idna，此处无第三方库，见报告）
    std::string dom = h.substr(0, 255);
    out.atyp = 0x03;
    out.bytes.assign(dom.begin(), dom.end());
    return out;
}

// ---------------------------------------------------------------- 拨号
socket_t dial_socks5_up(const Node& up, const std::string& host, int port, int timeout_ms,
                        std::string* err) {
    socket_t raw = tcp_connect(up.host, up.port, timeout_ms, err);
    if (raw == kBadSock) return kBadSock;
    SockGuard guard(raw);
    if (!set_io_timeout(raw, timeout_ms)) {
        set_err(err, "上游 SOCKS5 设置超时失败");
        return kBadSock;
    }

    // 方法协商：有账号就同时提供 0x00/0x02，没账号只提供 0x00
    unsigned char hello[4] = {0x05, 0x01, 0x00, 0x00};
    int hn = 3;
    if (!up.user.empty()) {
        hello[1] = 0x02;
        hello[2] = 0x00;
        hello[3] = 0x02;
        hn = 4;
    }
    if (!send_all(raw, hello, static_cast<std::size_t>(hn), timeout_ms)) {
        set_err(err, "上游 SOCKS5 握手写入失败");
        return kBadSock;
    }
    unsigned char rep[2] = {0, 0};
    if (!recv_exact(raw, rep, 2, timeout_ms)) {
        set_err(err, "上游 SOCKS5 无应答");
        return kBadSock;
    }
    if (rep[0] != 0x05) {
        set_err(err, fmt("上游不是 SOCKS5 (ver=%d)", static_cast<int>(rep[0])));
        return kBadSock;
    }
    if (rep[1] == 0x02) {
        // RFC1929 用户名/密码子协商
        const std::string u = up.user.substr(0, 255);
        const std::string p = up.pass.substr(0, 255);
        std::vector<unsigned char> sub;
        sub.reserve(3 + u.size() + p.size());
        sub.push_back(0x01);
        sub.push_back(static_cast<unsigned char>(u.size()));
        sub.insert(sub.end(), u.begin(), u.end());
        sub.push_back(static_cast<unsigned char>(p.size()));
        sub.insert(sub.end(), p.begin(), p.end());
        if (!send_all(raw, sub.data(), sub.size(), timeout_ms)) {
            set_err(err, "上游 SOCKS5 认证写入失败");
            return kBadSock;
        }
        unsigned char ar[2] = {0, 0};
        if (!recv_exact(raw, ar, 2, timeout_ms)) {
            set_err(err, "上游 SOCKS5 认证无应答");
            return kBadSock;
        }
        if (ar[1] != 0x00) {
            set_err(err, "上游 SOCKS5 认证失败");
            return kBadSock;
        }
    } else if (rep[1] != 0x00) {
        set_err(err, fmt("上游拒绝认证方式: %d", static_cast<int>(rep[1])));
        return kBadSock;
    }

    // CONNECT 请求
    const AddrEncoding addr = encode_addr(host);
    std::vector<unsigned char> req;
    req.reserve(7 + addr.bytes.size());
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(addr.atyp);
    if (addr.atyp == 0x03) req.push_back(static_cast<unsigned char>(addr.bytes.size()));
    req.insert(req.end(), addr.bytes.begin(), addr.bytes.end());
    req.push_back(static_cast<unsigned char>((port >> 8) & 0xff));
    req.push_back(static_cast<unsigned char>(port & 0xff));
    if (!send_all(raw, req.data(), req.size(), timeout_ms)) {
        set_err(err, "上游 SOCKS5 CONNECT 写入失败");
        return kBadSock;
    }
    unsigned char head[4] = {0, 0, 0, 0};
    if (!recv_exact(raw, head, 4, timeout_ms)) {
        set_err(err, "上游 SOCKS5 CONNECT 无应答");
        return kBadSock;
    }
    if (head[1] != 0x00) {
        set_err(err, fmt("上游 CONNECT 失败 code=%d", static_cast<int>(head[1])));
        return kBadSock;
    }
    // 吃掉 BND.ADDR + BND.PORT，一个字节都不许多读
    if (head[3] == 0x01) {
        unsigned char tail[6];
        if (!recv_exact(raw, tail, sizeof tail, timeout_ms)) {
            set_err(err, "上游 SOCKS5 回复截断");
            return kBadSock;
        }
    } else if (head[3] == 0x03) {
        unsigned char ln = 0;
        if (!recv_exact(raw, &ln, 1, timeout_ms)) {
            set_err(err, "上游 SOCKS5 回复截断");
            return kBadSock;
        }
        std::vector<unsigned char> tail(static_cast<std::size_t>(ln) + 2);
        if (!recv_exact(raw, tail.data(), tail.size(), timeout_ms)) {
            set_err(err, "上游 SOCKS5 回复截断");
            return kBadSock;
        }
    } else if (head[3] == 0x04) {
        unsigned char tail[18];
        if (!recv_exact(raw, tail, sizeof tail, timeout_ms)) {
            set_err(err, "上游 SOCKS5 回复截断");
            return kBadSock;
        }
    }
    return guard.release();
}

socket_t dial_http_up(const Node& up, const std::string& host, int port, int timeout_ms,
                      std::string* err) {
    socket_t raw = tcp_connect(up.host, up.port, timeout_ms, err);
    if (raw == kBadSock) return kBadSock;
    SockGuard guard(raw);
    if (!set_io_timeout(raw, timeout_ms)) {
        set_err(err, "上游 HTTP 设置超时失败");
        return kBadSock;
    }

    const std::string target = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    std::string auth;
    if (!up.user.empty()) {
        auth = "Proxy-Authorization: Basic " + b64_encode(up.user + ":" + up.pass) + "\r\n";
    }
    const std::string req = "CONNECT " + target + ":" + std::to_string(port) +
                            " HTTP/1.1\r\nHost: " + target + ":" + std::to_string(port) +
                            "\r\n" + auth + "Proxy-Connection: keep-alive\r\n\r\n";
    if (!send_all(raw, req.data(), req.size(), timeout_ms)) {
        set_err(err, "上游 HTTP CONNECT 写入失败");
        return kBadSock;
    }
    std::string head;
    if (!recv_until_strict(raw, "\r\n\r\n", kMaxHead, timeout_ms, &head)) {
        set_err(err, "上游 HTTP CONNECT 无完整应答");
        return kBadSock;
    }
    const std::size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol == std::string::npos ? head.size() : eol);
    // 状态行：HTTP/1.x <code> <reason>
    std::size_t sp = line.find(' ');
    std::string code;
    if (sp != std::string::npos) {
        const std::size_t sp2 = line.find(' ', sp + 1);
        code = line.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);
    }
    // 与 Python 一致：只看首位是否 '2'（200/201/204 都算隧道建立）
    if (code.empty() || code[0] != '2') {
        set_err(err, fmt("上游 HTTP CONNECT 失败: %s", line.c_str()));
        return kBadSock;
    }
    // 应答头之后若夹带字节（本场景不应出现，无 TLS），这里逐字节读，不会越界
    return guard.release();
}

}  // namespace

// ================================================================ dial_*
socket_t dial_direct(const std::string& host_in, int port, int timeout_ms, std::string* err) {
    const std::string host = strip_brackets(host_in);
    if (host.empty()) {
        set_err(err, "目标主机为空");
        return kBadSock;
    }
    if (port <= 0 || port > 65535) {
        set_err(err, fmt("目标端口非法: %d", port));
        return kBadSock;
    }
    // CDN 选边结果（ip_map）优先：缓存命中就直连 IP，失败再回落正常解析
    const std::string hint = is_ip(host) ? std::string() : lookup_ip_map(host);
    if (!hint.empty()) {
        socket_t s = tcp_connect_ip(strip_brackets(hint), port, timeout_ms);
        if (s != kBadSock) return s;
        log_write("DEBUG", fmt("ip_map %s->%s 直连失败，回退解析", host.c_str(), hint.c_str()));
    }
    return tcp_connect(host, port, timeout_ms, err);
}

socket_t dial_via(const Node& up, const std::string& host, int port, int timeout_ms,
                  std::string* err) {
    const std::string kind = lower(up.kind);
    if (kind == "socks5" && upstream_usable(up)) {
        return dial_socks5_up(up, host, port, timeout_ms, err);
    }
    if (kind == "http" && upstream_usable(up)) {
        return dial_http_up(up, host, port, timeout_ms, err);
    }
    // kind=direct / 上游是占位符 / 参数不全：一律直连（与 Python dial() 一致）
    return dial_direct(host, port, timeout_ms, err);
}

// ================================================================ 服务内核
namespace {

struct Core {
    const Config& cfg;
    Stats& st;
    std::atomic<bool>& stop;
    std::atomic<bool>& abort;            // 任一入口退出即整体收摊（serve_all 两个入口共享）
    Router router;
    Node up;
    int timeout_ms;
    bool auth_required;
    std::string auth_user, auth_pass;

    std::mutex mu;                       // 保护 active / live
    std::condition_variable cv;
    int active{0};
    std::vector<socket_t> live;

    Core(const Config& c, Stats& s, std::atomic<bool>& sp, std::atomic<bool>& ab)
        : cfg(c),
          st(s),
          stop(sp),
          abort(ab),
          router(c),
          up(to_node(c.upstream)),
          timeout_ms(c.connect_timeout > 0 ? c.connect_timeout * 1000 : kDefaultTimeoutMs),
          auth_required(!c.auth.user.empty()),
          auth_user(c.auth.user),
          auth_pass(c.auth.pass) {}

    void add_live(socket_t s) {
        std::lock_guard<std::mutex> lk(mu);
        live.push_back(s);
    }
    void del_live(socket_t s) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = std::find(live.begin(), live.end(), s);
        if (it != live.end()) live.erase(it);
    }
    // 退出时把所有活动 socket 半关（shutdown），阻塞中的 recv 立刻返回，线程自然收尾
    void shutdown_live() {
        std::lock_guard<std::mutex> lk(mu);
        for (socket_t s : live) ::shutdown(static_cast<SOCKET>(s), SD_BOTH);
    }
    void begin() {
        std::lock_guard<std::mutex> lk(mu);
        ++active;
    }
    void finish() {
        std::lock_guard<std::mutex> lk(mu);
        if (--active <= 0) cv.notify_all();
    }
    void wait_idle(int ms) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return active == 0; });
    }
};

bool socks_reply(socket_t s, std::uint8_t rep, int timeout_ms) {
    // BND.ADDR/BND.PORT 全 0 是允许的（RFC1928 明确认可）
    const unsigned char buf[10] = {0x05, rep, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    return send_all(s, buf, sizeof buf, timeout_ms);
}

bool http_simple(socket_t s, const char* text, int timeout_ms) {
    const std::size_t n = std::strlen(text);
    return send_all(s, text, n, timeout_ms);
}

void bad_request(socket_t s, int timeout_ms) {
    http_simple(s, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", timeout_ms);
}

void bad_gateway(socket_t s, int timeout_ms) {
    http_simple(s, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n", timeout_ms);
}

std::uint8_t rep_for(const std::string& err) {
    const std::string e = lower(err);
    const auto has = [&e](const char* needle) { return e.find(needle) != std::string::npos; };
    if (has("refused") || has("10061") || has("拒绝")) return 0x05;                 // 连接被拒
    if (has("net unreachable") || has("network unreachable") || has("10051") ||
        has("网络不可达")) {
        return 0x03;                                                              // 网络不可达
    }
    return 0x04;                                                                  // 主机不可达
}

// 统一接管一条连接的收尾：中继 + 字节统计
void do_relay(Core& core, socket_t client, socket_t remote) {
    SockGuard rg(remote);
    std::uint64_t u = 0, d = 0;
    (void)relay_stream(client, remote, kRelayIdleMs, &u, &d);
    core.st.up_bytes += u;
    core.st.down_bytes += d;
}

// 分流 + 拨号。返回 socket，失败返回 kBadSock 并写 err / 统计 errors。
socket_t route_dial(Core& core, const std::string& host, int port, std::string* err) {
    std::string decision = core.router.decide(host);
    bool use_upstream = false;
    if (decision == "upstream") {
        if (upstream_usable(core.up)) {
            use_upstream = true;
        } else {
            // 契约要求：没配上游时退回直连并留一条 WARN，别让用户以为在走节点
            log_write("WARN", fmt("上游未配置，%s:%d 改走直连", host.c_str(), port));
        }
    }
    if (use_upstream) {
        core.st.via_upstream++;
        return dial_via(core.up, host, port, core.timeout_ms, err);
    }
    core.st.via_direct++;
    return dial_direct(host, port, core.timeout_ms, err);
}

// ---------------------------------------------------------------- SOCKS5 会话
void socks_session(Core& core, socket_t cs) {
    const int to = core.timeout_ms;
    if (!set_io_timeout(cs, to)) return;

    unsigned char greet[2] = {0, 0};
    if (!recv_exact(cs, greet, 2, to)) return;
    if (greet[0] != 0x05) return;
    const int nmethods = greet[1];
    if (nmethods <= 0) {
        const unsigned char no[2] = {0x05, 0xff};
        send_all(cs, no, 2, to);
        return;
    }
    std::vector<unsigned char> methods(static_cast<std::size_t>(nmethods));
    if (!recv_exact(cs, methods.data(), methods.size(), to)) return;
    bool have_noauth = false, have_userpass = false;
    for (unsigned char m : methods) {
        if (m == 0x00) have_noauth = true;
        if (m == 0x02) have_userpass = true;
    }

    if (core.auth_required) {
        if (!have_userpass) {
            const unsigned char no[2] = {0x05, 0xff};
            send_all(cs, no, 2, to);
            return;
        }
        const unsigned char yes[2] = {0x05, 0x02};
        if (!send_all(cs, yes, 2, to)) return;
        // RFC1929：ver(1) ulen(1) uname plen(1) passwd
        unsigned char vb = 0, ulen = 0, plen = 0;
        if (!recv_exact(cs, &vb, 1, to)) return;
        if (!recv_exact(cs, &ulen, 1, to)) return;
        std::string user(ulen, '\0');
        if (ulen && !recv_exact(cs, user.data(), ulen, to)) return;
        if (!recv_exact(cs, &plen, 1, to)) return;
        std::string pass(plen, '\0');
        if (plen && !recv_exact(cs, pass.data(), plen, to)) return;
        const bool ok = (user == core.auth_user && pass == core.auth_pass);
        const unsigned char res[2] = {0x01, static_cast<unsigned char>(ok ? 0x00 : 0x01)};
        send_all(cs, res, 2, to);
        if (!ok) {
            log_write("WARN", "SOCKS5 本地认证失败");
            core.st.errors++;
            return;
        }
    } else {
        if (!have_noauth) {
            const unsigned char no[2] = {0x05, 0xff};
            send_all(cs, no, 2, to);
            return;
        }
        const unsigned char yes[2] = {0x05, 0x00};
        if (!send_all(cs, yes, 2, to)) return;
    }

    unsigned char head[4] = {0, 0, 0, 0};
    if (!recv_exact(cs, head, 4, to)) return;
    if (head[0] != 0x05) return;
    const std::uint8_t cmd = head[1];
    const std::uint8_t atyp = head[3];
    if (cmd != 0x01) {                       // 只支持 CONNECT
        socks_reply(cs, 0x07, to);
        log_write("DEBUG", fmt("SOCKS5 不支持的命令 0x%02x", static_cast<int>(cmd)));
        return;
    }

    std::string host;
    if (atyp == 0x01) {
        unsigned char ip[4];
        if (!recv_exact(cs, ip, 4, to)) return;
        char text[INET_ADDRSTRLEN] = {0};
        if (!::inet_ntop(AF_INET, ip, text, sizeof text)) return;
        host = text;
    } else if (atyp == 0x03) {
        unsigned char ln = 0;
        if (!recv_exact(cs, &ln, 1, to)) return;
        std::string dom(ln, '\0');
        if (ln && !recv_exact(cs, dom.data(), ln, to)) return;
        host = dom;
    } else if (atyp == 0x04) {
        unsigned char ip[16];
        if (!recv_exact(cs, ip, 16, to)) return;
        char text[INET6_ADDRSTRLEN] = {0};
        if (!::inet_ntop(AF_INET6, ip, text, sizeof text)) return;
        host = text;
    } else {
        socks_reply(cs, 0x08, to);           // 地址类型不支持
        return;
    }
    unsigned char pb[2] = {0, 0};
    if (!recv_exact(cs, pb, 2, to)) return;
    const int port = (static_cast<int>(pb[0]) << 8) | static_cast<int>(pb[1]);

    core.st.conns++;
    core.st.hit(host);

    std::string err;
    socket_t remote = route_dial(core, host, port, &err);
    if (remote == kBadSock) {
        core.st.errors++;
        log_write("WARN", fmt("socks dial fail %s:%d %s", host.c_str(), port, err.c_str()));
        socks_reply(cs, rep_for(err), to);
        return;
    }
    if (!socks_reply(cs, 0x00, to)) {
        core.st.errors++;
        close_socket(remote);
        return;
    }
    log_write("DEBUG", fmt("socks %s:%d -> pipe", host.c_str(), port));
    do_relay(core, cs, remote);
}

// ---------------------------------------------------------------- HTTP 会话
struct HttpHead {
    std::string method, target, version;
    std::vector<std::string> lines;             // 原始头行（不含 CRLF），保持顺序
};

bool parse_head(const std::string& raw, HttpHead* out) {
    // 去掉末尾的 CRLFCRLF，保留头部原文
    std::string body = raw;
    while (body.size() >= 2 && body.compare(body.size() - 2, 2, "\r\n") == 0) {
        body.resize(body.size() - 2);
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = body.find("\r\n", start);
        if (pos == std::string::npos) {
            lines.push_back(body.substr(start));
            break;
        }
        lines.push_back(body.substr(start, pos - start));
        start = pos + 2;
    }
    if (lines.empty() || lines[0].empty()) return false;
    const std::string& req = lines[0];
    const std::size_t sp1 = req.find(' ');
    if (sp1 == std::string::npos) return false;
    const std::size_t sp2 = req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    out->method = req.substr(0, sp1);
    out->target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    out->version = req.substr(sp2 + 1);
    if (out->method.empty() || out->target.empty() || out->version.empty()) return false;
    out->lines.assign(lines.begin() + 1, lines.end());
    return true;
}

std::string header_value(const std::vector<std::string>& lines, const std::string& key_lower) {
    for (const std::string& ln : lines) {
        const std::size_t colon = ln.find(':');
        if (colon == std::string::npos) continue;
        if (lower(trim(ln.substr(0, colon))) == key_lower) return trim(ln.substr(colon + 1));
    }
    return {};
}

// "host:port" → 主机 + 端口。失败返回 false（调用方回 400）。
bool split_host_port(const std::string& hostport, int default_port, std::string* host,
                     int* port) {
    std::string hp = hostport;
    std::string ps;
    // IPv6：[::1]:8080
    if (!hp.empty() && hp.front() == '[') {
        const std::size_t close = hp.find(']');
        if (close == std::string::npos) return false;
        *host = strip_brackets(hp.substr(0, close + 1));
        const std::string rest = hp.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            ps = rest.substr(1);
        }
    } else {
        const std::size_t colon = hp.rfind(':');
        if (colon == std::string::npos) {
            *host = hp;
        } else {
            // 裸 IPv6（多个冒号）不按 host:port 拆
            if (hp.find(':') != colon) {
                *host = hp;
            } else {
                *host = hp.substr(0, colon);
                ps = hp.substr(colon + 1);
            }
        }
    }
    *host = strip_brackets(trim(*host));
    if (ps.empty()) {
        *port = default_port;
        return true;
    }
    // 只接受纯数字，且不越界（Python 的 int() 也拒绝 "12 3"/"abc"）
    if (ps.size() > 5) return false;
    int v = 0;
    for (char c : ps) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v <= 0 || v > 65535) return false;
    *port = v;
    return true;
}

void http_session(Core& core, socket_t cs) {
    const int to = kHttpHeadTimeoutMs;
    if (!set_io_timeout(cs, to)) return;

    std::string head_raw, leftover;
    if (!recv_until_chunked(cs, "\r\n\r\n", kMaxHead, to, &head_raw, &leftover)) {
        bad_request(cs, to);              // 截图空行 / 半截头 / 超限头：一律 400
        return;
    }
    HttpHead hh;
    if (!parse_head(head_raw, &hh)) {
        bad_request(cs, to);
        return;
    }

    if (core.auth_required) {
        const std::string want = "Basic " + b64_encode(core.auth_user + ":" + core.auth_pass);
        if (header_value(hh.lines, "proxy-authorization") != want) {
            const std::string body = "proxy auth required";
            const std::string resp =
                "HTTP/1.1 407 Proxy Authentication Required\r\n"
                "Proxy-Authenticate: Basic realm=\"svpn\"\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            http_simple(cs, resp.c_str(), to);
            core.st.errors++;
            return;
        }
    }

    const std::string method_upper = upper_ascii(hh.method);

    // ---- CONNECT 隧道
    if (method_upper == "CONNECT") {
        std::string host;
        int port = 0;
        const std::size_t colon = hh.target.rfind(':');
        if (colon == std::string::npos) {
            // Python 的 rpartition 会给 host="" port=443，等价于必然 502；这里直接 400
            bad_request(cs, to);
            return;
        }
        if (!split_host_port(hh.target, 443, &host, &port) || host.empty()) {
            bad_request(cs, to);          // CONNECT host:abc / host:99999
            return;
        }
        core.st.conns++;
        core.st.hit(host);
        std::string err;
        socket_t remote = route_dial(core, host, port, &err);
        if (remote == kBadSock) {
            core.st.errors++;
            log_write("WARN", fmt("http connect fail %s:%d %s", host.c_str(), port, err.c_str()));
            bad_gateway(cs, to);
            return;
        }
        if (!http_simple(cs, "HTTP/1.1 200 Connection Established\r\n\r\n", to)) {
            core.st.errors++;
            close_socket(remote);
            return;
        }
        // 头部之后夹带的字节属于隧道内容，先原样灌给上游，一个都不丢
        if (!leftover.empty()) {
            if (!send_all(remote, leftover.data(), leftover.size(), core.timeout_ms)) {
                core.st.errors++;
                close_socket(remote);
                return;
            }
        }
        log_write("DEBUG", fmt("http CONNECT %s:%d -> pipe", host.c_str(), port));
        do_relay(core, cs, remote);
        return;
    }

    // ---- 绝对 URI 正向代理（只支持 http://）
    if (!starts_with(lower(hh.target), "http://")) {
        bad_request(cs, to);
        return;
    }
    const std::string rest = hh.target.substr(7);
    const std::size_t slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    std::string host;
    int port = 0;
    if (!split_host_port(hostport, 80, &host, &port) || host.empty()) {
        bad_request(cs, to);              // http://:80/ 、http://host:abc/
        return;
    }

    core.st.conns++;
    core.st.hit(host);
    std::string err;
    socket_t remote = route_dial(core, host, port, &err);
    if (remote == kBadSock) {
        core.st.errors++;
        log_write("WARN", fmt("http get fail %s:%d %s", host.c_str(), port, err.c_str()));
        bad_gateway(cs, to);
        return;
    }
    SockGuard rg(remote);

    // 改写请求行 + 剥掉代理专用头 + 强制 close（其余头原样转发）
    std::string out = hh.method + " /" + path + " " + hh.version + "\r\n";
    for (const std::string& ln : hh.lines) {
        const std::size_t colon = ln.find(':');
        std::string key = colon == std::string::npos ? std::string() : lower(trim(ln.substr(0, colon)));
        if (key == "proxy-authorization" || key == "proxy-connection" || key == "connection") {
            continue;
        }
        if (ln.empty()) continue;
        out += ln;
        out += "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    if (!send_all(remote, out.data(), out.size(), core.timeout_ms)) {
        core.st.errors++;
        bad_gateway(cs, to);
        return;
    }
    if (!leftover.empty()) {              // POST body 等已被读出的字节，先补发
        if (!send_all(remote, leftover.data(), leftover.size(), core.timeout_ms)) {
            core.st.errors++;
            bad_gateway(cs, to);
            return;
        }
    }
    std::uint64_t u = 0, d = 0;
    (void)relay_stream(cs, remote, kRelayIdleMs, &u, &d);
    core.st.up_bytes += u;
    core.st.down_bytes += d;
}

// ---------------------------------------------------------------- 连接线程
void conn_main(Core& core, socket_t cs, bool http_mode) {
    SockGuard guard(cs);
    try {
        if (http_mode) {
            http_session(core, cs);
        } else {
            socks_session(core, cs);
        }
    } catch (const std::exception& e) {
        core.st.errors++;
        log_write("WARN", fmt("连接处理异常: %s", e.what()));
    } catch (...) {
        core.st.errors++;
        log_write("WARN", "连接处理未知异常");
    }
}

void spawn(Core& core, socket_t cs, bool http_mode) {
    core.add_live(cs);
    core.begin();
    try {
        std::thread th([&core, cs, http_mode] {
            conn_main(core, cs, http_mode);
            core.del_live(cs);     // 先摘除再减计数，保证 shutdown_live 不会碰到已关的 socket
            core.finish();
        });
        th.detach();
    } catch (...) {
        core.del_live(cs);         // finish 未执行，手工补一次
        {
            std::lock_guard<std::mutex> lk(core.mu);
            --core.active;
            if (core.active <= 0) core.cv.notify_all();
        }
        close_socket(cs);
        core.st.errors++;
        log_write("WARN", "无法创建连接线程");
    }
}

// 统一 accept 循环：select(100ms) 负责响应 stop，accept_one 负责真正取连接
void accept_loop(Core& core, socket_t ls, bool http_mode) {
    int fails = 0;
    while (!core.stop.load() && !core.abort.load()) {
        if (!wait_readable(ls, kAcceptTickMs)) continue;
        socket_t cs = accept_one(ls, nullptr);
        if (cs == kBadSock) {
            if (core.stop.load() || core.abort.load()) break;
            if (++fails >= 50) {
                core.st.errors++;
                log_write("WARN", "accept 连续失败，监听循环退出");
                break;
            }
            continue;
        }
        fails = 0;
        spawn(core, cs, http_mode);
    }
    core.abort = true;             // 通知另一入口一起收摊
    core.shutdown_live();
    core.wait_idle(kDrainWaitMs);
}

int serve_one(const Config& cfg, Stats& st, std::atomic<bool>& stop, std::atomic<bool>& abort,
              bool http_mode, socket_t prebound) {
    // ip_map 在每次打开监听时重发布一次：运行期 CDN 选边结果改了也能生效
    publish_ip_map(cfg.ip_map);

    const std::string host = cfg.listen_host.empty() ? std::string("127.0.0.1") : cfg.listen_host;
    const int port = http_mode ? cfg.http_port : cfg.listen_port;

    SockGuard guard(prebound);
    if (prebound == kBadSock) {
        std::string err;
        socket_t ls = listen_on(host, port, &err);
        if (ls == kBadSock) {
            log_write("WARN", fmt("%s 监听 %s:%d 失败: %s",
                                 http_mode ? "HTTP" : "SOCKS5", host.c_str(), port,
                                 err.empty() ? "未知错误" : err.c_str()));
            return 1;
        }
        guard.s = ls;
        const char* tag = http_mode ? "HTTP     listening" : "SOCKS5   listening";
        log_write("INFO", fmt("%s %s:%d", tag, host.c_str(), port));
    }

    Core core(cfg, st, stop, abort);
    accept_loop(core, guard.s, http_mode);
    return 0;
}

}  // namespace

// ================================================================ 服务入口
int serve_socks(const Config& cfg, Stats& st, std::atomic<bool>& stop) {
    std::atomic<bool> abort{false};
    return serve_one(cfg, st, stop, abort, false, kBadSock);
}

int serve_http(const Config& cfg, Stats& st, std::atomic<bool>& stop) {
    if (cfg.http_port <= 0) return 0;          // 0 = 不启动 HTTP 入口
    std::atomic<bool> abort{false};
    return serve_one(cfg, st, stop, abort, true, kBadSock);
}

int serve_all(const Config& cfg, Stats& st, std::atomic<bool>& stop) {
    const std::string host = cfg.listen_host.empty() ? std::string("127.0.0.1") : cfg.listen_host;

    // 先把两个监听都 bind 好，任一失败就整体撤掉，绝不半死不活
    std::string err;
    socket_t socks_ls = listen_on(host, cfg.listen_port, &err);
    if (socks_ls == kBadSock) {
        log_write("WARN", fmt("SOCKS5 监听 %s:%d 失败: %s", host.c_str(), cfg.listen_port,
                             err.empty() ? "未知错误" : err.c_str()));
        return 1;
    }
    SockGuard socks_guard(socks_ls);

    socket_t http_ls = kBadSock;
    SockGuard http_guard;
    if (cfg.http_port > 0) {
        if (cfg.http_port == cfg.listen_port) {
            log_write("WARN", fmt("listen_port 与 http_port 相同: %d", cfg.listen_port));
            return 3;                          // socks_guard 析构时自动关掉已 bind 的监听
        }
        http_ls = listen_on(host, cfg.http_port, &err);
        if (http_ls == kBadSock) {
            log_write("WARN", fmt("HTTP 监听 %s:%d 失败: %s", host.c_str(), cfg.http_port,
                                 err.empty() ? "未知错误" : err.c_str()));
            return 2;
        }
        http_guard.s = http_ls;
    }
    log_write("INFO", fmt("SOCKS5   listening %s:%d", host.c_str(), cfg.listen_port));
    if (http_ls != kBadSock) {
        log_write("INFO", fmt("HTTP     listening %s:%d", host.c_str(), cfg.http_port));
    }

    // 两个入口共享 abort：任一边结束（bind/accept 失败或 stop）另一边立刻收摊
    std::atomic<bool> abort{false};
    std::atomic<int> socks_rc{0};
    const socket_t socks_fd = socks_guard.release();
    const socket_t http_fd = http_guard.release();

    std::thread socks_thread;
    try {
        socks_thread = std::thread([&] { socks_rc = serve_one(cfg, st, stop, abort, false, socks_fd); });
    } catch (...) {
        log_write("WARN", "无法创建 SOCKS 监听线程");
        if (http_fd != kBadSock) close_socket(http_fd);
        return 4;
    }

    if (http_fd != kBadSock) {
        serve_one(cfg, st, stop, abort, true, http_fd);   // HTTP 入口跑在调用线程
    } else {
        while (!stop.load() && !abort.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptTickMs));
        }
    }
    abort = true;
    if (socks_thread.joinable()) socks_thread.join();
    return socks_rc.load();
}

}  // namespace svpn
