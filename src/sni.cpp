// SNI 透明转发：监听 127.0.0.1:sni_port，从每个连接的 TLS ClientHello 明文里读出
// SNI，再按 SNI 决定真实目标，把「已读到的字节 + 后续字节」原样拼接转发。
//
// 行为基准：Python 版 svpn/forward.py 的 SniForwarder（_handle / _allowed / extract_sni）。
// 使用场景：hosts 把 CDN 域名指到 127.0.0.1（Steam 下载/更新是裸 TCP，HTTP 代理管不到），
// 到这里只能靠 ClientHello 里的 SNI 还原出真实域名。
//
// 三条纪律（Python 版都栽过）：
//   1. 白名单为空 => 全部拒绝。空表放行等于在本机开了一个通用 443 出口。
//   2. 上游 socket 必须 RAII 关闭。Python 版成功路径漏关 sw，每条连接泄一个 fd。
//   3. 拒绝时不回 TLS alert（我们不是 TLS 服务器），直接关 socket 让客户端看到 RST。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winsock2.h>

#include "svpn.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace svpn {
namespace {

// ---------------------------------------------------------------- 常量
constexpr std::size_t kMaxHead = 16 * 1024;   // ClientHello 上限，读满还没解析出来就断
constexpr std::size_t kReadChunk = 4096;      // 与 Python 版 reader.read(4096) 一致
constexpr int kHeadReadMs = 10000;            // 单次读 ClientHello 的超时（Python wait_for 10s）
constexpr int kStopPollMs = 100;              // stop 原子量检查间隔
constexpr int kDefaultTargetPort = 443;

// ---------------------------------------------------------------- 运行期设置
// 由 serve_sni_to 一次性算好，所有连接线程只读共享（线程生命周期不超过 serve_*）。
struct Settings {
    std::vector<std::string> allow;                 // 归一后的白名单后缀
    std::map<std::string, std::string> ip_map;      // domain -> 最优边缘 IP（键已归一）
    int target_port{kDefaultTargetPort};
    int io_ms{kDefaultTimeoutMs};
};

// ---------------------------------------------------------------- 在途 socket 登记
// 退出时只 shutdown 不 close：跨线程 close 正在 select/recv 的 socket 是未定义行为，
// 而 shutdown(SD_BOTH) 能让阻塞在 recv/select 里的线程立刻返回，线程才能 join 掉。
class LiveSet {
  public:
    void add(socket_t s) {
        if (s == kBadSock) return;
        std::lock_guard<std::mutex> lk(mu_);
        set_.insert(s);
    }
    void remove(socket_t s) {
        std::lock_guard<std::mutex> lk(mu_);
        set_.erase(s);
    }
    void shutdown_all() {
        std::lock_guard<std::mutex> lk(mu_);
        for (socket_t s : set_) ::shutdown(static_cast<SOCKET>(s), SD_BOTH);
    }

  private:
    std::mutex mu_;
    std::set<socket_t> set_;
};

// 函数出口一律关闭：漏一个上游 socket 就是每条连接泄一个句柄
struct SockGuard {
    socket_t v{kBadSock};
    ~SockGuard() {
        if (v != kBadSock) close_socket(v);
    }
    SockGuard() = default;
    SockGuard(const SockGuard&) = delete;
    SockGuard& operator=(const SockGuard&) = delete;
};

// ---------------------------------------------------------------- 白名单
// host 为已归一化域名；allow 为空一律拒绝（见文件头纪律 1）。
bool allowed(const std::string& host, const std::vector<std::string>& allow) {
    if (host.empty() || allow.empty()) return false;
    for (const std::string& suf : allow) {
        if (match_suffix(host, suf)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 读 ClientHello
// 边收边判：读够一个完整 TLS 记录就停，不必死等 16KB。
// 非 0x16 首字节立刻放弃（Python 版同样逻辑：别为一条非 TLS 连接等满 10 秒）。
std::string read_head(socket_t c) {
    std::string head;
    head.reserve(kReadChunk * 2);
    while (head.size() < kMaxHead) {
        char buf[kReadChunk];
        const std::size_t want = std::min(kReadChunk, kMaxHead - head.size());
        const int got = recv_some(c, buf, want, kHeadReadMs);
        if (got <= 0) break;                       // -1 超时/出错，0 EOF
        head.append(buf, static_cast<std::size_t>(got));
        if (head.size() >= 5) {
            if (static_cast<unsigned char>(head[0]) != 0x16) break;
            const std::size_t need =
                5 + ((static_cast<std::size_t>(static_cast<unsigned char>(head[3])) << 8) |
                     static_cast<std::size_t>(static_cast<unsigned char>(head[4])));
            if (head.size() >= need) break;
        }
    }
    return head;
}

// ---------------------------------------------------------------- 单连接
void handle_conn(socket_t client, const Settings& s, Stats& st, LiveSet& live,
                 const std::atomic<bool>& stop) {
    st.conns.fetch_add(1, std::memory_order_relaxed);

    SockGuard cli;
    cli.v = client;
    live.add(cli.v);
    struct Unreg {
        LiveSet& live;
        socket_t s;
        ~Unreg() { live.remove(s); }
    } unreg{live, cli.v};

    const std::string head = read_head(cli.v);
    std::string sni;
    const bool parsed = !head.empty() &&
                        parse_client_hello_sni(reinterpret_cast<const unsigned char*>(head.data()),
                                               head.size(), &sni);
    if (!parsed) {
        st.errors.fetch_add(1, std::memory_order_relaxed);
        log_write("WARN", fmt("[sni] 无 SNI，拒绝（首包 %zu 字节）", head.size()));
        return;
    }

    // SNI 里可能出现大写 / 尾点 / 方括号形式，先归一
    const std::string host = normalize_host(sni);
    if (!allowed(host, s.allow)) {
        st.errors.fetch_add(1, std::memory_order_relaxed);
        log_write("WARN", fmt("[sni] SNI 拒绝: %s", (host.empty() ? sni : host).c_str()));
        return;                                    // 直接关，让客户端看到 RST，不回 alert
    }
    if (stop.load(std::memory_order_acquire)) return;

    // 选目标：ip_map 命中就直连最优边缘 IP，否则系统 DNS 取首个可用 IP。
    // Python 版在 hint 拨号失败时会退回 dial(up, sni)；这里对应「退回解析结果」。
    socket_t ups = kBadSock;
    auto hint = s.ip_map.find(host);
    if (hint != s.ip_map.end() && !hint->second.empty()) {
        ups = tcp_connect_ip(hint->second, s.target_port, s.io_ms);
        if (ups != kBadSock) log_write("DEBUG", fmt("[sni] %s -> 边缘 %s", host.c_str(), hint->second.c_str()));
    }
    if (ups == kBadSock) {
        const std::vector<std::string> ips = resolve_system(host, s.io_ms);
        for (const std::string& ip : ips) {
            if (stop.load(std::memory_order_acquire)) break;
            ups = tcp_connect_ip(ip, s.target_port, s.io_ms);
            if (ups != kBadSock) break;
        }
    }
    if (ups == kBadSock) {
        st.errors.fetch_add(1, std::memory_order_relaxed);
        log_write("WARN", fmt("[sni] 拨号失败: %s:%d", host.c_str(), s.target_port));
        return;
    }

    SockGuard up;
    up.v = ups;
    live.add(up.v);

    // 必须先把已读到的 ClientHello 原样送给目标，再进入双向中继
    if (!send_all(up.v, head.data(), head.size(), s.io_ms)) {
        st.errors.fetch_add(1, std::memory_order_relaxed);
        log_write("WARN", fmt("[sni] 回注 ClientHello 失败: %s", host.c_str()));
        return;
    }
    st.up_bytes.fetch_add(static_cast<std::uint64_t>(head.size()), std::memory_order_relaxed);
    st.hit(host);

    // idle_ms=0：不限空闲（Steam 下载是长连接，Python 版也没有空闲超时）
    std::uint64_t up_bytes = 0;
    std::uint64_t down_bytes = 0;
    const int rc = relay_stream(cli.v, up.v, 0, &up_bytes, &down_bytes);
    st.up_bytes.fetch_add(up_bytes, std::memory_order_relaxed);
    st.down_bytes.fetch_add(down_bytes, std::memory_order_relaxed);
    if (rc != 0) {
        // 中继阶段的中断（含退出时被 shutdown）与 Python 版一样不计 errors：
        // 客户端取消下载是常态，记成错误会把 errors 刷成噪声。
        log_write("DEBUG", fmt("[sni] %s 中继结束 rc=%d up=%llu down=%llu", host.c_str(), rc,
                               static_cast<unsigned long long>(up_bytes),
                               static_cast<unsigned long long>(down_bytes)));
    }
}

}  // namespace

// ============================================================ 服务入口
// target_port 默认 443；暴露成独立符号只为让测试能指向高位端口的假上游
// （cfg.cdn.sni_port 是监听端口，配置里没有「目标端口」这一项）。
int serve_sni_to(const Config& cfg, Stats& st, std::atomic<bool>& stop, int target_port) {
    Settings s;
    s.target_port = target_port > 0 ? target_port : kDefaultTargetPort;
    s.io_ms = cfg.connect_timeout > 0 ? cfg.connect_timeout * 1000 : kDefaultTimeoutMs;

    for (const std::string& raw : cfg.cdn.hosts) {
        const std::string h = normalize_host(raw);
        if (!h.empty()) s.allow.push_back(h);
    }
    for (const auto& kv : cfg.ip_map) {
        const std::string k = normalize_host(kv.first);
        if (k.empty() || kv.second.empty()) continue;
        s.ip_map[k] = kv.second;
    }

    if (s.allow.empty()) {
        log_write("WARN", "[sni] 白名单为空，全部拒绝（检查 cdn.hosts 配置）");
    }

    std::string err;
    const socket_t ls = listen_on("127.0.0.1", cfg.cdn.sni_port, &err);
    if (ls == kBadSock) {
        log_write("WARN", fmt("[sni] 监听 127.0.0.1:%d 失败: %s（443 可能被 IIS/VMware 等占用）",
                              cfg.cdn.sni_port, err.c_str()));
        return 2;
    }
    log_write("INFO", fmt("[sni] 127.0.0.1:%d 按 SNI 转发（白名单 %zu 条）", cfg.cdn.sni_port,
                          s.allow.size()));

    LiveSet live;
    std::vector<std::thread> workers;
    std::vector<std::shared_ptr<std::atomic<bool>>> done;   // 与 workers 同下标

    // 只由 accept 线程调用：回收已退出线程，向量不会无限长
    auto reap = [&workers, &done](bool all) {
        std::size_t i = 0;
        while (i < workers.size()) {
            if (all || done[i]->load(std::memory_order_acquire)) {
                if (workers[i].joinable()) workers[i].join();
                workers.erase(workers.begin() + static_cast<std::ptrdiff_t>(i));
                done.erase(done.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    };

    int rc = 0;
    while (!stop.load(std::memory_order_acquire)) {
        // 不直接阻塞 accept：靠 select 的 100ms 超时轮询 stop，才能及时退出
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(static_cast<SOCKET>(ls), &rf);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = static_cast<long>(kStopPollMs) * 1000;
        const int n = ::select(0, &rf, nullptr, nullptr, &tv);
        if (n == SOCKET_ERROR) {
            log_write("WARN", fmt("[sni] select 失败: %d", WSAGetLastError()));
            rc = 3;
            break;
        }
        if (n == 0) {
            reap(false);
            continue;
        }
        const socket_t c = accept_one(ls, nullptr);
        if (c == kBadSock) {
            reap(false);
            continue;                              // 对端提前消失之类的瞬态错误
        }
        done.push_back(std::make_shared<std::atomic<bool>>(false));
        workers.emplace_back([&s, &st, &live, &stop, c, flag = done.back()] {
            handle_conn(c, s, st, live, stop);
            flag->store(true, std::memory_order_release);
        });
        reap(false);
    }

    close_socket(ls);
    // 退出：先让在途连接的中继立刻返回，再 join，避免长连接把进程钉死
    live.shutdown_all();
    reap(true);
    return rc;
}

int serve_sni(const Config& cfg, Stats& st, std::atomic<bool>& stop) {
    return serve_sni_to(cfg, st, stop, kDefaultTargetPort);
}

}  // namespace svpn
