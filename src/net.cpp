// 网络底层：Winsock 生命周期、非阻塞 connect、系统 DNS（带超时）、多 IP 竞速、
// 完整收发循环、单线程 select 双向中继。
//
// 行为基准：Python 版 svpn/netutil.py（resolve / tcp_probe）与
// svpn/proxy.py 的 _open_tcp / _race_connect / pipe。
//
// 两条硬纪律：
//   1. 连接超时只能靠「非阻塞 connect + select」实现。Windows 上阻塞 connect 的
//      超时不可控（SYN 重传约 21s），getaddrinfo 之后串行阻塞连接会让一次被墙的
//      域名卡满整个超时——这正是 Python 版写 _race_connect 的原因。
//   2. 竞速里除了赢家，其它分支成功建立的连接**必须** close，否则每次拨号漏一个 fd。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "svpn.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace svpn {
namespace {

// ---------------------------------------------------------------- 常量
constexpr int kRaceMax = 4;           // 竞速最多取几个 IP（与 Python 版 ips[:4] 一致）
constexpr int kRaceStaggerMs = 120;    // 第 i 个 IP 延迟 i*120ms 起跑
constexpr std::size_t kRelayBuf = 32 * 1024;
constexpr std::size_t kIoTip = 1u << 20;   // 单次 send/recv 上限

// ---------------------------------------------------------------- 类型转换
SOCKET raw(socket_t s) { return static_cast<SOCKET>(s); }

socket_t wrap(SOCKET s) { return s == INVALID_SOCKET ? kBadSock : static_cast<socket_t>(s); }

// "[::1]" -> "::1"，去首尾空白与方括号
std::string bare_host(const std::string& h) {
    std::string s = trim(h);
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    return s;
}

bool parse_v4(const std::string& h, sockaddr_in* out) {
    if (h.empty() || h.find(':') != std::string::npos) return false;
    return InetPtonA(AF_INET, h.c_str(), &out->sin_addr) == 1;
}

bool parse_v6(const std::string& h, sockaddr_in6* out) {
    if (h.find(':') == std::string::npos) return false;
    return InetPtonA(AF_INET6, h.c_str(), &out->sin6_addr) == 1;
}

bool looks_like_ip(const std::string& h) {
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    if (h.find(':') != std::string::npos) return parse_v6(h, &v6);
    return parse_v4(h, &v4);
}

std::string addr_text(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&ss);
        if (InetNtopA(AF_INET, &v4->sin_addr, buf, sizeof buf)) return buf;
    } else if (ss.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (InetNtopA(AF_INET6, &v6->sin6_addr, buf, sizeof buf)) return buf;
    }
    return {};
}

// ---------------------------------------------------------------- 时间/等待
// deadline<=0 表示不限（返回 -1）；已过期返回 0
int remain_ms(std::int64_t deadline) {
    if (deadline <= 0) return -1;
    std::int64_t left = deadline - now_ms();
    return left <= 0 ? 0 : static_cast<int>(left);
}

// 1 = 就绪 / 0 = 超时 / -1 = 出错
int wait_fd(SOCKET s, bool want_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = static_cast<long>(timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int n = want_write ? ::select(0, nullptr, &set, nullptr, ptv)
                       : ::select(0, &set, nullptr, nullptr, ptv);
    if (n == SOCKET_ERROR) return -1;
    return n > 0 ? 1 : 0;
}

// 非阻塞 connect，用 WSAEventSelect 等落定。cancel 非空时可被外部立刻打断。
//
// 为什么不用 select：select 是阻塞调用，没法从别的线程叫醒。竞速里赢家一到，剩下的
// 分支还卡在 select 里等自己的超时，join 会把「已经成功」的拨号拖满整个 timeout——
// 实测 localhost 的 ::1 分支就是这样把 135ms 的成功等到 2016ms。事件 + 取消句柄
// 才能让输家在微秒级退出，然后照样 join。
socket_t connect_ip_wait(const std::string& ip, int port, int timeout_ms, HANDLE cancel) {
    if (!net_init()) return kBadSock;
    if (port <= 0 || port > 65535) return kBadSock;

    const std::string host = bare_host(ip);
    if (host.empty()) return kBadSock;

    sockaddr_storage ss{};
    int slen = 0;
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    if (parse_v4(host, &v4)) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(static_cast<u_short>(port));
        std::memcpy(&ss, &v4, sizeof v4);
        slen = static_cast<int>(sizeof v4);
    } else if (parse_v6(host, &v6)) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(static_cast<u_short>(port));
        std::memcpy(&ss, &v6, sizeof v6);
        slen = static_cast<int>(sizeof v6);
    } else {
        return kBadSock;
    }

    SOCKET s = ::socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kBadSock;

    WSAEVENT ev = WSACreateEvent();
    if (ev == WSA_INVALID_EVENT) {
        closesocket(s);
        return kBadSock;
    }
    // 必须在 connect 之前绑定事件（FD_CONNECT 可能在 connect 返回到绑定之间就绪），
    // 顺带把 socket 置成非阻塞——所以这里不需要额外的 FIONBIO。
    if (WSAEventSelect(s, ev, FD_CONNECT | FD_CLOSE) == SOCKET_ERROR) {
        WSACloseEvent(ev);
        closesocket(s);
        return kBadSock;
    }

    bool ok = false;
    bool cancelled = false;
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&ss), slen);
    if (rc == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        HANDLE hs[2] = {ev, cancel};
        const DWORD cnt = cancel != nullptr ? 2u : 1u;
        const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
        if (WaitForMultipleObjects(cnt, hs, FALSE, wait) == WAIT_OBJECT_0) {
            WSANETWORKEVENTS ne{};
            if (WSAEnumNetworkEvents(s, ev, &ne) == 0 && (ne.lNetworkEvents & FD_CONNECT) != 0 &&
                ne.iErrorCode[FD_CONNECT_BIT] == 0) {
                ok = true;
            }
            // 已经被赢家挤掉的分支：连上了也不算数，直接丢弃
            if (ok && cancel != nullptr && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
                cancelled = true;
            }
        }
    }

    WSAEventSelect(s, nullptr, 0);        // 解绑事件，socket 回到阻塞模式
    WSACloseEvent(ev);
    if (!ok || cancelled) {
        closesocket(s);
        return kBadSock;
    }
    u_long nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    socket_t out = wrap(s);
    set_io_timeout(out, timeout_ms);
    return out;
}

// 把 n 字节全写进去；写不动就用 select 等可写（idle_ms<=0 表示无限等）
bool push_all(SOCKET s, const char* p, std::size_t n, int idle_ms) {
    const std::int64_t deadline = idle_ms > 0 ? now_ms() + idle_ms : 0;
    std::size_t sent = 0;
    while (sent < n) {
        int want = static_cast<int>(std::min<std::size_t>(n - sent, kIoTip));
        int rc = ::send(s, p + sent, want, 0);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) return false;
        }
        int wait = deadline > 0 ? remain_ms(deadline) : -1;
        if (wait == 0) return false;
        if (wait_fd(s, true, wait) != 1) return false;
    }
    return true;
}

// ---------------------------------------------------------------- 竞速
// 多 IP happy-eyeballs：第 i 个延迟 i*120ms 起跑，第一个成功者胜出。
// deadline 是全体的总预算（0 = 不限）；所有分支都会 join，非赢家的连接一律关掉。
socket_t race_connect(const std::vector<std::string>& ips, int port, std::int64_t deadline) {
    const std::size_t n = std::min<std::size_t>(ips.size(), static_cast<std::size_t>(kRaceMax));
    if (n == 0) return kBadSock;

    struct Race {
        std::mutex mu;
        std::condition_variable cv;
        socket_t win{kBadSock};
        std::size_t finished{0};
    } st;

    // 赢家一出现就 SetEvent：输家从 WaitForMultipleObjects 里立刻退出来
    HANDLE cancel = WSACreateEvent();

    std::vector<std::thread> pool;
    pool.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        pool.emplace_back([&st, &ips, port, deadline, cancel, i] {
            const int delay = static_cast<int>(i) * kRaceStaggerMs;
            {
                std::unique_lock<std::mutex> lk(st.mu);
                // 起跑延迟用 cv 等：赢家一出现立刻唤醒，不必实打实睡满 stagger
                if (delay > 0) {
                    st.cv.wait_for(lk, std::chrono::milliseconds(delay),
                                   [&st] { return st.win != kBadSock; });
                }
                if (st.win != kBadSock) {          // 已经有赢家，本轮不必起跑
                    ++st.finished;
                    st.cv.notify_all();
                    return;
                }
            }

            const int left = deadline > 0 ? remain_ms(deadline) : -1;
            socket_t s = kBadSock;
            if (left != 0) s = connect_ip_wait(ips[i], port, left, cancel);

            std::lock_guard<std::mutex> lk(st.mu);
            if (s != kBadSock) {
                if (st.win == kBadSock) {
                    st.win = s;
                    if (cancel != WSA_INVALID_EVENT) WSASetEvent(cancel);
                } else {
                    close_socket(s);               // 迟到的赢家：不关就漏一个 fd
                }
            }
            ++st.finished;
            st.cv.notify_all();
        });
    }

    {
        std::unique_lock<std::mutex> lk(st.mu);
        st.cv.wait(lk, [&st, n] { return st.win != kBadSock || st.finished == n; });
    }
    for (auto& t : pool) {
        if (t.joinable()) t.join();                // 所有分支都 join，不 detach
    }
    if (cancel != WSA_INVALID_EVENT) WSACloseEvent(cancel);
    return st.win;
}

}  // namespace

// ============================================================ 生命周期
bool net_init() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wd{};
        ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    });
    return ok;
}

void net_shutdown() {
    // 故意不做 WSACleanup：还有别的线程在用 socket 时调它会让所有句柄失效，
    // 进程退出时系统自会回收。保持「可空实现」的契约。
}

// ============================================================ 连接
socket_t tcp_connect_ip(const std::string& ip, int port, int timeout_ms) {
    return connect_ip_wait(ip, port, timeout_ms, nullptr);
}

std::vector<std::string> resolve_system(const std::string& host, int timeout_ms) {
    std::vector<std::string> out;
    if (!net_init()) return out;

    const std::string name = bare_host(host);
    if (name.empty() || name.size() > 253) return out;    // DNS 名上限，别拿去浪费一次超时
    if (looks_like_ip(name)) {
        out.push_back(name);
        return out;
    }

    // getaddrinfo 不可中断：放线程里跑，主线程只等到点为止，绝不卡死调用方。
    struct Job {
        std::mutex mu;
        std::condition_variable cv;
        bool done{false};
        std::vector<std::string> ips;
    };
    auto job = std::make_shared<Job>();

    std::thread worker([job, name] {
        std::vector<std::string> ips;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* res = nullptr;
        if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                const void* src = nullptr;
                if (p->ai_family == AF_INET) {
                    src = &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr;
                } else if (p->ai_family == AF_INET6) {
                    src = &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr;
                } else {
                    continue;
                }
                char buf[INET6_ADDRSTRLEN] = {0};
                if (InetNtopA(p->ai_family, src, buf, sizeof buf) == nullptr) continue;
                std::string ip(buf);
                if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(std::move(ip));
            }
            freeaddrinfo(res);
        }
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->ips = std::move(ips);
            job->done = true;
        }
        job->cv.notify_all();
    });

    const int wait = timeout_ms > 0 ? timeout_ms : -1;
    bool finished = false;
    {
        std::unique_lock<std::mutex> lk(job->mu);
        if (wait < 0) {
            job->cv.wait(lk, [&job] { return job->done; });
            finished = true;
        } else {
            finished = job->cv.wait_for(lk, std::chrono::milliseconds(wait),
                                        [&job] { return job->done; });
        }
        if (finished) out = job->ips;
    }
    if (!finished) {
        // 超时：线程只持有 shared_ptr，返回时自己回收；不 detach 就只能陪着一起卡。
        // 这是唯一一个不等 join 的分支，且不会泄漏任何东西（无裸指针、无 fd）。
        worker.detach();
        return {};
    }
    worker.join();
    return out;
}

socket_t tcp_connect(const std::string& host, int port, int timeout_ms, std::string* err) {
    auto fail = [err](const std::string& msg) -> socket_t {
        if (err) *err = msg;
        return kBadSock;
    };

    if (!net_init()) return fail("Winsock 初始化失败");
    const std::string name = bare_host(host);
    if (name.empty()) return fail("空主机名");
    if (port <= 0 || port > 65535) return fail(fmt("端口超出范围: %d", port));

    if (looks_like_ip(name)) {                       // 字面量 IP：直接连，不查 DNS
        socket_t s = tcp_connect_ip(name, port, timeout_ms);
        if (s == kBadSock) return fail(fmt("连接 %s:%d 失败", name.c_str(), port));
        if (err) err->clear();
        return s;
    }

    const std::vector<std::string> ips = resolve_system(name, timeout_ms);
    if (ips.empty()) return fail(fmt("解析失败: %s", name.c_str()));

    const std::int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    socket_t win = race_connect(ips, port, deadline);

    if (win == kBadSock) {
        // 兜底：竞速窗口过后再顺序重试一遍，预算另算一份（总耗时 ≤ 2×timeout）
        const std::int64_t d2 = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
        for (const std::string& ip : ips) {
            const int left = d2 > 0 ? remain_ms(d2) : -1;
            if (d2 > 0 && left == 0) break;
            win = tcp_connect_ip(ip, port, left);
            if (win != kBadSock) break;
        }
    }

    if (win == kBadSock) {
        const std::string msg = fmt("全部 %zu 个 IP 连接失败: %s", ips.size(), name.c_str());
        log_write("WARN", msg);
        return fail(msg);
    }
    set_io_timeout(win, timeout_ms);
    if (err) err->clear();
    return win;
}

bool set_io_timeout(socket_t s, int timeout_ms) {
    if (s == kBadSock) return false;
    const DWORD tv = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0;   // 0 = 不限
    int rc1 = setsockopt(raw(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                         static_cast<int>(sizeof tv));
    int rc2 = setsockopt(raw(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                         static_cast<int>(sizeof tv));
    return rc1 == 0 && rc2 == 0;
}

void close_socket(socket_t s) {
    if (s == kBadSock) return;                       // 幂等：kBadSock 直接返回
    closesocket(raw(s));
}

// ============================================================ 收发
bool send_all(socket_t s, const void* data, std::size_t n, int timeout_ms) {
    if (s == kBadSock) return false;
    if (n == 0) return true;
    if (data == nullptr) return false;

    const std::int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    const char* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < n) {
        int want = static_cast<int>(std::min<std::size_t>(n - sent, kIoTip));
        int rc = ::send(raw(s), p + sent, want, 0);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) return false;
        }
        int wait = deadline > 0 ? remain_ms(deadline) : -1;
        if (wait == 0) return false;
        if (wait_fd(raw(s), true, wait) != 1) return false;
    }
    return true;
}

bool recv_exact(socket_t s, void* buf, std::size_t n, int timeout_ms) {
    if (s == kBadSock) return false;
    if (n == 0) return true;
    if (buf == nullptr) return false;

    const std::int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    char* p = static_cast<char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        int want = static_cast<int>(std::min<std::size_t>(n - got, kIoTip));
        int rc = ::recv(raw(s), p + got, want, 0);
        if (rc > 0) {
            got += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == 0) return false;                    // 对端提前 EOF
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return false;
        int wait = deadline > 0 ? remain_ms(deadline) : -1;
        if (wait == 0) return false;
        if (wait_fd(raw(s), false, wait) != 1) return false;
    }
    return true;
}

int recv_some(socket_t s, void* buf, std::size_t cap, int timeout_ms) {
    if (s == kBadSock || buf == nullptr || cap == 0) return -1;

    const std::int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    for (;;) {
        int want = static_cast<int>(std::min<std::size_t>(cap, kIoTip));
        int rc = ::recv(raw(s), static_cast<char*>(buf), want, 0);
        if (rc >= 0) return rc;                       // 0 = EOF，>0 = 字节数
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) return -1;
        int wait = deadline > 0 ? remain_ms(deadline) : -1;
        if (wait == 0) return -1;
        if (wait_fd(raw(s), false, wait) != 1) return -1;
    }
}

// ============================================================ 中继
int relay_stream(socket_t a, socket_t b, int idle_ms, std::uint64_t* up, std::uint64_t* down) {
    if (a == kBadSock || b == kBadSock || a == b) return -1;
    const SOCKET sa = raw(a);
    const SOCKET sb = raw(b);

    // 全程非阻塞：readiness 一律交给 select。否则空闲连接会在 SO_RCVTIMEO 到点时
    // 收到 WSAETIMEDOUT（是错误不是 wouldblock），好好的长连接会被判死。
    u_long nb = 1;
    ioctlsocket(sa, FIONBIO, &nb);
    ioctlsocket(sb, FIONBIO, &nb);

    std::vector<char> buf(kRelayBuf);
    bool a_open = true;
    bool b_open = true;
    int rc = 0;
    std::int64_t last = now_ms();

    while (a_open || b_open) {
        fd_set rf;
        FD_ZERO(&rf);
        if (a_open) FD_SET(sa, &rf);
        if (b_open) FD_SET(sb, &rf);

        int wait = -1;
        if (idle_ms > 0) {
            std::int64_t left = static_cast<std::int64_t>(idle_ms) - (now_ms() - last);
            if (left <= 0) break;                     // 空闲到点：正常收尾
            wait = static_cast<int>(left);
        }
        timeval tv{};
        timeval* ptv = nullptr;
        if (wait >= 0) {
            tv.tv_sec = wait / 1000;
            tv.tv_usec = static_cast<long>(wait % 1000) * 1000;
            ptv = &tv;
        }
        int n = ::select(0, &rf, nullptr, nullptr, ptv);
        if (n == 0) break;                            // idle 到点，正常返回
        if (n == SOCKET_ERROR) {
            rc = -1;
            break;
        }

        bool moved = false;
        if (a_open && FD_ISSET(sa, &rf)) {            // a → b
            int got = ::recv(sa, buf.data(), static_cast<int>(buf.size()), 0);
            if (got > 0) {
                if (!push_all(sb, buf.data(), static_cast<std::size_t>(got), idle_ms)) {
                    rc = -1;
                    break;
                }
                if (up) *up += static_cast<std::uint64_t>(got);
                moved = true;
            } else if (got == 0) {
                a_open = false;
                ::shutdown(sb, SD_SEND);              // 半关闭：b 还能继续回数据
            } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                rc = -1;
                break;
            }
        }
        if (b_open && FD_ISSET(sb, &rf)) {            // b → a
            int got = ::recv(sb, buf.data(), static_cast<int>(buf.size()), 0);
            if (got > 0) {
                if (!push_all(sa, buf.data(), static_cast<std::size_t>(got), idle_ms)) {
                    rc = -1;
                    break;
                }
                if (down) *down += static_cast<std::uint64_t>(got);
                moved = true;
            } else if (got == 0) {
                b_open = false;
                ::shutdown(sa, SD_SEND);
            } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                rc = -1;
                break;
            }
        }
        if (moved) last = now_ms();
    }

    nb = 0;
    ioctlsocket(sa, FIONBIO, &nb);                    // 交还给调用方时恢复阻塞模式
    ioctlsocket(sb, FIONBIO, &nb);
    return rc;
}

// ============================================================ 地址查询
std::string peer_ip(socket_t s) {
    if (s == kBadSock) return {};
    sockaddr_storage ss{};
    int len = static_cast<int>(sizeof ss);
    if (getpeername(raw(s), reinterpret_cast<sockaddr*>(&ss), &len) == SOCKET_ERROR) return {};
    return addr_text(ss);
}

socket_t listen_on(const std::string& host, int port, std::string* err) {
    auto fail = [err](const std::string& msg) -> socket_t {
        if (err) *err = msg;
        return kBadSock;
    };
    if (!net_init()) return fail("Winsock 初始化失败");
    if (port < 0 || port > 65535) return fail(fmt("端口超出范围: %d", port));

    const std::string name = bare_host(host);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const std::string port_s = std::to_string(port);
    addrinfo* res = nullptr;
    int grc = getaddrinfo(name.empty() ? nullptr : name.c_str(), port_s.c_str(), &hints, &res);
    if (grc != 0 || res == nullptr) {
        return fail(fmt("解析监听地址失败 %s: %d", name.empty() ? "*" : name.c_str(), grc));
    }

    socket_t out = kBadSock;
    std::string last;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        SOCKET s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        // 不加 SO_REUSEADDR：Windows 上它允许两个进程同时绑同一端口（SO_REUSEADDR
        // 语义与 Unix 不同），那就等于放弃端口冲突检测，服务会「看起来起来了」但收不到包。
        if (::bind(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == SOCKET_ERROR) {
            last = fmt("bind %s:%d 失败: %d", name.c_str(), port, WSAGetLastError());
            closesocket(s);
            continue;
        }
        if (::listen(s, SOMAXCONN) == SOCKET_ERROR) {
            last = fmt("listen %s:%d 失败: %d", name.c_str(), port, WSAGetLastError());
            closesocket(s);
            continue;
        }
        out = wrap(s);
        break;
    }
    freeaddrinfo(res);

    if (out == kBadSock) return fail(last.empty() ? "监听失败" : last);
    if (err) err->clear();
    return out;
}

socket_t accept_one(socket_t ls, std::string* peer) {
    if (ls == kBadSock) return kBadSock;
    sockaddr_storage ss{};
    int len = static_cast<int>(sizeof ss);
    SOCKET s = ::accept(raw(ls), reinterpret_cast<sockaddr*>(&ss), &len);
    if (s == INVALID_SOCKET) return kBadSock;         // 调用方按 stop 判断是否继续
    if (peer) *peer = addr_text(ss);
    return wrap(s);
}

}  // namespace svpn
