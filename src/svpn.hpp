// svpn (C++) —— 共享契约。
// Windows only，零外部依赖：Winsock2 + SChannel + WinHTTP + Win32。
// 约定：源码 UTF-8；对外字符串一律 UTF-8；socket 用 socket_t(std::uintptr_t) 抹平 SOCKET 类型。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace svpn {

// ============================================================ 基础类型
using socket_t = std::uintptr_t;
constexpr socket_t kBadSock = static_cast<socket_t>(~0ull);

// 0.8 秒级超时统一用 ms；-1 表示无限等
constexpr int kDefaultTimeoutMs = 12000;

// ---------------------------------------------------------------- 节点
struct Node {
    std::string kind{"socks5"};   // socks5 | http | direct
    std::string host;
    int port{0};
    std::string user, pass;

    bool usable() const { return port > 0 && !host.empty(); }
    std::string url() const;
};

// ---------------------------------------------------------------- 配置
struct UpstreamCfg {
    std::string kind{"socks5"};
    std::string host{"NODE_HOST"};   // 占位符：出现它就等于「没配节点」
    int port{1080};
    std::string user, pass;
    bool configured() const;
};

struct AuthCfg { std::string user, pass; };

struct BrowserCfg {
    bool apply_system_proxy{true};
    bool use_pac{false};
    std::string bypass;
};

struct CdnCfg {
    bool enabled{false};
    double refresh_hours{6};
    int sni_port{443};
    std::vector<std::string> hosts;
};

struct Config {
    std::string listen_host{"127.0.0.1"};
    int listen_port{7899};
    int http_port{7900};        // 0 = 不启动 HTTP 入口
    std::string mode{"whitelist"};
    int connect_timeout{12};
    bool auto_node{true};
    UpstreamCfg upstream;
    AuthCfg auth;
    BrowserCfg browser;
    CdnCfg cdn;
    std::vector<std::string> proxy_hosts, direct_hosts;
    // 可选高级键（不在模板里，写了才生效）
    std::vector<std::string> proxy_suffixes, direct_suffixes;
    // 运行期派生：domain -> ip（CDN 选边 / 探测结果），不上盘
    std::map<std::string, std::string> ip_map;
};

struct Paths {
    std::filesystem::path root;       // exe 所在目录
    std::filesystem::path config;     // root/config.json
    std::filesystem::path runtime;    // root/runtime
    std::filesystem::path cdn_cache;  // runtime/cdn-cache.json
    std::filesystem::path probe;      // runtime/probe.json
    std::filesystem::path log;        // runtime/svpn.log
    std::filesystem::path pid;        // runtime/svpn.pid
    std::filesystem::path stats;      // runtime/stats.json
    std::filesystem::path hosts_backup;
    std::filesystem::path wininet_backup;
    static Paths resolve();
    void ensure_runtime() const;
};

// ---------------------------------------------------------------- 统计
struct Stats {
    std::atomic<std::uint64_t> conns{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> up_bytes{0};
    std::atomic<std::uint64_t> down_bytes{0};
    std::atomic<std::uint64_t> via_upstream{0};
    std::atomic<std::uint64_t> via_direct{0};
    std::atomic<std::int64_t> started_ms{0};

    mutable std::mutex mu;
    std::map<std::string, std::uint64_t> top_hosts;

    void hit(const std::string& host);
    void dump(const Paths& p) const;
    std::string summary_line() const;
};

// ============================================================ json
enum class JType { Null, Bool, Num, Str, Arr, Obj };

struct Json {
    JType type{JType::Null};
    bool b{false};
    double num{0};
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    static Json parse(const std::string& text, std::string* err = nullptr);
    std::string dump(int indent = -1) const;

    bool is_null() const { return type == JType::Null; }
    bool has(const std::string& k) const;
    const Json* get(const std::string& k) const;
    bool as_bool(bool def) const;
    double as_num(double def) const;
    std::string as_str(const std::string& def = "") const;
    const std::vector<Json>* as_arr() const;

    static Json make_obj();
    static Json make_arr();
    static Json make_str(const std::string& s);
    static Json make_num(double v);
    static Json make_bool(bool v);
    Json& set(const std::string& k, Json v);   // 仅 Obj
    Json& push(Json v);                        // 仅 Arr
};

// ============================================================ util
std::string trim(const std::string& s);
std::string lower(std::string s);
std::vector<std::string> split(const std::string& s, char sep, bool keep_empty = false);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::string now_hms();
std::int64_t now_ms();
std::string fmt(const char* f, ...);
void console_utf8();                    // 控制台输入输出改 UTF-8
void log_init(const Paths& p, bool verbose);
void log_write(const char* level, const std::string& msg);

// ============================================================ rules_data（自动生成）
extern const char* const kSteamSuffixes[];
extern const std::size_t kSteamSuffixes_count;
extern const char* const kCnDirectSuffixes[];
extern const std::size_t kCnDirectSuffixes_count;
extern const char* const kSteamCidrs[];
extern const std::size_t kSteamCidrs_count;
extern const char* const kCdnDomains[];
extern const std::size_t kCdnDomains_count;
extern const char* const kDefaultBypass;

// ============================================================ rules
std::string normalize_host(std::string h);
bool match_suffix(const std::string& host, const std::string& suffix);
bool is_ip(const std::string& s);
bool in_cidr(const std::string& ip, const std::string& cidr);

struct Router {
    std::vector<std::string> direct_hosts, proxy_hosts;
    std::vector<std::string> proxy_suffixes, direct_suffixes, proxy_cidrs;
    std::string mode{"whitelist"};

    explicit Router(const Config& c);
    std::string decide(const std::string& host) const;   // "direct" | "upstream"
};

// ============================================================ config
Config default_config();
Config merge_config(const Json* user);
Config load_config(const Paths& p, std::string* err = nullptr);
bool save_config(const Paths& p, const Config& cfg);
Json config_to_json(const Config& cfg);
std::string config_note();   // config.example.json 里的 _note 文案
// 上一次 merge_config 丢掉的不认识的非 `_` 键（供 doctor 报告拼写错误）
const std::vector<std::string>& unknown_config_keys();

// ============================================================ net
bool net_init();
void net_shutdown();
socket_t tcp_connect_ip(const std::string& ip, int port, int timeout_ms);
socket_t tcp_connect(const std::string& host, int port, int timeout_ms, std::string* err = nullptr);
std::vector<std::string> resolve_system(const std::string& host, int timeout_ms);
bool set_io_timeout(socket_t s, int timeout_ms);
void close_socket(socket_t s);
bool send_all(socket_t s, const void* data, std::size_t n, int timeout_ms);
bool recv_exact(socket_t s, void* buf, std::size_t n, int timeout_ms);
int recv_some(socket_t s, void* buf, std::size_t cap, int timeout_ms);   // -1 err / 0 eof
int relay_stream(socket_t a, socket_t b, int idle_ms, std::uint64_t* up, std::uint64_t* down);
std::string peer_ip(socket_t s);
socket_t listen_on(const std::string& host, int port, std::string* err);
socket_t accept_one(socket_t ls, std::string* peer = nullptr);

// ============================================================ tls（SChannel）
struct TlsConn {
    socket_t sock{kBadSock};
    void* ctx{nullptr};       // CtxtHandle*（堆上，内部管理）
    bool ok{false};
    int send(const void* data, std::size_t n);
    int recv(void* buf, std::size_t cap);   // -1 err / 0 eof
    void close();
};

struct TlsProbe {
    bool ok{false};
    std::string cert_cn, protocol, err;
    double ms{0};
};

// 在已连通的 socket 上做 TLS 客户端握手（SNI=server_name）
TlsConn tls_connect(socket_t sock, const std::string& server_name, int timeout_ms, std::string* err = nullptr);
// 一次性探测：连 ip:443 → 握手 → 取证书 CN；失败时自己关 socket
TlsProbe tls_probe(const std::string& ip, const std::string& sni, int timeout_ms);
// 从 ClientHello 明文中解析 SNI（bytes 不足返回 false）
bool parse_client_hello_sni(const unsigned char* buf, std::size_t n, std::string* sni);

// ============================================================ upstream
socket_t dial_via(const Node& up, const std::string& host, int port, int timeout_ms, std::string* err = nullptr);
socket_t dial_direct(const std::string& host, int port, int timeout_ms, std::string* err = nullptr);

// ============================================================ proxy（本地服务）
int serve_socks(const Config& cfg, Stats& st, std::atomic<bool>& stop);
int serve_http(const Config& cfg, Stats& st, std::atomic<bool>& stop);
// 供 _serve 用：同时起两个监听，任一失败返回非 0
int serve_all(const Config& cfg, Stats& st, std::atomic<bool>& stop);

// ============================================================ cdn
struct Edge {
    std::string ip;
    double ms{0}, mbps{0};
    int code{0};
    std::string err;
    double score() const;
};

struct DomainResult {
    std::string domain, source, best_ip;
    int candidates{0}, tested{0};
    double best_ms{0}, best_mbps{0};
    std::int64_t ts{0};
    std::vector<Edge> top;
};

using Cache = std::map<std::string, DomainResult>;

bool doh_query(const std::string& provider, const std::string& domain, int timeout_ms,
               std::vector<std::string>* ips, std::string* err);
DomainResult rank_domain(const std::string& domain, bool bench, int timeout_ms);
Cache refresh(const std::vector<std::string>& domains, bool bench, int timeout_ms);
Cache load_cache(const Paths& p, double max_age_s);
bool save_cache(const Paths& p, const Cache& c);
double cache_age_hours(const Paths& p);        // <0 = 没有缓存
std::map<std::string, std::string> best_ip_map(const Paths& p, double max_age_s);
// 把已缓存且未过期的 edge IP 并入 cfg.ip_map（供代理拨号用）
void apply_ip_map(const Paths& p, Config& cfg, double max_age_s);

// ============================================================ winsys
struct HostsResult {
    bool ok{false};
    int entries{0};
    std::string reason;
    std::vector<std::string> rejected;
};

HostsResult apply_hosts(const std::map<std::string, std::string>& ip_to_hosts, const Paths& p);
HostsResult clear_hosts(const Paths& p);
int hosts_managed_entries(const Paths& p);      // 托管块里的条目数
void flush_dns();

struct WininetState {
    bool ok{false};
    std::string reason;
    bool enable{false};
    std::string server, override_list, autoconfig;
};

WininetState read_wininet();
bool backup_wininet(const Paths& p);
WininetState apply_wininet(const std::string& server, const std::string& bypass);
WininetState restore_wininet(const Paths& p);

bool is_admin();
// 以管理员重启自身（args 为完整命令行尾部），成功返回 true
bool elevate(const std::string& args);

// ============================================================ pac
std::string build_pac(const std::string& host, int port,
                      const std::vector<std::string>& proxy_suffixes,
                      const std::vector<std::string>& direct_suffixes,
                      bool local_net, const std::string& mode);

// ============================================================ sni（透明转发）
int serve_sni(const Config& cfg, Stats& st, std::atomic<bool>& stop);

}  // namespace svpn
