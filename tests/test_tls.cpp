// tests/test_tls.cpp —— A3 自测（临时 main，不参与 svpn.exe 构建）
//
// 因为 A2 的 src/net.cpp 还没落地，这里自带一份最小的 net 层桩（tcp_connect_ip /
// send_all / recv_some / close_socket / set_io_timeout / resolve_system），
// 只用 Winsock，语义与 svpn.hpp 契约一致。
//
// 编译：
//   g++ -std=c++20 -O2 -Wall -Wextra tests/test_tls.cpp src/tls.cpp src/util.cpp src/json.cpp
//       -o build/test_tls.exe -lws2_32 -lcrypt32 -lsecur32
#include "../src/svpn.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ================================================================ net 层桩
namespace svpn {

socket_t tcp_connect_ip(const std::string& ip, int port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string ps = std::to_string(port);
    if (getaddrinfo(ip.c_str(), ps.c_str(), &hints, &res) != 0 || res == nullptr) return kBadSock;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return kBadSock;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return kBadSock;
        }
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int sel = select(0, nullptr, &wf, &ef, &tv);
        if (sel <= 0 || FD_ISSET(s, &ef)) {
            closesocket(s);
            return kBadSock;
        }
    }
    u_long blk = 0;
    ioctlsocket(s, FIONBIO, &blk);
    return static_cast<socket_t>(s);
}

socket_t tcp_connect(const std::string& host, int port, int timeout_ms, std::string* err) {
    auto ips = resolve_system(host, timeout_ms);
    if (ips.empty()) {
        if (err) *err = "解析失败";
        return kBadSock;
    }
    return tcp_connect_ip(ips[0], port, timeout_ms);
}

std::vector<std::string> resolve_system(const std::string& host, int /*timeout_ms*/) {
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || res == nullptr) return out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[64] = {0};
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (InetNtopA(AF_INET, &sa->sin_addr, buf, sizeof(buf)) != nullptr) {
            std::string ip(buf);
            if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        }
    }
    freeaddrinfo(res);
    return out;
}

bool set_io_timeout(socket_t s, int timeout_ms) {
    if (s == kBadSock) return false;
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    return true;
}

void close_socket(socket_t s) {
    if (s == kBadSock) return;
    closesocket(static_cast<SOCKET>(s));
}

bool send_all(socket_t s, const void* data, std::size_t n, int timeout_ms) {
    if (s == kBadSock) return false;
    const char* p = static_cast<const char*>(data);
    std::size_t done = 0;
    while (done < n) {
        int chunk = static_cast<int>(std::min<std::size_t>(n - done, 65536));
        int r = ::send(static_cast<SOCKET>(s), p + done, chunk, 0);
        if (r <= 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                fd_set wf;
                FD_ZERO(&wf);
                FD_SET(static_cast<SOCKET>(s), &wf);
                timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
                if (select(0, nullptr, &wf, nullptr, &tv) <= 0) return false;
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(r);
    }
    return true;
}

int recv_some(socket_t s, void* buf, std::size_t cap, int timeout_ms) {
    if (s == kBadSock || cap == 0) return -1;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(static_cast<SOCKET>(s), &rf);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int sel = select(0, &rf, nullptr, nullptr, &tv);
    if (sel <= 0) return -1;
    int r = ::recv(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(cap), 0);
    if (r == 0) return 0;
    if (r < 0) return -1;
    return r;
}

}  // namespace svpn

// ================================================================ 小工具
namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

std::vector<std::string> resolve(const std::string& host) {
    return svpn::resolve_system(host, 5000);
}

// 用本地监听端口接一枚真实 SChannel ClientHello
std::vector<unsigned char> capture_client_hello(const std::string& sni, std::size_t* out_len) {
    *out_len = 0;
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(ls);
        return {};
    }
    int alen = sizeof(addr);
    getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen);
    ::listen(ls, 1);

    SOCKET cli = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (::connect(cli, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(ls);
        closesocket(cli);
        return {};
    }

    std::thread th([cli, sni]() {
        std::string err;
        svpn::TlsConn c = svpn::tls_connect(static_cast<svpn::socket_t>(cli), sni, 2500, &err);
        c.close();
    });

    SOCKET srv = ::accept(ls, nullptr, nullptr);
    std::vector<unsigned char> got;
    if (srv != INVALID_SOCKET) {
        svpn::set_io_timeout(static_cast<svpn::socket_t>(srv), 3000);
        unsigned char tmp[4096];
        while (got.size() < 5 || got.size() < 5u + ((got[3] << 8) | got[4])) {
            int r = ::recv(srv, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (r <= 0) break;
            got.insert(got.end(), tmp, tmp + r);
            if (got.size() > 65536) break;
        }
        *out_len = got.size();
        closesocket(srv);
    }
    closesocket(ls);
    th.join();
    return got;
}

std::string hex_head(const std::vector<unsigned char>& v, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < std::min(n, v.size()); ++i) s += svpn::fmt("%02x", v[i]);
    return s;
}

}  // namespace

// ================================================================ main
int main() {
    WSADATA wd{};
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        std::printf("WSAStartup 失败\n");
        return 2;
    }
    std::printf("=== A3 tls 自测 ===\n");

    // ---------------------------------------------------------- 1. 解析器：真实 ClientHello
    std::printf("\n[1] parse_client_hello_sni：抓一段真实 SChannel ClientHello\n");
    std::size_t hello_n = 0;
    auto hello = capture_client_hello("store.steampowered.com", &hello_n);
    std::printf("  抓到 %zu 字节，头部 %s\n", hello_n, hex_head(hello, 16).c_str());
    check(hello_n > 40, "抓到非空 ClientHello");
    if (!hello.empty()) {
        std::string sni;
        bool ok = svpn::parse_client_hello_sni(hello.data(), hello.size(), &sni);
        std::printf("  解析结果 ok=%d sni=\"%s\"\n", static_cast<int>(ok), sni.c_str());
        check(ok, "解析成功");
        check(sni == "store.steampowered.com", "SNI == store.steampowered.com");
    }

    // ---------------------------------------------------------- 2. 解析器：边界与畸形
    std::printf("\n[2] parse_client_hello_sni：边界 / 截断 / 非法长度\n");
    {
        std::string sni;
        check(!svpn::parse_client_hello_sni(nullptr, 0, &sni), "nullptr,0 → false");
        check(!svpn::parse_client_hello_sni(nullptr, 100, &sni), "nullptr,100 → false");
    }
    if (!hello.empty()) {
        std::string sni;
        // 每条完整记录之前的所有前缀都必须 false（不允许越界读半个字段）
        std::size_t reclen = (static_cast<std::size_t>(hello[3]) << 8) | hello[4];
        std::size_t full = std::min(hello.size(), 5 + reclen);
        bool trunc_all_false = true;
        for (std::size_t cut = 0; cut < full; ++cut) {
            sni.clear();
            if (svpn::parse_client_hello_sni(hello.data(), cut, &sni)) trunc_all_false = false;
        }
        check(trunc_all_false, "0..整条记录之前的所有前缀都返回 false（不越界读）");

        // 破坏各字段长度
        auto bad = hello;
        bad[0] = 0x17;   // 不是 handshake
        check(!svpn::parse_client_hello_sni(bad.data(), bad.size(), &sni), "记录类型 0x17 → false");
        bad = hello;
        bad[5] = 0x02;   // 不是 client_hello
        check(!svpn::parse_client_hello_sni(bad.data(), bad.size(), &sni), "handshake type 0x02 → false");
        bad = hello;
        bad[3] = 0xFF;
        bad[4] = 0xFF;   // record 长度远大于实际
        check(!svpn::parse_client_hello_sni(bad.data(), bad.size(), &sni), "record 长度超长 → false");
        bad = hello;
        bad[1] = 0x02;   // 不是 0x03
        check(!svpn::parse_client_hello_sni(bad.data(), bad.size(), &sni), "record 版本非 0x03 → false");
    }

    // ---------------------------------------------------------- 3. 解析器：随机模糊
    std::printf("\n[3] parse_client_hello_sni：200 段随机字节（固定种子）\n");
    {
        std::mt19937 rng(20261024u);
        std::string sni;
        int parsed = 0;
        for (int i = 0; i < 200; ++i) {
            std::size_t len = rng() % 600;
            std::vector<unsigned char> buf(len);
            for (auto& b : buf) b = static_cast<unsigned char>(rng() & 0xFF);
            // 一半样本强制打上合法的 TLS 记录头，逼解析器往深处走
            if (i % 2 == 0 && len >= 6) {
                buf[0] = 0x16;
                buf[1] = 0x03;
                buf[2] = 0x01;
                buf[3] = static_cast<unsigned char>((len - 5) >> 8);
                buf[4] = static_cast<unsigned char>((len - 5) & 0xFF);
                buf[5] = 0x01;
            }
            sni.clear();
            if (svpn::parse_client_hello_sni(buf.data(), buf.size(), &sni)) ++parsed;
        }
        check(true, "200 段随机字节未崩溃");
        std::printf("  其中被判定为合法 SNI 的：%d 条\n", parsed);
    }

    // ---------------------------------------------------------- 4. 实网握手
    // 先按要求打 store；本机到 Steam 边缘常被丢包，所以再准备两个备选目标，
    // 只要有一个能握上手，就说明 SChannel 链路本身是对的。
    struct Target { const char* host; };
    const Target targets[] = {{"store.steampowered.com"}, {"api.steampowered.com"}, {"www.baidu.com"}};

    std::printf("\n[4] tls_probe 实网握手\n");
    std::string good_ip, good_sni;
    for (const auto& t : targets) {
        auto ips = resolve(t.host);
        std::printf("  %s → %zu 个 IP", t.host, ips.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(ips.size(), 3); ++i)
            std::printf(" %s", ips[i].c_str());
        std::printf("\n");
        for (const auto& ip : ips) {
            svpn::TlsProbe p = svpn::tls_probe(ip, t.host, 8000);
            std::printf("    probe %s ok=%d protocol=%s cert_cn=%s ms=%.0f err=%s\n", ip.c_str(),
                        static_cast<int>(p.ok), p.protocol.c_str(), p.cert_cn.c_str(), p.ms,
                        p.err.c_str());
            if (p.ok) {
                check(true, std::string("握手成功: ") + t.host + " @ " + ip);
                check(!p.protocol.empty(), "protocol 非空");
                check(!p.cert_cn.empty(), "cert_cn 非空");
                check(p.cert_cn.find("steampowered") != std::string::npos ||
                          p.cert_cn.find("baidu") != std::string::npos ||
                          p.cert_cn.find(".") != std::string::npos,
                      "cert_cn 是域名形态");
                if (good_ip.empty()) {
                    good_ip = ip;
                    good_sni = t.host;
                }
                break;
            }
        }
        if (!good_ip.empty()) break;
    }
    check(!good_ip.empty(), "至少一个实网目标握手成功");

    // ---------------------------------------------------------- 5. 证书不匹配
    std::printf("\n[5] tls_probe：A 的 IP + B 的 SNI（应失败）\n");
    if (!good_ip.empty()) {
        svpn::TlsProbe p = svpn::tls_probe(good_ip, "example.com", 8000);
        std::printf("  ip=%s sni=example.com ok=%d cert_cn=%s protocol=%s err=%s\n", good_ip.c_str(),
                    static_cast<int>(p.ok), p.cert_cn.c_str(), p.protocol.c_str(), p.err.c_str());
        check(!p.ok, "ok == false");
        check(p.err.find("主机名") != std::string::npos || p.err.find("证书") != std::string::npos,
              "err 指明证书问题");

        // 同 IP 换成自己的 SNI 应当又能通，说明失败来自证书而非网络
        svpn::TlsProbe q = svpn::tls_probe(good_ip, good_sni, 8000);
        std::printf("  对照：ip=%s sni=%s ok=%d cert_cn=%s\n", good_ip.c_str(), good_sni.c_str(),
                    static_cast<int>(q.ok), q.cert_cn.c_str());
        check(q.ok, "同 IP 用正确 SNI 仍可握手（失败确系证书）");
    } else {
        std::printf("  （跳过：无可用 IP）\n");
    }

    // ---------------------------------------------------------- 6. 连不通的 IP
    std::printf("\n[6] tls_probe：不可达 IP\n");
    {
        svpn::TlsProbe p = svpn::tls_probe("127.0.0.1", "store.steampowered.com", 1500);
        std::printf("  ok=%d ms=%.0f err=%s\n", static_cast<int>(p.ok), p.ms, p.err.c_str());
        check(!p.ok, "ok == false");
        check(!p.err.empty(), "err 非空");
    }

    // ---------------------------------------------------------- 7. 加密通道收发
    std::printf("\n[7] TlsConn send/recv：HTTP HEAD 实网往返\n");
    if (!good_ip.empty()) {
        svpn::socket_t s = svpn::tcp_connect_ip(good_ip, 443, 8000);
        check(s != svpn::kBadSock, "TCP 连接建立");
        if (s != svpn::kBadSock) {
            std::string err;
            svpn::TlsConn c = svpn::tls_connect(s, good_sni, 8000, &err);
            std::printf("  握手 ok=%d err=%s\n", static_cast<int>(c.ok), err.c_str());
            check(c.ok, "握手成功");
            if (c.ok) {
                std::string req = "HEAD / HTTP/1.1\r\nHost: " + good_sni +
                                  "\r\nUser-Agent: svpn-test\r\nAccept: */*\r\n"
                                  "Connection: close\r\n\r\n";
                int sent = c.send(req.data(), req.size());
                std::printf("  发送 %d 字节\n", sent);
                check(sent == static_cast<int>(req.size()), "send 返回完整长度");

                std::string resp;
                char buf[4096];
                for (int i = 0; i < 8; ++i) {
                    int r = c.recv(buf, sizeof(buf));
                    if (r <= 0) break;
                    resp.append(buf, static_cast<std::size_t>(r));
                    if (resp.find("\r\n\r\n") != std::string::npos) break;
                }
                std::string first = resp.substr(0, resp.find("\r\n"));
                std::printf("  收到 %zu 字节，状态行: %s\n", resp.size(), first.c_str());
                check(resp.rfind("HTTP/1.", 0) == 0, "收到 HTTP 响应");
                // 幂等：连按两次 close 不许崩
                c.close();
                c.close();
                check(true, "close() 连调两次不崩");
            } else {
                svpn::close_socket(s);
            }
        }
    }

    // ---------------------------------------------------------- 8. 对端 close_notify
    std::printf("\n[8] 对端 close_notify → recv 返回 0\n");
    if (!good_ip.empty()) {
        svpn::socket_t s = svpn::tcp_connect_ip(good_ip, 443, 8000);
        if (s != svpn::kBadSock) {
            std::string err;
            svpn::TlsConn c = svpn::tls_connect(s, good_sni, 8000, &err);
            if (c.ok) {
                std::string req = "GET / HTTP/1.1\r\nHost: " + good_sni +
                                  "\r\nConnection: close\r\nUser-Agent: svpn-test\r\n\r\n";
                c.send(req.data(), req.size());
                int last = 0;
                long total = 0;
                for (int i = 0; i < 4000; ++i) {
                    char buf[16384];
                    last = c.recv(buf, sizeof(buf));
                    if (last <= 0) break;
                    total += last;
                }
                std::printf("  共收 %ld 字节，最后一次 recv=%d\n", total, last);
                check(last == 0, "读到 EOF 时 recv 返回 0（不是 -1）");
                c.close();
            } else {
                svpn::close_socket(s);
            }
        }
    }

    std::printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    WSACleanup();
    return g_fail == 0 ? 0 : 1;
}
