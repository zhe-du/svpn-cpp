// A4 proxy 自测：真起监听、真拨号、真中继。
// 覆盖：SOCKS5（无认证/有认证/RFC1929 错误口令/命令不支持）、HTTP CONNECT、
//       绝对 URI 改写、畸形输入 400、407、20 路并发、字节统计、dial_via 上游链。
#include "../src/svpn.hpp"

#ifndef WIN32_WINNT
#define WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace svpn;

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const std::string& name, const std::string& detail = {}) {
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s %s\n", name.c_str(), detail.c_str());
    }
}

void section(const char* title) { std::printf("\n== %s ==\n", title); }

// ---------------------------------------------------------------- 客户端小工具
struct Sock {
    SOCKET s{INVALID_SOCKET};
    Sock() = default;
    Sock(const Sock&) = delete;
    Sock& operator=(const Sock&) = delete;
    Sock(Sock&& o) noexcept : s(o.s) { o.s = INVALID_SOCKET; }
    Sock& operator=(Sock&& o) noexcept {
        if (this != &o) {
            close();
            s = o.s;
            o.s = INVALID_SOCKET;
        }
        return *this;
    }
    ~Sock() { close(); }
    void close() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    bool ok() const { return s != INVALID_SOCKET; }
};

Sock tcp_to(int port, const char* host = "127.0.0.1") {
    Sock c;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &res) != 0) return c;
    c.s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.s != INVALID_SOCKET) {
        if (::connect(c.s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
            c.close();
        } else {
            DWORD tv = 4000;
            setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
            setsockopt(c.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
        }
    }
    freeaddrinfo(res);
    return c;
}

bool send_str(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int rc = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (rc <= 0) return false;
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

std::string recv_n(SOCKET s, std::size_t n) {
    std::string out;
    out.resize(n);
    std::size_t got = 0;
    while (got < n) {
        int rc = ::recv(s, &out[got], static_cast<int>(n - got), 0);
        if (rc <= 0) break;
        got += static_cast<std::size_t>(rc);
    }
    out.resize(got);
    return out;
}

// 读一份响应：数据到齐（间隔 300ms 无新数据）或 EOF 就返回；不会为长连接干等
std::string recv_reply(SOCKET s, std::size_t cap = 8192) {
    std::string out;
    char buf[2048];
    while (out.size() < cap) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) break;
        int rc = ::recv(s, buf, sizeof buf, 0);
        if (rc <= 0) break;
        out.append(buf, static_cast<std::size_t>(rc));
    }
    return out;
}

// 定长字面量发送：避免手写字节数写错
bool send_cstr(SOCKET s, const char* text, int timeout_ms = 5000) {
    const std::size_t n = std::strlen(text);
    return send_all(s, text, n, timeout_ms);
}

// ---------------------------------------------------------------- 夹具服务
// 纯回显
socket_t make_echo(int port) {
    std::string err;
    socket_t ls = listen_on("127.0.0.1", port, &err);
    if (ls == kBadSock) {
        std::printf("[x] echo listen %d fail: %s\n", port, err.c_str());
        std::exit(3);
    }
    std::thread([ls] {
        for (;;) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            std::thread([c] {
                char buf[4096];
                for (;;) {
                    int n = recv_some(c, buf, sizeof buf, 5000);
                    if (n <= 0) break;
                    if (!send_all(c, buf, static_cast<std::size_t>(n), 5000)) break;
                }
                close_socket(c);
            }).detach();
        }
    }).detach();
    return ls;
}

// 假 HTTP 源站：收下请求头 → 记录 → 回固定 200
std::mutex g_origin_mu;
std::string g_origin_req;

socket_t make_origin(int port) {
    std::string err;
    socket_t ls = listen_on("127.0.0.1", port, &err);
    if (ls == kBadSock) {
        std::printf("[x] origin listen %d fail: %s\n", port, err.c_str());
        std::exit(3);
    }
    std::thread([ls] {
        for (;;) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            std::thread([c] {
                std::string req;
                char ch = 0;
                while (req.size() < 8192) {
                    int n = recv_some(c, &ch, 1, 5000);
                    if (n <= 0) break;
                    req.push_back(ch);
                    if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) break;
                }
                {
                    std::lock_guard<std::mutex> lk(g_origin_mu);
                    g_origin_req = req;
                }
                send_cstr(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
                close_socket(c);
            }).detach();
        }
    }).detach();
    return ls;
}

// 假 SOCKS5 上游：验证协商/CONNECT 字节，然后自己连目标并中继
std::atomic<int> g_up_atyp{0};
std::atomic<bool> g_up_auth_fail{false};

socket_t make_socks_upstream(int port, bool require_auth) {
    std::string err;
    socket_t ls = listen_on("127.0.0.1", port, &err);
    if (ls == kBadSock) {
        std::printf("[x] upstream listen %d fail: %s\n", port, err.c_str());
        std::exit(3);
    }
    std::thread([ls, require_auth] {
        for (;;) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            std::thread([c, require_auth] {
                unsigned char b2[2] = {0, 0};
                if (!recv_exact(c, b2, 2, 5000) || b2[0] != 0x05) {
                    close_socket(c);
                    return;
                }
                std::vector<unsigned char> methods(b2[1]);
                if (b2[1] && !recv_exact(c, methods.data(), methods.size(), 5000)) {
                    close_socket(c);
                    return;
                }
                const bool offers_user = std::find(methods.begin(), methods.end(), 0x02) != methods.end();
                if (require_auth) {
                    if (!offers_user) {                       // 客户端没提认证方式：拒绝
                        send_all(c, "\x05\xff", 2, 5000);
                        close_socket(c);
                        return;
                    }
                    send_all(c, "\x05\x02", 2, 5000);
                    unsigned char vb = 0, ul = 0, pl = 0;
                    recv_exact(c, &vb, 1, 5000);
                    recv_exact(c, &ul, 1, 5000);
                    std::string u(ul, '\0');
                    if (ul) recv_exact(c, u.data(), ul, 5000);
                    recv_exact(c, &pl, 1, 5000);
                    std::string p(pl, '\0');
                    if (pl) recv_exact(c, p.data(), pl, 5000);
                    if (u != "u" || p != "p") {
                        g_up_auth_fail = true;
                        send_all(c, "\x01\x01", 2, 5000);
                        close_socket(c);
                        return;
                    }
                    send_all(c, "\x01\x00", 2, 5000);
                } else {
                    send_all(c, "\x05\x00", 2, 5000);
                }
                unsigned char head[4] = {0, 0, 0, 0};
                if (!recv_exact(c, head, 4, 5000)) { close_socket(c); return; }
                g_up_atyp = head[3];
                std::string host;
                if (head[3] == 0x01) {
                    unsigned char ip[4];
                    recv_exact(c, ip, 4, 5000);
                    char t[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, ip, t, sizeof t);
                    host = t;
                } else if (head[3] == 0x04) {
                    unsigned char ip[16];
                    recv_exact(c, ip, 16, 5000);
                    char t[INET6_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET6, ip, t, sizeof t);
                    host = t;
                } else {
                    unsigned char ln = 0;
                    recv_exact(c, &ln, 1, 5000);
                    host.resize(ln);
                    if (ln) recv_exact(c, host.data(), ln, 5000);
                }
                unsigned char pb[2] = {0, 0};
                recv_exact(c, pb, 2, 5000);
                const int port = (static_cast<int>(pb[0]) << 8) | pb[1];
                socket_t remote = tcp_connect(host, port, 3000, nullptr);
                if (remote == kBadSock) {
                    send_all(c, "\x05\x04\x00\x01\x00\x00\x00\x00\x00\x00", 10, 5000);
                    close_socket(c);
                    return;
                }
                send_all(c, "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10, 5000);
                std::uint64_t u = 0, d = 0;
                relay_stream(c, remote, 5000, &u, &d);
                close_socket(remote);
                close_socket(c);
            }).detach();
        }
    }).detach();
    return ls;
}

// 假 HTTP 上游（CONNECT）
std::atomic<bool> g_http_up_ok{false};

socket_t make_http_upstream(int port, bool require_auth) {
    std::string err;
    socket_t ls = listen_on("127.0.0.1", port, &err);
    if (ls == kBadSock) {
        std::printf("[x] http upstream listen %d fail: %s\n", port, err.c_str());
        std::exit(3);
    }
    std::thread([ls, require_auth] {
        for (;;) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            std::thread([c, require_auth] {
                std::string req;
                char ch = 0;
                while (req.size() < 8192) {
                    int n = recv_some(c, &ch, 1, 5000);
                    if (n <= 0) break;
                    req.push_back(ch);
                    if (req.size() >= 4 && req.compare(req.size() - 4, 4, "\r\n\r\n") == 0) break;
                }
                bool good = req.compare(0, 8, "CONNECT ") == 0;
                if (require_auth) {
                    good = good && req.find("Proxy-Authorization: Basic dTpw") != std::string::npos;
                }
                if (!good) {
                    send_cstr(c, "HTTP/1.1 407 Proxy Authentication Required\r\n\r\n");
                    close_socket(c);
                    return;
                }
                g_http_up_ok = true;
                // 目标从请求行里取：CONNECT host:port HTTP/1.1
                const std::size_t sp = req.find(' ');
                const std::size_t sp2 = req.find(' ', sp + 1);
                const std::string target = req.substr(sp + 1, sp2 - sp - 1);
                const std::size_t colon = target.rfind(':');
                const std::string host = target.substr(0, colon);
                const int port = std::atoi(target.substr(colon + 1).c_str());
                socket_t remote = tcp_connect(host, port, 3000, nullptr);
                if (remote == kBadSock) {
                    send_cstr(c, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
                    close_socket(c);
                    return;
                }
                send_cstr(c, "HTTP/1.1 200 Connection Established\r\n\r\n");
                std::uint64_t u = 0, d = 0;
                relay_stream(c, remote, 5000, &u, &d);
                close_socket(remote);
                close_socket(c);
            }).detach();
        }
    }).detach();
    return ls;
}

// ---------------------------------------------------------------- SOCKS5 客户端
// 返回握手是否走到「隧道建立」
bool socks_open(SOCKET s, const std::string& host, int port, bool with_auth,
                const char* user = "u", const char* pass = "p", unsigned char* rep = nullptr,
                unsigned char* method = nullptr) {
    if (with_auth) {
        if (!send_str(s, std::string("\x05\x02\x00\x02", 4))) return false;
    } else {
        if (!send_str(s, std::string("\x05\x01\x00", 3))) return false;
    }
    std::string m = recv_n(s, 2);
    if (m.size() != 2) return false;
    if (method) *method = static_cast<unsigned char>(m[1]);
    if (with_auth) {
        if (static_cast<unsigned char>(m[1]) != 0x02) return false;
        std::string sub;
        sub.push_back(0x01);
        sub.push_back(static_cast<char>(std::strlen(user)));
        sub += user;
        sub.push_back(static_cast<char>(std::strlen(pass)));
        sub += pass;
        if (!send_str(s, sub)) return false;
        std::string ar = recv_n(s, 2);
        if (ar.size() != 2 || ar[1] != 0x00) return false;
    } else if (static_cast<unsigned char>(m[1]) != 0x00) {
        return false;
    }
    std::string req;
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);
    // 只测 v4 / 域名两种写法
    unsigned char ip[4];
    if (inet_pton(AF_INET, host.c_str(), ip) == 1) {
        req.push_back(0x01);
        req.append(reinterpret_cast<char*>(ip), 4);
    } else {
        req.push_back(0x03);
        req.push_back(static_cast<char>(host.size()));
        req += host;
    }
    req.push_back(static_cast<char>((port >> 8) & 0xff));
    req.push_back(static_cast<char>(port & 0xff));
    if (!send_str(s, req)) return false;
    std::string r = recv_n(s, 10);
    if (r.size() != 10) return false;
    if (rep) *rep = static_cast<unsigned char>(r[1]);
    return r[0] == 0x05 && r[1] == 0x00;
}

}  // namespace

int main() {
    console_utf8();
    if (!net_init()) {
        std::printf("[x] net_init 失败\n");
        return 1;
    }

    constexpr int kEchoPort = 17990;
    constexpr int kOriginPort = 17991;
    constexpr int kSocksPort = 17901;
    constexpr int kHttpPort = 17902;
    constexpr int kAuthSocksPort = 17903;
    constexpr int kAuthHttpPort = 17904;
    constexpr int kUpSocksPort = 17905;
    constexpr int kUpSocksAuthPort = 17906;
    constexpr int kUpHttpPort = 17907;

    make_echo(kEchoPort);
    make_origin(kOriginPort);

    // ---- 服务 1：无本地鉴权，上游未配置（NODE_HOST）→ 全部直连
    Config cfg = default_config();
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = kSocksPort;
    cfg.http_port = kHttpPort;
    cfg.connect_timeout = 3;
    cfg.auth = AuthCfg{};
    // 故意用一个解析不出来的域名：只有 ip_map 注入生效才可能连上
    cfg.ip_map["svpn-test.invalid"] = "127.0.0.1";

    // ---- 服务 2：本地鉴权 u/p
    Config acfg = default_config();
    acfg.listen_host = "127.0.0.1";
    acfg.listen_port = kAuthSocksPort;
    acfg.http_port = kAuthHttpPort;
    acfg.connect_timeout = 3;
    acfg.auth = AuthCfg{"u", "p"};
    acfg.ip_map["svpn-test.invalid"] = "127.0.0.1";   // 两个服务发布同一份，避免互相覆盖

    Stats st, ast;
    std::atomic<bool> stop{false};

    std::thread srv1([&] { serve_all(cfg, st, stop); });
    std::thread srv2([&] { serve_all(acfg, ast, stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));   // 等 bind 就绪

    // ---------------------------------------------------------------- SOCKS5 无认证
    section("SOCKS5 无认证");
    {
        Sock c = tcp_to(kSocksPort);
        check(c.ok(), "连上 SOCKS5 入口");
        unsigned char rep = 0xff;
        bool up = socks_open(c.s, "127.0.0.1", kEchoPort, false, "u", "p", &rep);
        check(up, "握手 + CONNECT 127.0.0.1 成功", fmt("rep=0x%02x", rep));
        check(rep == 0x00, "REP=0x00 成功", fmt("rep=0x%02x", rep));
        send_str(c.s, "hello");
        std::string back = recv_n(c.s, 5);
        check(back == "hello", "隧道内回显 hello", "got=" + back);
    }
    {
        // 域名走 ip_map（svpn-test.invalid → 127.0.0.1），DNS 必然解不出
        Sock c = tcp_to(kSocksPort);
        unsigned char rep = 0xff;
        bool up = socks_open(c.s, "svpn-test.invalid", kEchoPort, false, "u", "p", &rep);
        check(up, "CONNECT 解析不出的域名（仅靠 ip_map 直连 IP）", fmt("rep=0x%02x", rep));
        send_str(c.s, "abc");
        check(recv_n(c.s, 3) == "abc", "ip_map 路径回显 abc");
    }
    {
        // 命令不支持（BIND=0x02）→ 07
        Sock c = tcp_to(kSocksPort);
        send_str(c.s, std::string("\x05\x01\x00", 3));
        recv_n(c.s, 2);
        send_str(c.s, std::string("\x05\x02\x00\x01\x7f\x00\x00\x01\x00\x50", 10));
        std::string r = recv_n(c.s, 10);
        check(r.size() == 10 && static_cast<unsigned char>(r[1]) == 0x07,
              "非 CONNECT 命令回 REP=0x07",
              r.size() == 10 ? fmt("rep=0x%02x", static_cast<unsigned char>(r[1])) : "无应答");
    }
    {
        // 连不通的目标 → REP 非 0
        Sock c = tcp_to(kSocksPort);
        unsigned char rep = 0;
        bool up = socks_open(c.s, "127.0.0.1", 17999, false, "u", "p", &rep);
        check(!up && rep != 0x00, "目标不可达时 REP 非 0", fmt("rep=0x%02x", rep));
    }

    // ---------------------------------------------------------------- SOCKS5 有认证
    section("SOCKS5 本地认证");
    {
        Sock c = tcp_to(kAuthSocksPort);
        unsigned char method = 0, rep = 0;
        bool up = socks_open(c.s, "127.0.0.1", kEchoPort, true, "u", "p", &rep, &method);
        check(method == 0x02, "协商选中 0x02（用户名/密码）", fmt("method=0x%02x", method));
        check(up, "正确口令 通过并建隧道");
        send_str(c.s, "auth-ok");
        check(recv_n(c.s, 7) == "auth-ok", "带认证的隧道回显");
    }
    {
        Sock c = tcp_to(kAuthSocksPort);
        send_str(c.s, std::string("\x05\x02\x00\x02", 4));
        std::string m = recv_n(c.s, 2);
        check(m.size() == 2 && static_cast<unsigned char>(m[1]) == 0x02, "服务端选 0x02");
        send_str(c.s, std::string("\x01\x01u\x01x", 5));      // 口令错
        std::string r = recv_n(c.s, 2);
        check(r.size() == 2 && static_cast<unsigned char>(r[1]) == 0x01, "错误口令回 01 01",
              r.size() == 2 ? fmt("status=0x%02x", static_cast<unsigned char>(r[1])) : "无应答");
        // 口令失败后连接应被关闭
        std::string more = recv_reply(c.s, 64);
        check(more.empty(), "认证失败后服务端关连接");
    }
    {
        // 只提供 0x00 而不提供 0x02 → 05 ff
        Sock c = tcp_to(kAuthSocksPort);
        send_str(c.s, std::string("\x05\x01\x00", 3));
        std::string r = recv_n(c.s, 2);
        check(r.size() == 2 && static_cast<unsigned char>(r[1]) == 0xff,
              "需要认证时仅提供 0x00 回 05 ff",
              r.size() == 2 ? fmt("m=0x%02x", static_cast<unsigned char>(r[1])) : "无应答");
    }

    // ---------------------------------------------------------------- HTTP 代理
    section("HTTP CONNECT");
    {
        Sock c = tcp_to(kHttpPort);
        check(c.ok(), "连上 HTTP 入口");
        send_str(c.s, "CONNECT 127.0.0.1:17990 HTTP/1.1\r\nHost: 127.0.0.1:17990\r\n\r\n");
        std::string head = recv_reply(c.s, 200);   // 只拿到 200 头（1024 上限内）
        check(head.find("200 Connection Established") != std::string::npos,
              "CONNECT 回 200", "got=" + head.substr(0, 40));
        send_str(c.s, "tunnel");
        check(recv_n(c.s, 6) == "tunnel", "CONNECT 隧道内收发");
    }
    {
        Sock c = tcp_to(kHttpPort);
        send_str(c.s, "CONNECT 127.0.0.1:abc HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "CONNECT 端口非数字 → 400",
              "got=" + r.substr(0, 40));
    }
    {
        Sock c = tcp_to(kHttpPort);
        send_str(c.s, "CONNECT 127.0.0.1:99999 HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "CONNECT 端口越界 → 400");
    }
    {
        Sock c = tcp_to(kHttpPort);
        send_str(c.s, "GET http://:80/ HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "空主机 http://:80/ → 400");
    }
    {
        Sock c = tcp_to(kHttpPort);
        send_str(c.s, "GARBAGE\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "无请求行字段 → 400");
    }
    {
        Sock c = tcp_to(kHttpPort);
        send_str(c.s, "\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "空行请求 → 400");
    }
    {
        // 头部超过 16KB 上限：不许吃光内存，直接 400
        Sock c = tcp_to(kHttpPort);
        std::string huge = "GET http://127.0.0.1:17991/ HTTP/1.1\r\n";
        while (huge.size() < 20000) huge += "X-Pad: 012345678901234567890123456789\r\n";
        send_str(c.s, huge);
        std::string r = recv_reply(c.s, 200);
        check(r.find("400 Bad Request") != std::string::npos, "头部超 16KB → 400");
    }
    section("HTTP 绝对 URI 正向代理");
    {
        {
            std::lock_guard<std::mutex> lk(g_origin_mu);
            g_origin_req.clear();
        }
        Sock c = tcp_to(kHttpPort);
        send_str(c.s,
                 "GET http://127.0.0.1:17991/abc?x=1 HTTP/1.1\r\n"
                 "Host: 127.0.0.1:17991\r\n"
                 "Proxy-Connection: keep-alive\r\n"
                 "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n"
                 "User-Agent: svpn-test\r\n\r\n");
        std::string resp = recv_reply(c.s, 4096);
        check(resp.find("200 OK") != std::string::npos, "响应原样回传（200 OK）",
              "got=" + resp.substr(0, 40));
        std::string seen;
        {
            std::lock_guard<std::mutex> lk(g_origin_mu);
            seen = g_origin_req;
        }
        check(starts_with(seen, "GET /abc?x=1"), "请求行改写为相对路径", "got=" + seen.substr(0, 30));
        check(seen.find("Proxy-Connection") == std::string::npos, "剥掉 Proxy-Connection");
        check(seen.find("Proxy-Authorization") == std::string::npos, "剥掉 Proxy-Authorization");
        check(seen.find("Connection: close") != std::string::npos, "补上 Connection: close");
        check(seen.find("User-Agent: svpn-test") != std::string::npos, "其余头原样转发");
    }

    // ---------------------------------------------------------------- 407
    section("HTTP 本地鉴权 407");
    {
        Sock c = tcp_to(kAuthHttpPort);
        send_str(c.s, "CONNECT 127.0.0.1:17990 HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string r = recv_reply(c.s, 400);
        check(r.find("407 Proxy Authentication Required") != std::string::npos, "无凭据 → 407",
              "got=" + r.substr(0, 40));
        check(r.find("Proxy-Authenticate: Basic realm=\"svpn\"") != std::string::npos,
              "407 带 Proxy-Authenticate");
    }
    {
        Sock c = tcp_to(kAuthHttpPort);
        send_str(c.s,
                 "CONNECT 127.0.0.1:17990 HTTP/1.1\r\nHost: x\r\n"
                 "Proxy-Authorization: Basic dTpw\r\n\r\n");
        std::string r = recv_reply(c.s, 200);
        check(r.find("200 Connection Established") != std::string::npos, "正确凭据 → 200",
              "got=" + r.substr(0, 40));
        send_str(c.s, "pw-ok");
        check(recv_n(c.s, 5) == "pw-ok", "带凭据的隧道可用");
    }
    {
        Sock c = tcp_to(kAuthHttpPort);
        send_str(c.s,
                 "CONNECT 127.0.0.1:17990 HTTP/1.1\r\nHost: x\r\n"
                 "Proxy-Authorization: Basic eDp4\r\n\r\n");
        std::string r = recv_reply(c.s, 400);
        check(r.find("407") != std::string::npos, "错误凭据 → 407");
    }

    // ---------------------------------------------------------------- 前缀相同的日志不会干扰断言
    // ---------------------------------------------------------------- 20 路并发
    section("20 路并发 SOCKS5");
    {
        constexpr int kThreads = 20;
        std::atomic<int> ok{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < kThreads; ++i) {
            ts.emplace_back([&ok, i] {
                Sock c = tcp_to(kSocksPort);
                if (!c.ok()) return;
                if (!socks_open(c.s, "127.0.0.1", kEchoPort, false)) return;
                const std::string msg = fmt("payload-%02d", i);
                if (!send_str(c.s, msg)) return;
                if (recv_n(c.s, msg.size()) != msg) return;
                ok++;
            });
        }
        for (auto& t : ts) t.join();
        check(ok.load() == kThreads, "20/20 全部成功", fmt("ok=%d", ok.load()));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ---------------------------------------------------------------- 统计
    section("统计计数");
    std::printf("  conns=%llu errors=%llu up=%llu down=%llu via_direct=%llu via_upstream=%llu\n",
                static_cast<unsigned long long>(st.conns.load()),
                static_cast<unsigned long long>(st.errors.load()),
                static_cast<unsigned long long>(st.up_bytes.load()),
                static_cast<unsigned long long>(st.down_bytes.load()),
                static_cast<unsigned long long>(st.via_direct.load()),
                static_cast<unsigned long long>(st.via_upstream.load()));
    check(st.conns.load() >= 20, "conns >= 20",
          fmt("conns=%llu", static_cast<unsigned long long>(st.conns.load())));
    check(st.down_bytes.load() > 0, "down_bytes > 0",
          fmt("down=%llu", static_cast<unsigned long long>(st.down_bytes.load())));
    check(st.up_bytes.load() > 0, "up_bytes > 0",
          fmt("up=%llu", static_cast<unsigned long long>(st.up_bytes.load())));
    check(st.via_direct.load() >= 20, "via_direct >= 20（上游未配置）",
          fmt("via_direct=%llu", static_cast<unsigned long long>(st.via_direct.load())));

    // ---------------------------------------------------------------- dial_via
    section("dial_via 上游链");
    make_socks_upstream(kUpSocksPort, false);
    make_socks_upstream(kUpSocksAuthPort, true);
    make_http_upstream(kUpHttpPort, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        Node up;
        up.kind = "socks5";
        up.host = "127.0.0.1";
        up.port = kUpSocksPort;
        std::string err;
        socket_t s = dial_via(up, "127.0.0.1", kEchoPort, 3000, &err);
        check(s != kBadSock, "socks5 上游 CONNECT IPv4 成功", err);
        check(g_up_atyp.load() == 0x01, "IPv4 用 ATYP=0x01",
              fmt("atyp=0x%02x", g_up_atyp.load()));
        if (s != kBadSock) {
            send_all(s, "via-ip", 6, 3000);
            char buf[8] = {0};
            bool got = recv_exact(s, buf, 6, 3000);
            check(got && std::string(buf, 6) == "via-ip", "上游隧道回显");
            close_socket(s);
        }
    }
    {
        Node up;
        up.kind = "socks5";
        up.host = "127.0.0.1";
        up.port = kUpSocksPort;
        std::string err;
        socket_t s = dial_via(up, "localhost", kEchoPort, 3000, &err);
        check(s != kBadSock, "socks5 上游 CONNECT 域名成功（远端 DNS）", err);
        check(g_up_atyp.load() == 0x03, "域名用 ATYP=0x03",
              fmt("atyp=0x%02x", g_up_atyp.load()));
        if (s != kBadSock) close_socket(s);
    }
    {
        Node up;
        up.kind = "socks5";
        up.host = "127.0.0.1";
        up.port = kUpSocksPort;
        std::string err;
        socket_t s = dial_via(up, "::1", kEchoPort, 2000, &err);
        if (s != kBadSock) close_socket(s);
        check(g_up_atyp.load() == 0x04, "IPv6 用 ATYP=0x04（不按域名发）",
              fmt("atyp=0x%02x", g_up_atyp.load()));
    }
    {
        Node up;
        up.kind = "socks5";
        up.host = "127.0.0.1";
        up.port = kUpSocksAuthPort;
        up.user = "u";
        up.pass = "p";
        std::string err;
        g_up_auth_fail = false;
        socket_t s = dial_via(up, "127.0.0.1", kEchoPort, 3000, &err);
        check(s != kBadSock && !g_up_auth_fail, "socks5 上游 RFC1929 认证通过", err);
        if (s != kBadSock) {
            send_all(s, "via-auth", 8, 3000);
            char buf[8] = {0};
            check(recv_exact(s, buf, 8, 3000) && std::string(buf, 8) == "via-auth",
                  "带认证的上游隧道回显");
            close_socket(s);
        }
    }
    {
        Node up;
        up.kind = "socks5";
        up.host = "127.0.0.1";
        up.port = kUpSocksAuthPort;
        up.user = "u";
        up.pass = "wrong";
        std::string err;
        socket_t s = dial_via(up, "127.0.0.1", kEchoPort, 3000, &err);
        check(s == kBadSock && err.find("认证失败") != std::string::npos,
              "错误口令被拒且报错可读", err);
        if (s != kBadSock) close_socket(s);
    }
    {
        Node up;
        up.kind = "http";
        up.host = "127.0.0.1";
        up.port = kUpHttpPort;
        up.user = "u";
        up.pass = "p";
        std::string err;
        socket_t s = dial_via(up, "127.0.0.1", kEchoPort, 3000, &err);
        check(s != kBadSock && g_http_up_ok.load(), "http 上游 CONNECT + Basic 认证成功", err);
        if (s != kBadSock) {
            send_all(s, "via-http", 8, 3000);
            char buf[8] = {0};
            check(recv_exact(s, buf, 8, 3000) && std::string(buf, 8) == "via-http",
                  "http 上游隧道回显");
            close_socket(s);
        }
    }
    {
        Node up;
        up.kind = "socks5";
        up.host = "NODE_HOST";       // 占位符 = 没配节点
        up.port = 1080;
        std::string err;
        socket_t s = dial_via(up, "127.0.0.1", kEchoPort, 3000, &err);
        check(s != kBadSock, "占位符上游自动回落 direct", err);
        if (s != kBadSock) {
            send_all(s, "fallback", 8, 3000);
            char buf[8] = {0};
            check(recv_exact(s, buf, 8, 3000) && std::string(buf, 8) == "fallback",
                  "回落直连后隧道可用");
            close_socket(s);
        }
    }

    // ---------------------------------------------------------------- 优雅退出
    section("退出");
    // 留一条空闲隧道不关：验证 stop 时会 shutdown 活动连接，不会把进程拖住
    Sock idle = tcp_to(kSocksPort);
    bool idle_open = idle.ok() && socks_open(idle.s, "127.0.0.1", kEchoPort, false);
    check(idle_open, "退出前先建立一条空闲隧道");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const std::int64_t t0 = now_ms();
    stop = true;
    srv1.join();
    srv2.join();
    const std::int64_t cost = now_ms() - t0;
    check(cost < 8000, "stop 后 8s 内两个服务全部收尾（含空闲隧道）",
          fmt("cost=%lldms", static_cast<long long>(cost)));
    std::printf("  stop 收尾耗时 %lldms\n", static_cast<long long>(cost));

    net_shutdown();
    std::printf("\n结果: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
