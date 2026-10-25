// A2 net 模块自测：listen/connect/竞速/DNS/中继/句柄泄漏。
//
// 构建（在 svpn-cpp 根目录，一行）：
//   D:/LE/AI/tools/mingw64/bin/g++.exe -std=c++20 -O2 -Wall -Wextra -finput-charset=UTF-8
//       -fexec-charset=UTF-8 -I src tests/test_net.cpp src/net.cpp src/util.cpp src/json.cpp
//       -o test_net.exe -lws2_32
//
// 全部用例跑在本机回环上，不需要外网；唯一涉及外网语义的用例（不可达 IP 的超时）
// 只断言「耗时上界」，允许它真的连上（某些环境有透明代理）。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "svpn.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace svpn;

namespace {

int g_fail = 0;
int g_pass = 0;

void report(bool ok, const char* file, int line, const char* what) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s   (%s:%d)\n", what, file, line);
    }
}

#define CHECK(cond) report((cond), __FILE__, __LINE__, #cond)
#define CHECKF(cond, ...)                       \
    do {                                        \
        char _m[512];                           \
        std::snprintf(_m, sizeof _m, __VA_ARGS__); \
        report((cond), __FILE__, __LINE__, _m);  \
    } while (0)

int local_port(socket_t s) {
    sockaddr_in a{};
    int len = static_cast<int>(sizeof a);
    if (getsockname(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&a), &len) == SOCKET_ERROR)
        return 0;
    return ntohs(a.sin_port);
}

// 关掉一个监听器之前先拿到它占的端口，用来制造「已关闭的端口」
int ephemeral_port_then_close() {
    std::string err;
    socket_t ls = listen_on("127.0.0.1", 0, &err);
    if (ls == kBadSock) return 0;
    int p = local_port(ls);
    close_socket(ls);
    return p;
}

// ---------------------------------------------------------------- 1. listen + 回显
void test_listen_and_echo() {
    std::printf("[1] listen_on / tcp_connect_ip / send_all / recv_exact / peer_ip\n");
    std::string err = "x";
    socket_t ls = listen_on("127.0.0.1", 0, &err);
    CHECKF(ls != kBadSock, "listen_on(127.0.0.1, 0) 成功（err=\"%s\"）", err.c_str());
    if (ls == kBadSock) return;
    const int port = local_port(ls);
    CHECKF(port > 0, "getsockname 拿到真实端口 = %d", port);

    std::atomic<bool> served{false};
    std::atomic<bool> peer_matched{false};
    std::thread echo([&] {
        std::string peer;
        socket_t c = accept_one(ls, &peer);
        if (c == kBadSock) return;
        peer_matched = (peer == "127.0.0.1");
        std::vector<char> b(4096);
        for (;;) {
            int n = recv_some(c, b.data(), b.size(), 3000);
            if (n <= 0) break;
            if (!send_all(c, b.data(), static_cast<std::size_t>(n), 3000)) break;
        }
        close_socket(c);
        served = true;
    });

    socket_t c = tcp_connect_ip("127.0.0.1", port, 2000);
    CHECKF(c != kBadSock, "tcp_connect_ip(127.0.0.1, %d, 2000) 成功", port);
    if (c != kBadSock) {
        const char* msg = "hello-svpn-net";
        const std::size_t n = std::strlen(msg);
        CHECKF(send_all(c, msg, n, 2000), "send_all %zu 字节", n);
        char rbuf[64] = {0};
        bool got = recv_exact(c, rbuf, n, 2000);
        CHECKF(got && std::memcmp(rbuf, msg, n) == 0, "recv_exact 回显一致 = \"%.*s\"",
               static_cast<int>(n), rbuf);
        std::string pip = peer_ip(c);
        CHECKF(pip == "127.0.0.1", "peer_ip() = \"%s\"", pip.c_str());
        close_socket(c);
    }
    echo.join();
    CHECK(served.load());
    CHECK(peer_matched.load());
    close_socket(ls);

    // listen_on 二次绑定同一端口必须失败（没有 SO_REUSEADDR）
    std::string e2;
    socket_t ls2 = listen_on("127.0.0.1", port, &e2);
    CHECKF(ls2 != kBadSock, "关闭后可以重新绑定 %d", port);
    if (ls2 != kBadSock) {
        std::string e3;
        socket_t ls3 = listen_on("127.0.0.1", port, &e3);
        CHECKF(ls3 == kBadSock, "重复绑定同一端口被拒（err=\"%s\"）", e3.c_str());
        if (ls3 != kBadSock) close_socket(ls3);
        close_socket(ls2);
    }
}

// ---------------------------------------------------------------- 2. 死端口必须快速失败
void test_dead_port_fast() {
    std::printf("[2] 死端口 tcp_connect 必须快速返回（不许卡 12 秒）\n");
    const int dead = ephemeral_port_then_close();
    CHECKF(dead > 0, "拿到一个已关闭的端口 = %d", dead);
    if (dead <= 0) return;

    std::int64_t t0 = now_ms();
    socket_t s = tcp_connect_ip("127.0.0.1", dead, 800);
    std::int64_t dt = now_ms() - t0;
    CHECKF(s == kBadSock, "tcp_connect_ip 到死端口返回 kBadSock");
    // 本机对「回环已关闭端口」的连接是**静默丢包**（不发 RST，Python 阻塞版也要等满
    // 2s），所以这里只能等满自己给的 800ms。要断言的是「上界由我们决定」，不是「立刻」。
    CHECKF(dt <= 1100, "tcp_connect_ip 耗时 %lldms ≤ 800+250ms（超时由事件等待掐死，不是 12s）",
           static_cast<long long>(dt));
    close_socket(s);

    std::string err;
    t0 = now_ms();
    s = tcp_connect("127.0.0.1", dead, 800, &err);
    dt = now_ms() - t0;
    CHECKF(s == kBadSock, "tcp_connect 到死端口返回 kBadSock（err=\"%s\"）", err.c_str());
    CHECKF(dt < 900, "tcp_connect 耗时 %lldms < 900ms", static_cast<long long>(dt));
    close_socket(s);

    // 不可达 IP：只断言耗时上界。有些环境挂着透明代理，可能真的连上。
    t0 = now_ms();
    socket_t b = tcp_connect_ip("192.0.2.1", 443, 300);
    dt = now_ms() - t0;
    CHECKF(dt < 900, "不可达 IP 192.0.2.1:443 在 %lldms 内返回（超时 300ms 生效，rc=%s）",
           static_cast<long long>(dt), b == kBadSock ? "kBadSock" : "connected");
    close_socket(b);

    // 活端口必须「一连上就返回」，不能傻等满超时（成功判定走 writefds + SO_ERROR）
    std::string e0;
    socket_t ls = listen_on("127.0.0.1", 0, &e0);
    CHECK(ls != kBadSock);
    if (ls != kBadSock) {
        const int live = local_port(ls);
        t0 = now_ms();
        socket_t ok = tcp_connect_ip("127.0.0.1", live, 2000);
        dt = now_ms() - t0;
        CHECKF(ok != kBadSock && dt < 300, "活端口 %d 在 %lldms 内连上（不等满 2000ms）", live,
               static_cast<long long>(dt));
        close_socket(ok);
        close_socket(ls);
    }
}

// ---------------------------------------------------------------- 3. 双向中继
void test_relay() {
    std::printf("[3] relay_stream 双向转发 1MB + 字节计数\n");
    constexpr std::size_t kTotal = 1024 * 1024;
    constexpr std::size_t kBack = 1000;

    // 目标端（相当于真实服务器）
    std::string err;
    socket_t us = listen_on("127.0.0.1", 0, &err);
    CHECK(us != kBadSock);
    if (us == kBadSock) return;
    const int uport = local_port(us);

    // 浏览器端
    socket_t ds = listen_on("127.0.0.1", 0, &err);
    CHECK(ds != kBadSock);
    if (ds == kBadSock) {
        close_socket(us);
        return;
    }
    const int dport = local_port(ds);

    std::atomic<std::size_t> sink_bytes{0};
    std::atomic<std::uint64_t> sink_sum{0};
    std::atomic<bool> sink_done{false};

    std::thread target([&] {                       // 目标端：先吐 1000 字节，再收满
        std::string peer;
        socket_t k = accept_one(us, &peer);
        if (k == kBadSock) return;
        std::vector<char> back(kBack, 'd');
        send_all(k, back.data(), back.size(), 3000);
        std::vector<char> b(16384);
        std::size_t total = 0;
        std::uint64_t sum = 0;
        for (;;) {
            int n = recv_some(k, b.data(), b.size(), 5000);
            if (n <= 0) break;
            total += static_cast<std::size_t>(n);
            for (int i = 0; i < n; ++i) sum += static_cast<unsigned char>(b[i]);
        }
        sink_bytes = total;
        sink_sum = sum;
        close_socket(k);
        sink_done = true;
    });

    socket_t b = tcp_connect_ip("127.0.0.1", uport, 2000);
    CHECK(b != kBadSock);
    if (b == kBadSock) {
        close_socket(us);
        close_socket(ds);
        target.detach();
        return;
    }

    std::atomic<std::size_t> browser_read{0};
    std::thread browser([&] {                      // 浏览器端：推 1MB，然后读到 EOF
        std::string peer;
        socket_t a = accept_one(ds, &peer);
        if (a == kBadSock) return;
        std::vector<char> payload(kTotal);
        std::uint64_t want_sum = 0;
        for (std::size_t i = 0; i < kTotal; ++i) {
            payload[i] = static_cast<char>(i & 0xFF);
            want_sum += static_cast<unsigned char>(payload[i]);
        }
        bool ok = send_all(a, payload.data(), payload.size(), 8000);
        if (!ok) std::printf("  [warn] 浏览器端 send_all 中途失败\n");
        ::shutdown(static_cast<SOCKET>(a), SD_SEND);   // 半关闭：数据已发完
        std::vector<char> b2(4096);
        std::size_t got = 0;
        for (;;) {
            int n = recv_some(a, b2.data(), b2.size(), 5000);
            if (n <= 0) break;
            got += static_cast<std::size_t>(n);
        }
        browser_read = got;
        close_socket(a);
        CHECKF(want_sum > 0, "1MB 载荷校验和 = %llu", static_cast<unsigned long long>(want_sum));
    });

    socket_t a = tcp_connect_ip("127.0.0.1", dport, 2000);
    CHECK(a != kBadSock);
    if (a == kBadSock) {
        browser.detach();
        target.detach();
        close_socket(b);
        close_socket(us);
        close_socket(ds);
        return;
    }

    std::uint64_t up = 0, down = 0;
    std::int64_t t0 = now_ms();
    int rc = relay_stream(a, b, 8000, &up, &down);
    std::int64_t dt = now_ms() - t0;

    CHECKF(rc == 0, "relay_stream 返回 0（实际 %d，耗时 %lldms）", rc,
           static_cast<long long>(dt));
    CHECKF(up == kTotal, "*up = %llu 字节（期望 %zu）", static_cast<unsigned long long>(up), kTotal);
    CHECKF(down == kBack, "*down = %llu 字节（期望 %zu）", static_cast<unsigned long long>(down),
           kBack);

    browser.join();
    target.join();
    CHECKF(sink_bytes.load() == kTotal, "对端实收 %zu 字节（期望 %zu）", sink_bytes.load(), kTotal);
    CHECKF(sink_sum.load() > 0, "对端收到的数据校验和 = %llu",
           static_cast<unsigned long long>(sink_sum.load()));
    CHECKF(browser_read.load() == kBack, "浏览器端收到回程 %zu 字节（期望 %zu）",
           browser_read.load(), kBack);

    close_socket(a);
    close_socket(b);
    close_socket(us);
    close_socket(ds);
    CHECK(sink_done.load());
}

// ---------------------------------------------------------------- 4. 中继空闲退出
void test_relay_idle() {
    std::printf("[4] relay_stream 空闲超时退出\n");
    std::string err;
    socket_t ls = listen_on("127.0.0.1", 0, &err);
    CHECK(ls != kBadSock);
    if (ls == kBadSock) return;
    const int port = local_port(ls);

    socket_t b = tcp_connect_ip("127.0.0.1", port, 1000);
    std::string peer;
    socket_t srv = accept_one(ls, &peer);
    CHECK(b != kBadSock && srv != kBadSock);
    if (b != kBadSock && srv != kBadSock) {
        std::uint64_t up = 0, down = 0;
        std::int64_t t0 = now_ms();
        int rc = relay_stream(b, srv, 300, &up, &down);
        std::int64_t dt = now_ms() - t0;
        CHECKF(rc == 0 && dt >= 250 && dt < 1500, "idle=300ms 时 %lldms 后正常返回 rc=%d",
               static_cast<long long>(dt), rc);
        CHECK(up == 0 && down == 0);
    }
    close_socket(b);
    close_socket(srv);
    close_socket(ls);
}

// ---------------------------------------------------------------- 5. DNS
void test_resolve() {
    std::printf("[5] resolve_system\n");
    std::vector<std::string> ips = resolve_system("localhost", 3000);
    bool has_lo = false;
    std::string joined;
    for (const auto& ip : ips) {
        if (ip == "127.0.0.1") has_lo = true;
        joined += ip + " ";
    }
    CHECKF(has_lo, "resolve_system(\"localhost\", 3000) 含 127.0.0.1 → [%s]", joined.c_str());

    std::vector<std::string> lit = resolve_system("10.1.2.3", 1000);
    CHECKF(lit.size() == 1 && lit[0] == "10.1.2.3", "字面量 IP 原样返回 [%s]",
           lit.empty() ? "" : lit[0].c_str());

    CHECK(resolve_system("", 1000).empty());
    CHECK(resolve_system(std::string(300, 'a'), 1000).empty());
    CHECKF(resolve_system("no-such-host.invalid", 2000).empty(),
           "不存在的域名返回空（不抛不卡）");

    std::int64_t t0 = now_ms();
    std::vector<std::string> slow = resolve_system("no-such-host.invalid", 1);
    std::int64_t dt = now_ms() - t0;
    CHECKF(dt <= 1200, "1ms 超时的解析在 %lldms 内返回（不卡调用方）",
           static_cast<long long>(dt));
}

// ---------------------------------------------------------------- 6. tcp_connect（域名 + 竞速）
void test_tcp_connect_domain() {
    std::printf("[6] tcp_connect（域名 → 多 IP 竞速）\n");
    std::string err;
    socket_t ls = listen_on("127.0.0.1", 0, &err);
    CHECK(ls != kBadSock);
    if (ls == kBadSock) return;
    const int port = local_port(ls);

    std::atomic<int> accepted{0};
    std::thread srv([&] {
        for (int i = 0; i < 1; ++i) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            char ch = 0;
            if (recv_exact(c, &ch, 1, 2000)) send_all(c, &ch, 1, 2000);
            close_socket(c);
            ++accepted;
        }
    });

    std::int64_t t0 = now_ms();
    socket_t c = tcp_connect("localhost", port, 2000, &err);
    std::int64_t dt = now_ms() - t0;
    CHECKF(c != kBadSock, "tcp_connect(\"localhost\", %d) 成功（err=\"%s\", %lldms）", port,
           err.c_str(), static_cast<long long>(dt));
    if (c != kBadSock) {
        char q = 'Z';
        CHECK(send_all(c, &q, 1, 2000));
        char r = 0;
        CHECK(recv_exact(c, &r, 1, 2000) && r == 'Z');
        close_socket(c);
    }
    srv.join();
    close_socket(ls);
    CHECK(accepted.load() == 1);

    // 域名解析失败路径
    std::string e2;
    socket_t bad = tcp_connect("no-such-host.invalid", 80, 1500, &e2);
    CHECKF(bad == kBadSock, "解析失败的域名返回 kBadSock（err=\"%s\"）", e2.c_str());
    close_socket(bad);

    // 端口越界
    std::string e3;
    CHECK(tcp_connect("127.0.0.1", 0, 500, &e3) == kBadSock);
    CHECK(tcp_connect("127.0.0.1", 70000, 500, &e3) == kBadSock);
    CHECK(tcp_connect("", 80, 500, &e3) == kBadSock);
}

// ---------------------------------------------------------------- 7. 句柄不泄漏
void test_no_fd_leak() {
    std::printf("[7] 50 次连接（含失败）后句柄数不涨\n");
    std::string err;
    socket_t ls = listen_on("127.0.0.1", 0, &err);
    CHECK(ls != kBadSock);
    if (ls == kBadSock) return;
    const int port = local_port(ls);

    const int rounds = 50;
    std::thread srv([&] {
        for (int i = 0; i < rounds; ++i) {
            socket_t c = accept_one(ls, nullptr);
            if (c == kBadSock) return;
            char ch = 0;
            recv_exact(c, &ch, 1, 1000);
            close_socket(c);
        }
    });

    // 预热：让 CRT / WinSock 的一次性分配先发生
    for (int i = 0; i < 5; ++i) {
        socket_t c = tcp_connect_ip("127.0.0.1", port, 1000);
        close_socket(c);
    }
    DWORD before = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);

    const int dead = ephemeral_port_then_close();
    for (int i = 0; i < rounds; ++i) {
        socket_t c = tcp_connect_ip("127.0.0.1", port, 1000);
        if (c == kBadSock) continue;
        char ch = 'a';
        send_all(c, &ch, 1, 1000);
        close_socket(c);
        close_socket(tcp_connect_ip("127.0.0.1", dead, 300));   // 失败路径也要不漏
    }
    srv.join();

    DWORD after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &after);
    const long long delta = static_cast<long long>(after) - static_cast<long long>(before);
    CHECKF(delta <= 5 && delta >= -5, "句柄数 %lu → %lu（Δ=%lld，允许 ±5）",
           static_cast<unsigned long>(before), static_cast<unsigned long>(after), delta);
    close_socket(ls);
}

// ---------------------------------------------------------------- 8. 工具函数
void test_misc() {
    std::printf("[8] close_socket / set_io_timeout 边界\n");
    close_socket(kBadSock);          // 必须幂等且不炸
    CHECK(true);
    CHECK(!set_io_timeout(kBadSock, 1000));
    CHECK(peer_ip(kBadSock).empty());
    CHECK(accept_one(kBadSock, nullptr) == kBadSock);
    CHECK(send_all(kBadSock, "x", 1, 100) == false);
    CHECK(recv_exact(kBadSock, nullptr, 1, 100) == false);
    CHECK(recv_some(kBadSock, nullptr, 0, 100) == -1);
    CHECK(relay_stream(kBadSock, kBadSock, 100, nullptr, nullptr) == -1);

    std::string err;
    CHECK(listen_on("127.0.0.1", 70000, &err) == kBadSock);
    CHECK(listen_on("999.999.999.999", 80, &err) == kBadSock);
}

}  // namespace

int main() {
    console_utf8();
    if (!net_init()) {
        std::printf("WSAStartup 失败\n");
        return 2;
    }
    test_listen_and_echo();
    test_dead_port_fast();
    test_relay();
    test_relay_idle();
    test_resolve();
    test_tcp_connect_domain();
    test_no_fd_leak();
    test_misc();

    std::printf("\n===== %d 通过 / %d 失败 =====\n", g_pass, g_fail);
    net_shutdown();
    return g_fail == 0 ? 0 : 1;
}
