// svpn (C++) —— PAC 生成。输出与 Python `svpn/pac.py::build_pac` **逐字节一致**。
//
// 注意：PAC 里必须是 **数组字面量**（`[ ... ]`），对象字面量在 PAC 环境里是语法错误，
// 浏览器会整份 PAC 加载失败 —— 表现是「设了 PAC 之后完全没网」。历史事故，别再改。
#include "svpn.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace svpn {
namespace {

// 等价 json.dumps(s, ensure_ascii=False)：只转义 " \ 和 <0x20 的控制字符，
// 双引号/换行/反斜杠进数组时必须转义，否则整份 PAC 变非法 JS。
std::string json_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += ch;   // UTF-8 原样输出（ensure_ascii=False）
                }
        }
    }
    out += '"';
    return out;
}

// sorted(set(...))：Python 按码点排序；UTF-8 的字节序与码点序一致，直接按字节排即可。
std::vector<std::string> sorted_unique(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

std::string js_list(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ",\n";
        out += "  ";
        out += json_quote(items[i]);
    }
    return out;
}

std::vector<std::string> copy_table(const char* const* table, std::size_t n) {
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (table[i] != nullptr) out.emplace_back(table[i]);
    }
    return out;
}

}  // namespace

std::string build_pac(const std::string& host, int port,
                      const std::vector<std::string>& proxy_suffixes,
                      const std::vector<std::string>& direct_suffixes,
                      bool local_net, const std::string& mode) {
    std::vector<std::string> prox =
        proxy_suffixes.empty() ? copy_table(kSteamSuffixes, kSteamSuffixes_count) : proxy_suffixes;
    prox = sorted_unique(std::move(prox));

    // blacklist 模式必须真带直连名单：空表会让「国内直连」变成一句空话
    // （Router 侧是 `direct_suffixes or CN_DIRECT_SUFFIXES`，这里必须同口径）
    std::vector<std::string> direct = direct_suffixes;
    if (direct.empty() && mode == "blacklist") {
        direct = copy_table(kCnDirectSuffixes, kCnDirectSuffixes_count);
    }
    direct = sorted_unique(std::move(direct));

    const std::string pjs = js_list(prox);
    const std::string djs = js_list(direct);

    std::string out;
    out += "// svpn 自动生成，重跑 `svpn pac` 覆盖\n";
    out += "var SOCK = \"SOCKS5 ";
    out += host;
    out += ':';
    out += std::to_string(port);
    out += "\";\n";
    out += "var PROXY = [\n";
    out += pjs;
    out += "\n];\n";
    out += "var DIRECT = [\n";
    out += djs;
    out += "\n];\n";
    out += "\n";
    out += "function _hit(host, tbl) {\n";
    out += "  for (var i = 0; i < tbl.length; i++) {\n";
    out += "    var s = tbl[i];\n";
    out += "    if (host === s || host.substring(host.length - s.length - 1) === \".\" + s) return true;\n";
    out += "  }\n";
    out += "  return false;\n";
    out += "}\n";
    out += "\n";
    out += "function FindProxyForURL(url, host) {\n";
    out += "  host = host.toLowerCase().replace(/\\.$/, \"\");\n";
    out += "  if (host === \"localhost\" || host === \"127.0.0.1\" || host === \"::1\") return \"DIRECT\";\n";
    if (local_net) {
        out += "  if (isPlainHostName(host) || shExpMatch(host, '*.local')) return 'DIRECT';\n";
    } else {
        // Python 里这一行是 `  {"" if not local_net}` → 留下两个空格的空行，逐字节照抄
        out += "  \n";
    }
    out += "  if (_hit(host, PROXY)) return SOCK + \"; DIRECT\";\n";
    out += "  if (_hit(host, DIRECT)) return \"DIRECT\";\n";
    if (mode == "all") {
        out += "  return SOCK + \"; DIRECT\";\n";
    } else if (mode == "blacklist") {
        out += "  if (_hit(host, DIRECT)) return \"DIRECT\";\n";
        out += "  return SOCK + \"; DIRECT\";\n";
    } else {
        out += "  return \"DIRECT\";\n";
    }
    out += "}\n";
    return out;
}

}  // namespace svpn
