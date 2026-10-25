// tests/test_sni.cpp —— A7（sni.cpp）自测。
//
// 覆盖：
//   1. 白名单命中：ClientHello（分三片发送，模拟 TCP 拆包）原样到达上游，逐字节比对；
//      上游回包原样回到客户端。
//   2. 白名单含 steampowered.com：store.steampowered.com 通过；example.com 被拒（EOF），
//      且被拒时不会向上游拨号。
//   3. 白名单为空（cdn.hosts 为空）=> 全部拒绝。
//   4. 非 TLS 首包 => 立刻拒绝（不傻等 10 秒）。
//   5. 16KB 上限：读满 max_head 仍无 SNI => 拒绝。
//
// 编译（A1/A3 未就绪时用桩，见文件末尾）：
//   g++ -std=c++20 -O2 -Wall -Wextra -I src
//       tests/test_sni.cpp src/sni.cpp src/net.cpp src/util.cpp src/json.cpp
//       -o tests/test_sni.exe -lws2_32
// A1 的 rules.cpp / A3 的 tls.cpp 就绪后加 -DSNI_TEST_REAL_RULES -DSNI_TEST_REAL_TLS，
// 并把对应 .cpp 一起编译，桩自动失效。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winsock2.h>

#include "svpn.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;

}  // namespace

namespace svpn {
// sni.cpp 内部暴露的测试入口（头文件里没有：cfg 没有「目标端口」这一项，
// 而测试必须把上游指到高位端口，不能去抢 443）。
int serve_sni_to(const Config& cfg, Stats& st, std::atomic<bool>& stop, int target_port);
}  // namespace svpn

namespace {

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("[ ok ] %s\n", what);
    } else {
        std::printf("[FAIL] %s\n", what);
        ++g_fail;
    }
}

void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int local_port(svpn::socket_t s) {
    sockaddr_in addr{};
    int len = static_cast<int>(sizeof addr);
    if (::getsockname(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), &len) ==
        SOCKET_ERROR) {
        return 0;
    }
    return static_cast<int>(ntohs(addr.sin_port));
}

// 临时占一个端口再放掉：serve_sni 的监听端口只能从 cfg 里给
int pick_free_port() {
    std::string err;
    svpn::socket_t s = svpn::listen_on("127.0.0.1", 0, &err);
    if (s == svpn::kBadSock) return 0;
    const int port = local_port(s);
    svpn::close_socket(s);
    return port;
}

// ---------------------------------------------------------------- ClientHello 构造
void put16(std::string& s, unsigned v) {
    s.push_back(static_cast<char>((v >> 8) & 0xff));
    s.push_back(static_cast<char>(v & 0xff));
}

void put24(std::string& s, unsigned v) {
    s.push_back(static_cast<char>((v >> 16) & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
    s.push_back(static_cast<char>(v & 0xff));
}

// 最小但结构完整的 ClientHello（TLS1.2 记录层 + handshake 头 + 体积较大的扩展段）。
// 刻意做成「session_id 非空 + server_name 前后各有别的扩展」，覆盖真实客户端的样子。
std::string make_client_hello(const std::string& sni) {
    // server_name 扩展的 wire 格式（RFC 6066）：
    //   list_len(2) + [ name_type(1) + name_len(2) + name ]
    std::string ext_data;
    put16(ext_data, static_cast<unsigned>(3 + sni.size()));   // server_name_list 长度
    ext_data.push_back('\0');                  // name_type = host_name
    put16(ext_data, static_cast<unsigned>(sni.size()));       // 单个 name 长度
    ext_data += sni;

    auto put_ext = [](std::string& out, unsigned type, const std::string& data) {
        put16(out, type);
        put16(out, static_cast<unsigned>(data.size()));
        out += data;
    };

    std::string ext;
    put_ext(ext, 0x000b, std::string("\x01\x00", 2));          // ec_point_formats（server_name 之前）
    put_ext(ext, 0x0000, ext_data);                            // server_name
    put_ext(ext, 0x0016, std::string());                       // encrypt_then_mac（server_name 之后，空体）
    put_ext(ext, 0x0017, std::string());                       // extended_master_secret

    std::string body;
    body.push_back(0x03);                      // client_version
    body.push_back(0x03);
    for (int i = 0; i < 32; ++i) body.push_back(static_cast<char>(i));   // random
    body.push_back(32);                        // session_id 长度 32（真实客户端常有）
    for (int i = 0; i < 32; ++i) body.push_back(static_cast<char>(0xa0 + i));
    put16(body, 4);                            // cipher_suites 长度
    put16(body, 0xc02f);                       // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
    put16(body, 0x002f);                       // TLS_RSA_WITH_AES_128_CBC_SHA
    body.push_back(0x01);                      // compression_methods 长度
    body.push_back(0x00);                      // null
    put16(body, static_cast<unsigned>(ext.size()));
    body += ext;

    std::string hs;
    hs.push_back(0x01);                        // handshake type = ClientHello
    put24(hs, static_cast<unsigned>(body.size()));
    hs += body;

    std::string rec;
    rec.push_back(0x16);                       // handshake
    rec.push_back(0x03);
    rec.push_back(0x01);
    put16(rec, static_cast<unsigned>(hs.size()));
    rec += hs;
    return rec;
}

// ---------------------------------------------------------------- 小工具
svpn::socket_t accept_timeout(svpn::socket_t ls, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(static_cast<SOCKET>(ls), &rf);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<long>(ms % 1000) * 1000;
    if (::select(0, &rf, nullptr, nullptr, &tv) != 1) return svpn::kBadSock;
    return svpn::accept_one(ls, nullptr);
}

// 服务线程可能还没 listen()，重试到超时为止
svpn::socket_t dial_local(int port, int io_ms) {
    const std::int64_t deadline = svpn::now_ms() + 5000;
    while (svpn::now_ms() < deadline) {
        svpn::socket_t s = svpn::tcp_connect_ip("127.0.0.1", port, io_ms);
        if (s != svpn::kBadSock) return s;
        nap(20);
    }
    return svpn::kBadSock;
}

struct SniServer {
    svpn::Config cfg;
    svpn::Stats st;
    std::atomic<bool> stop{false};
    std::thread th;
    int rc{-1};

    ~SniServer() { shutdown(); }

    void start(int target_port) {
        th = std::thread([this, target_port] { rc = svpn::serve_sni_to(cfg, st, stop, target_port); });
    }
    void shutdown() {
        stop.store(true, std::memory_order_release);
        if (th.joinable()) th.join();
    }
};

// ---------------------------------------------------------------- 用例 1/2
void test_forward_and_whitelist() {
    std::printf("\n=== 用例 1/2：ClientHello 逐字节转发 + 白名单判定 ===\n");

    std::string err;
    svpn::socket_t ls_up = svpn::listen_on("127.0.0.1", 0, &err);
    check(ls_up != svpn::kBadSock, "假上游监听成功");
    if (ls_up == svpn::kBadSock) return;
    const int up_port = local_port(ls_up);

    SniServer srv;
    srv.cfg.cdn.sni_port = pick_free_port();
    check(srv.cfg.cdn.sni_port > 0, "拿到测试用高位端口");
    srv.cfg.cdn.hosts = {"steampowered.com"};
    srv.cfg.ip_map["store.steampowered.com"] = "127.0.0.1";
    srv.cfg.connect_timeout = 5;
    srv.start(up_port);

    const std::string hello = make_client_hello("store.steampowered.com");
    check(hello.size() > 60 && static_cast<unsigned char>(hello[0]) == 0x16,
          "构造的 ClientHello 合法（记录层 0x16）");

    svpn::socket_t cli = dial_local(srv.cfg.cdn.sni_port, 5000);
    check(cli != svpn::kBadSock, "客户端连上 SNI 监听");
    if (cli != svpn::kBadSock) {
        // 拆成三片发：验证「边收边拼」
        const std::size_t cut1 = hello.size() / 3;
        const std::size_t cut2 = hello.size() * 2 / 3;
        bool sent = svpn::send_all(cli, hello.data(), cut1, 3000);
        nap(20);
        sent = sent && svpn::send_all(cli, hello.data() + cut1, cut2 - cut1, 3000);
        nap(20);
        sent = sent && svpn::send_all(cli, hello.data() + cut2, hello.size() - cut2, 3000);
        check(sent, "客户端分三片发出 ClientHello");

        svpn::socket_t up = accept_timeout(ls_up, 5000);
        check(up != svpn::kBadSock, "白名单命中 -> 已拨号上游");
        if (up != svpn::kBadSock) {
            std::string got(hello.size(), '\0');
            const bool read_ok = svpn::recv_exact(up, got.data(), got.size(), 5000);
            check(read_ok, "上游收到完整 ClientHello");
            check(read_ok && got == hello, "上游字节与客户端发出的逐字节一致（含 ClientHello）");

            const std::string reply = "SERVER-HELLO-REPLY-0123456789";
            svpn::send_all(up, reply.data(), reply.size(), 3000);
            std::string back(reply.size(), '\0');
            const bool back_ok = svpn::recv_exact(cli, back.data(), back.size(), 5000);
            check(back_ok && back == reply, "上游回包原样回到客户端（双向中继）");
            svpn::close_socket(up);
        }
        svpn::close_socket(cli);
    }

    // SNI 带大写 + 尾点：归一化后必须仍命中白名单
    {
        const std::string shout = make_client_hello("STORE.SteamPowered.COM.");
        svpn::socket_t c2 = dial_local(srv.cfg.cdn.sni_port, 5000);
        check(c2 != svpn::kBadSock, "大小写/尾点形式的连接被接受");
        if (c2 != svpn::kBadSock) {
            svpn::send_all(c2, shout.data(), shout.size(), 3000);
            svpn::socket_t up2 = accept_timeout(ls_up, 5000);
            check(up2 != svpn::kBadSock, "大写+尾点 SNI 仍命中白名单");
            if (up2 != svpn::kBadSock) {
                std::string got2(shout.size(), '\0');
                check(svpn::recv_exact(up2, got2.data(), got2.size(), 5000) && got2 == shout,
                      "大写+尾点连接同样逐字节透传");
                svpn::close_socket(up2);
            }
            svpn::close_socket(c2);
        }
    }

    // 白名单外：example.com 必须被拒，且不产生上游拨号
    const std::string bad_hello = make_client_hello("example.com");
    svpn::socket_t bad = dial_local(srv.cfg.cdn.sni_port, 5000);
    check(bad != svpn::kBadSock, "白名单外连接仍能连上监听（再被踢掉）");
    if (bad != svpn::kBadSock) {
        svpn::send_all(bad, bad_hello.data(), bad_hello.size(), 3000);
        char buf[64];
        const int n = svpn::recv_some(bad, buf, sizeof buf, 3000);
        check(n == 0, "白名单外 SNI(example.com) 被拒：读到 EOF");
        svpn::close_socket(bad);
    }
    check(accept_timeout(ls_up, 300) == svpn::kBadSock, "被拒的请求没有向上游拨号");

    // 非 TLS 首包：不该等满 10 秒
    const std::int64_t t0 = svpn::now_ms();
    svpn::socket_t plain = dial_local(srv.cfg.cdn.sni_port, 5000);
    if (plain != svpn::kBadSock) {
        const std::string junk = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        svpn::send_all(plain, junk.data(), junk.size(), 3000);
        char buf[64];
        const int n = svpn::recv_some(plain, buf, sizeof buf, 5000);
        const std::int64_t cost = svpn::now_ms() - t0;
        check(n == 0, "非 TLS 首包被拒：读到 EOF");
        check(cost < 3000, "非 TLS 首包立刻拒绝（未傻等 10s）");
        svpn::close_socket(plain);
    }

    // 16KB 上限：首字节是 0x16 但长度字段撒谎，读满 max_head 仍无 SNI
    const std::int64_t t1 = svpn::now_ms();
    svpn::socket_t fat = dial_local(srv.cfg.cdn.sni_port, 5000);
    if (fat != svpn::kBadSock) {
        std::string blob(16 * 1024, '\0');
        blob[0] = static_cast<char>(0x16);
        blob[1] = 0x03;
        blob[2] = 0x01;
        blob[3] = static_cast<char>(0xff);      // 谎报 65535 字节记录长度
        blob[4] = static_cast<char>(0xff);
        for (std::size_t i = 5; i < blob.size(); ++i) blob[i] = static_cast<char>(0x41 + (i % 26));
        svpn::send_all(fat, blob.data(), blob.size(), 3000);
        char buf[64];
        const int n = svpn::recv_some(fat, buf, sizeof buf, 5000);
        check(n == 0, "读满 16KB 仍无 SNI：拒绝");
        check(svpn::now_ms() - t1 < 4000, "16KB 上限路径不拖时间");
        svpn::close_socket(fat);
    }

    srv.shutdown();
    svpn::close_socket(ls_up);
    check(srv.rc == 0, "stop 后 serve_sni 正常退出（rc=0）");
    check(srv.st.conns.load() == 5, "st.conns 统计 5 条连接");
    check(srv.st.errors.load() == 3, "st.errors 统计 3 次拒绝");
    check(srv.st.up_bytes.load() >= hello.size(), "up_bytes 累加了 ClientHello");
}

// ---------------------------------------------------------------- 用例 3
void test_empty_allow() {
    std::printf("\n=== 用例 3：白名单为空 => 全部拒绝 ===\n");

    std::string err;
    svpn::socket_t ls_up = svpn::listen_on("127.0.0.1", 0, &err);
    check(ls_up != svpn::kBadSock, "假上游监听成功");
    if (ls_up == svpn::kBadSock) return;
    const int up_port = local_port(ls_up);

    SniServer srv;                                  // cdn.hosts 保持空
    srv.cfg.cdn.sni_port = pick_free_port();
    srv.cfg.connect_timeout = 5;
    srv.start(up_port);

    const std::string hello = make_client_hello("store.steampowered.com");
    svpn::socket_t cli = dial_local(srv.cfg.cdn.sni_port, 5000);
    check(cli != svpn::kBadSock, "白名单为空时仍监听（好让客户端拿到 RST 而不是连接被拒)");
    if (cli != svpn::kBadSock) {
        svpn::send_all(cli, hello.data(), hello.size(), 3000);
        char buf[64];
        const int n = svpn::recv_some(cli, buf, sizeof buf, 3000);
        check(n == 0, "白名单为空：即使 SNI 合法也被拒（EOF）");
        svpn::close_socket(cli);
    }
    check(accept_timeout(ls_up, 300) == svpn::kBadSock, "白名单为空：绝不拨号上游");

    srv.shutdown();
    svpn::close_socket(ls_up);
    check(srv.rc == 0, "stop 后正常退出");
    check(srv.st.errors.load() == 1, "拒绝计入 errors");
}

// ---------------------------------------------------------------- 用例 4
void test_listen_conflict() {
    std::printf("\n=== 用例 4：端口被占 => 返回非 0 ===\n");

    std::string err;
    const int port = pick_free_port();
    svpn::socket_t holder = svpn::listen_on("127.0.0.1", port, &err);
    check(holder != svpn::kBadSock, "先占用端口");
    if (holder == svpn::kBadSock) return;

    SniServer srv;
    srv.cfg.cdn.sni_port = port;
    srv.cfg.cdn.hosts = {"steampowered.com"};
    srv.start(443);
    srv.shutdown();
    check(srv.rc != 0, "bind 失败时 serve_sni 返回非 0");
    svpn::close_socket(holder);
}

}  // namespace

// ============================================================ 临时桩
// A1 的 rules.cpp / A3 的 tls.cpp 还没落地时用来链接；语义按 Python 版
// rules.py(_norm / match_suffix) 与 forward.py(extract_sni) 逐条照抄。
// 真模块就绪后加 -DSNI_TEST_REAL_RULES / -DSNI_TEST_REAL_TLS 即可停用。
#ifndef SNI_TEST_REAL_RULES
namespace svpn {

std::string normalize_host(std::string h) {
    const std::size_t b = h.find_first_not_of(" \t\r\n");
    const std::size_t e = h.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    h = h.substr(b, e - b + 1);
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!h.empty() && h.back() == '.') h.pop_back();
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    return h;
}

bool match_suffix(const std::string& host, const std::string& suffix) {
    const std::string h = normalize_host(host);
    if (h.empty()) return false;
    std::string s = suffix;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t i = 0;
    while (i < s.size() && s[i] == '.') ++i;
    s = s.substr(i);
    if (s.empty()) return false;
    return h == s || (h.size() > s.size() + 1 && h.compare(h.size() - s.size() - 1, s.size() + 1, "." + s) == 0);
}

}  // namespace svpn
#endif

#ifndef SNI_TEST_REAL_TLS
namespace svpn {

bool parse_client_hello_sni(const unsigned char* buf, std::size_t n, std::string* sni) {
    if (buf == nullptr || n < 5 || buf[0] != 0x16) return false;
    const std::size_t rec_len = (static_cast<std::size_t>(buf[3]) << 8) | buf[4];
    if (n < 5 + rec_len) return false;
    const unsigned char* hs = buf + 5;
    const std::size_t hlen = rec_len;
    if (hlen < 1 || hs[0] != 0x01) return false;
    std::size_t p = 4;                                   // hs type(1) + len(3)
    if (p + 2 + 32 > hlen) return false;
    p += 2 + 32;
    if (p >= hlen) return false;
    p += 1 + static_cast<std::size_t>(hs[p]);            // session_id
    if (p + 2 > hlen) return false;
    p += 2 + ((static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1]);   // cipher_suites
    if (p >= hlen) return false;
    p += 1 + static_cast<std::size_t>(hs[p]);            // compression
    if (p + 2 > hlen) return false;
    const std::size_t ext_total = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
    p += 2;
    const std::size_t end = std::min(p + ext_total, hlen);
    while (p + 4 <= end) {
        const std::size_t etype = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
        const std::size_t elen = (static_cast<std::size_t>(hs[p + 2]) << 8) | hs[p + 3];
        p += 4;
        if (etype == 0x0000) {
            if (p + elen > hlen || elen < 5) return false;
            const unsigned char* seg = hs + p;
            const std::size_t nlen = (static_cast<std::size_t>(seg[3]) << 8) | seg[4];
            if (5 + nlen > elen) return false;
            std::string name;
            name.reserve(nlen);
            for (std::size_t i = 0; i < nlen; ++i) {
                name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(seg[5 + i]))));
            }
            while (!name.empty() && name.back() == '.') name.pop_back();
            if (name.empty()) return false;
            if (sni) *sni = name;
            return true;
        }
        p += elen;
    }
    return false;
}

}  // namespace svpn
#endif

int main() {
    svpn::console_utf8();
    if (!svpn::net_init()) {
        std::printf("[FAIL] WSAStartup 失败\n");
        return 1;
    }
    test_forward_and_whitelist();
    test_empty_allow();
    test_listen_conflict();

    std::printf("\n%s 失败项 %d\n", g_fail == 0 ? "[PASS]" : "[FAIL]", g_fail);
    return g_fail == 0 ? 0 : 1;
}
