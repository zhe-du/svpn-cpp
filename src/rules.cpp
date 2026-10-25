// 分流规则：域名后缀 / IP 段 / 三档策略。
// 行为基准：svpn/rules.py（_norm、is_ip、match_suffix、in_cidrs、Router.decide）。
// 不做 DNS 预解析：只看字面量，尽量把解析交给远端节点。
#include "svpn.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace svpn {
namespace {

// ---------------------------------------------------------------- IP 字面量解析
// 不用 InetPton：规则判断不该依赖 Winsock 是否已 net_init。
bool parse_v4(const std::string& s, std::array<unsigned char, 4>& out) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = s.find('.', start);
        if (dot == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    if (parts.size() != 4) return false;
    for (std::size_t k = 0; k < 4; ++k) {
        const std::string& piece = parts[k];
        if (piece.empty() || piece.size() > 3) return false;
        if (piece.size() > 1 && piece[0] == '0') return false;   // Python 3.9+ 拒前导零
        int val = 0;
        for (char ch : piece) {
            if (ch < '0' || ch > '9') return false;
            val = val * 10 + (ch - '0');
        }
        if (val > 255) return false;
        out[k] = static_cast<unsigned char>(val);
    }
    return true;
}

bool parse_hex_group(const std::string& s, std::uint16_t& out) {
    if (s.empty() || s.size() > 4) return false;
    unsigned val = 0;
    for (char ch : s) {
        unsigned d = 0;
        if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<unsigned>(ch - 'a') + 10;
        else if (ch >= 'A' && ch <= 'F') d = static_cast<unsigned>(ch - 'A') + 10;
        else return false;
        val = (val << 4) | d;
    }
    out = static_cast<std::uint16_t>(val);
    return true;
}

// 解析不含 "::" 的一段；末尾允许 IPv4 点分（占 2 组）。
bool parse_v6_groups(const std::string& s, std::vector<std::uint16_t>& out) {
    if (s.empty()) return true;
    std::size_t start = 0;
    while (true) {
        const std::size_t colon = s.find(':', start);
        const bool last = (colon == std::string::npos);
        const std::string token = s.substr(start, last ? std::string::npos : colon - start);
        if (token.empty()) return false;
        if (token.find('.') != std::string::npos) {
            // 点分只能落在整串末尾
            if (!last) return false;
            std::array<unsigned char, 4> v4{};
            if (!parse_v4(token, v4)) return false;
            out.push_back(static_cast<std::uint16_t>((static_cast<unsigned>(v4[0]) << 8) | v4[1]));
            out.push_back(static_cast<std::uint16_t>((static_cast<unsigned>(v4[2]) << 8) | v4[3]));
        } else {
            std::uint16_t g = 0;
            if (!parse_hex_group(token, g)) return false;
            out.push_back(g);
        }
        if (last) break;
        start = colon + 1;
    }
    return true;
}

bool parse_v6(const std::string& s, std::array<unsigned char, 16>& out) {
    std::vector<std::uint16_t> groups;
    const std::size_t dc = s.find("::");
    if (dc != std::string::npos) {
        if (s.find("::", dc + 1) != std::string::npos) return false;   // 只允许一个 "::"
        std::vector<std::uint16_t> left, right;
        if (!parse_v6_groups(s.substr(0, dc), left)) return false;
        if (!parse_v6_groups(s.substr(dc + 2), right)) return false;
        if (left.size() + right.size() > 7) return false;               // "::" 至少吃掉一组
        groups = left;
        groups.resize(8 - right.size(), 0);
        groups.insert(groups.end(), right.begin(), right.end());
    } else {
        if (!parse_v6_groups(s, groups)) return false;
        if (groups.size() != 8) return false;
    }
    for (std::size_t k = 0; k < 8; ++k) {
        out[k * 2] = static_cast<unsigned char>(groups[k] >> 8);
        out[k * 2 + 1] = static_cast<unsigned char>(groups[k] & 0xFF);
    }
    return true;
}

int parse_prefix(const std::string& text) {
    if (text.empty() || text.size() > 3) return -1;
    int val = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return -1;
        val = val * 10 + (ch - '0');
    }
    return val;
}

// :ffff:a.b.c.d 形式（IPv4-mapped）取出内嵌 v4
bool v4_mapped(const std::array<unsigned char, 16>& a6, std::string& out4) {
    static const unsigned char kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(a6.data(), kPrefix, 12) != 0) return false;
    out4 = fmt("%u.%u.%u.%u", static_cast<unsigned>(a6[12]), static_cast<unsigned>(a6[13]),
               static_cast<unsigned>(a6[14]), static_cast<unsigned>(a6[15]));
    return true;
}

// ---------------------------------------------------------------- 本地/内网段
// 逐条对齐 Python 3.13 ipaddress 常量表（IPv4Address._constants / IPv6Address._constants）：
// decide() 只认 is_private / is_loopback / is_link_local / is_multicast / is_reserved，
// 命中随便一个就直连，避免把本地服务、Docker(172.17)/WSL(172.20) 送进上游。
// 注意：100.64.0.0/10(CGNAT) 在 3.13 里是 _public_network，四个标志全 False → 这里也刻意不列。
const char* const kV4Private[] = {
    "0.0.0.0/8",      "10.0.0.0/8",      "127.0.0.0/8",     "169.254.0.0/16",
    "172.16.0.0/12",  "192.0.0.0/24",    "192.0.0.170/31",  "192.0.2.0/24",
    "192.168.0.0/16", "198.18.0.0/15",   "198.51.100.0/24", "203.0.113.0/24",
    "240.0.0.0/4",    "255.255.255.255/32",
};
const char* const kV4PrivateSkip[] = {"192.0.0.9/32", "192.0.0.10/32"};
const char* const kV4Loopback[] = {"127.0.0.0/8"};
const char* const kV4LinkLocal[] = {"169.254.0.0/16"};
const char* const kV4Multicast[] = {"224.0.0.0/4"};

const char* const kV6Private[] = {
    "::1/128",        "::/128",         "::ffff:0:0/96", "64:ff9b:1::/48",
    "100::/64",       "2001::/23",      "2001:db8::/32", "2002::/16",
    "3fff::/20",      "fc00::/7",       "fe80::/10",
};
const char* const kV6PrivateSkip[] = {
    "2001:1::1/128", "2001:1::2/128", "2001:3::/32",
    "2001:4:112::/48", "2001:20::/28", "2001:30::/28",
};
const char* const kV6Reserved[] = {
    "::/8",      "100::/8",  "200::/7",  "400::/6",  "800::/5",
    "1000::/4",  "4000::/3", "6000::/3", "8000::/3", "a000::/3",
    "c000::/3",  "e000::/4", "f000::/5", "f800::/6", "fe00::/9",
};
const char* const kV6Loopback[] = {"::1/128"};
const char* const kV6LinkLocal[] = {"fe80::/10"};
const char* const kV6Multicast[] = {"ff00::/8"};

bool match_any(const std::string& h, const std::vector<std::string>& suffixes) {
    for (const std::string& s : suffixes) {
        if (match_suffix(h, s)) return true;
    }
    return false;
}

bool match_any(const std::string& h, const char* const* table, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (match_suffix(h, table[i])) return true;
    }
    return false;
}

bool in_any_cidr(const std::string& h, const char* const* table, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (in_cidr(h, table[i])) return true;
    }
    return false;
}

bool in_any_cidr(const std::string& h, const std::vector<std::string>& cidrs) {
    for (const std::string& c : cidrs) {
        if (in_cidr(h, c)) return true;
    }
    return false;
}

template <std::size_t N>
bool in_table(const std::string& h, const char* const (&table)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (in_cidr(h, table[i])) return true;
    }
    return false;
}

// 语义 = Python 的 ip.is_private or is_loopback or is_link_local or is_multicast or is_reserved
bool is_local_only(const std::string& h) {
    std::array<unsigned char, 4> v4{};
    if (parse_v4(h, v4)) {
        if (in_table(h, kV4Private) && !in_table(h, kV4PrivateSkip)) return true;
        return in_table(h, kV4Loopback) || in_table(h, kV4LinkLocal) ||
               in_table(h, kV4Multicast);
    }
    std::array<unsigned char, 16> v6{};
    if (!parse_v6(h, v6)) return false;
    std::string mapped;
    if (v4_mapped(v6, mapped)) return is_local_only(mapped);   // ::ffff:a.b.c.d 交给 v4 规则
    if (in_table(h, kV6Private) && !in_table(h, kV6PrivateSkip)) return true;
    return in_table(h, kV6Loopback) || in_table(h, kV6LinkLocal) ||
           in_table(h, kV6Multicast) || in_table(h, kV6Reserved);
}

}  // namespace

// ================================================================ 基础规则
std::string normalize_host(std::string h) {
    h = trim(h);
    h = lower(h);
    while (!h.empty() && h.back() == '.') h.pop_back();
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    return h;
}

bool match_suffix(const std::string& host, const std::string& suffix) {
    const std::string h = normalize_host(host);
    if (h.empty()) return false;
    std::string s = lower(trim(suffix));
    std::size_t lead = 0;
    while (lead < s.size() && s[lead] == '.') ++lead;   // Python: lstrip(".")
    s = s.substr(lead);
    if (s.empty()) return false;
    if (h == s) return true;
    // 必须是 ".suffix" 结尾 —— 简单 ends_with 会把 notsteampowered.com 误判成 Steam
    if (h.size() <= s.size()) return false;
    return h[h.size() - s.size() - 1] == '.' && ends_with(h, s);
}

bool is_ip(const std::string& s) {
    const std::string h = normalize_host(s);
    if (h.empty()) return false;
    std::array<unsigned char, 4> v4{};
    if (parse_v4(h, v4)) return true;
    if (h.find(':') == std::string::npos) return false;
    std::array<unsigned char, 16> v6{};
    return parse_v6(h, v6);
}

bool in_cidr(const std::string& ip, const std::string& cidr) {
    const std::string addr = normalize_host(ip);
    const std::string net = trim(cidr);
    if (addr.empty() || net.empty()) return false;

    std::string netaddr = net;
    int prefix = -1;
    const std::size_t slash = net.find('/');
    if (slash != std::string::npos) {
        netaddr = trim(net.substr(0, slash));
        prefix = parse_prefix(trim(net.substr(slash + 1)));
        if (prefix < 0) return false;
    }

    std::array<unsigned char, 4> a4{}, n4{};
    std::array<unsigned char, 16> a6{}, n6{};
    const bool ip4 = parse_v4(addr, a4);
    const bool ip6 = !ip4 && addr.find(':') != std::string::npos && parse_v6(addr, a6);
    if (!ip4 && !ip6) return false;

    if (ip4) {
        if (!parse_v4(netaddr, n4)) return false;   // 族不同（v4 地址对 v6 段）→ 不匹配
        if (prefix < 0) prefix = 32;                // 不带 "/" 按单主机
        if (prefix > 32) return false;
        const int full = prefix / 8;
        const int rem = prefix % 8;
        for (int k = 0; k < full; ++k) {
            if (a4[k] != n4[k]) return false;
        }
        if (rem > 0) {
            const unsigned char mask = static_cast<unsigned char>(0xFFU << (8 - rem));
            if ((a4[full] & mask) != (n4[full] & mask)) return false;
        }
        return true;
    }

    if (!parse_v6(netaddr, n6)) return false;
    if (prefix < 0) prefix = 128;
    if (prefix > 128) return false;
    const int full = prefix / 8;
    const int rem = prefix % 8;
    for (int k = 0; k < full; ++k) {
        if (a6[k] != n6[k]) return false;
    }
    if (rem > 0) {
        const unsigned char mask = static_cast<unsigned char>(0xFFU << (8 - rem));
        if ((a6[full] & mask) != (n6[full] & mask)) return false;
    }
    return true;
}

// ================================================================ Router
Router::Router(const Config& c) {
    mode = c.mode;
    direct_hosts.reserve(c.direct_hosts.size());
    for (const std::string& h : c.direct_hosts) direct_hosts.push_back(normalize_host(h));
    proxy_hosts.reserve(c.proxy_hosts.size());
    for (const std::string& h : c.proxy_hosts) proxy_hosts.push_back(normalize_host(h));
    proxy_suffixes = c.proxy_suffixes;
    direct_suffixes = c.direct_suffixes;
    // proxy_cidrs 没有配置入口：留空即「用内置 kSteamCidrs」
}

std::string Router::decide(const std::string& host) const {
    const std::string h = normalize_host(host);

    // 1) 显式覆盖优先级最高
    for (const std::string& d : direct_hosts) {
        if (h == d) return "direct";
    }
    // 2) 强制代理名单
    for (const std::string& p : proxy_hosts) {
        if (h == p) return "upstream";
    }
    // 3) 回环/本地页面永远直连（steamloopback.host 是 Steam CEF 的本地源）
    if (h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "steamloopback.host" ||
        ends_with(h, ".localhost")) {
        return "direct";
    }
    // 4) 字面量 IP：Valve 段走上游，内网/回环/链路本地/多播/保留直连
    if (is_ip(h)) {
        if (in_any_cidr(h, kSteamCidrs, kSteamCidrs_count)) return "upstream";
        if (is_local_only(h)) return "direct";
    }
    // 5) 后缀 / 网段名单（配置为空则用内置 Steam 名单）
    if (proxy_suffixes.empty()) {
        if (match_any(h, kSteamSuffixes, kSteamSuffixes_count)) return "upstream";
    } else if (match_any(h, proxy_suffixes)) {
        return "upstream";
    }
    if (proxy_cidrs.empty()) {
        if (in_any_cidr(h, kSteamCidrs, kSteamCidrs_count)) return "upstream";
    } else if (in_any_cidr(h, proxy_cidrs)) {
        return "upstream";
    }
    // 6) mode 兜底
    if (mode == "all") return "upstream";
    if (mode == "blacklist") {
        if (direct_suffixes.empty()) {
            return match_any(h, kCnDirectSuffixes, kCnDirectSuffixes_count) ? "direct" : "upstream";
        }
        return match_any(h, direct_suffixes) ? "direct" : "upstream";
    }
    // 7) whitelist 兜底
    return "direct";
}

}  // namespace svpn
