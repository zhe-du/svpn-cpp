// 配置：默认值 / 深合并 / 逐键归一 / 读写。
// 行为基准：svpn/cli.py 的 DEFAULT_CONFIG、_as_number/_as_port/_as_bool/_as_text/_as_host_list、
// _normalize、merge_config、load_config、save_config。改动前先读 Python 版。
#include "svpn.hpp"

#ifndef NOMINMAX
#define NOMINMAX   // 别让 windows.h 的 min/max 宏污染 <algorithm>
#endif
#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace svpn {
namespace {

// ---------------------------------------------------------------- 键集合（契约：12 个顶层键）
const char* const kTopKeys[] = {
    "listen_host", "listen_port", "http_port",  "mode",     "connect_timeout", "auto_node",
    "upstream",    "auth",        "browser",    "cdn",      "proxy_hosts",     "direct_hosts",
};
const char* const kExtraKeys[] = {"proxy_suffixes", "direct_suffixes"};

constexpr std::size_t kTopCount = sizeof kTopKeys / sizeof kTopKeys[0];
constexpr std::size_t kExtraCount = sizeof kExtraKeys / sizeof kExtraKeys[0];

const std::vector<std::string> kNoHosts;

bool in_keys(const std::string& key, const char* const* keys, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (key == keys[i]) return true;
    }
    return false;
}
bool is_top_key(const std::string& k) { return in_keys(k, kTopKeys, kTopCount); }
bool is_extra_key(const std::string& k) { return in_keys(k, kExtraKeys, kExtraCount); }

// ---------------------------------------------------------------- Python _as_* 的等价物
// _as_number：None / bool / 乱码 / NaN / ±inf → default；"12abc" 这种半截数字也回落。
double as_number(const Json* v, double def) {
    if (!v) return def;
    if (v->type == JType::Num) return std::isfinite(v->num) ? v->num : def;
    if (v->type != JType::Str) return def;   // null / bool / arr / obj → default
    const std::string text = trim(v->str);
    if (text.empty()) return def;
    char* endp = nullptr;
    const double num = std::strtod(text.c_str(), &endp);
    if (endp == text.c_str() || *endp != '\0') return def;
    if (!std::isfinite(num)) return def;
    return num;
}

// _as_port：allow_zero 时 0 合法（http_port=0 = 不起 HTTP 入口），其余必须 1..65535。
int as_port(const Json* v, int def, bool allow_zero) {
    const double num = as_number(v, static_cast<double>(def));
    if (!std::isfinite(num) || num < -2147483648.0 || num > 2147483647.0) return def;
    const int port = static_cast<int>(num);
    if (allow_zero && port == 0) return 0;
    return (port >= 1 && port <= 65535) ? port : def;
}

// 契约里 connect_timeout 是 int 秒（Python 允许 0.6s，见报告「契约问题」）。
// 归一规则：非正 / >300 → 回落默认；亚秒 → 抬到 1s（int 字段装不下小数）。
int as_timeout_secs(const Json* v, int def) {
    const double num = as_number(v, static_cast<double>(def));
    if (!(num > 0) || num > 300) return def;
    if (num < 1) return 1;
    return static_cast<int>(num);
}

// _as_bool：字符串认 true/false/1/0/yes/no/on/off（大小写与空白不敏感）。
bool as_bool(const Json* v, bool def) {
    if (!v) return def;
    if (v->type == JType::Bool) return v->b;
    if (v->type == JType::Num) return v->num != 0;
    if (v->type == JType::Str) {
        const std::string low = lower(trim(v->str));
        if (low == "1" || low == "true" || low == "yes" || low == "on") return true;
        if (low == "0" || low == "false" || low == "no" || low == "off" || low.empty()) return false;
        return def;
    }
    return def;   // null / arr / obj
}

// _as_text：None / dict / list → default；其余取字符串形态。
std::string as_text(const Json* v, const std::string& def) {
    if (!v) return def;
    if (v->type == JType::Null || v->type == JType::Obj || v->type == JType::Arr) return def;
    return v->as_str(def);
}

// _as_host_list：接受数组，也接受逗号/空格分隔的字符串；逐项 trim、去空、去重（保序）。
// 注意：Python 这一层不做小写（小写发生在 rules.py::_norm），这里保持一致。
std::vector<std::string> as_host_list(const Json* v, const std::vector<std::string>& def) {
    if (!v) return def;
    std::vector<std::string> items;
    if (v->type == JType::Str) {
        std::string cur;
        for (char ch : v->str) {
            if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                if (!cur.empty()) {
                    items.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) items.push_back(cur);
    } else if (v->type == JType::Arr) {
        for (const Json& item : v->arr) {
            if (item.type == JType::Null || item.type == JType::Obj || item.type == JType::Arr) {
                log_write("DEBUG", "config: 丢弃非法域名项");
                continue;
            }
            items.push_back(item.as_str(""));
        }
    } else {
        return def;
    }
    std::vector<std::string> out;
    for (const std::string& raw : items) {
        const std::string text = trim(raw);
        if (text.empty()) continue;
        bool dup = false;
        for (const std::string& seen : out) {
            if (seen == text) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(text);
    }
    return out;
}

Json str_arr(const std::vector<std::string>& items) {
    Json a = Json::make_arr();
    for (const std::string& s : items) a.push(Json::make_str(s));
    return a;
}

const Json* sub_obj(const Json* parent) {
    if (!parent || parent->type != JType::Obj) return nullptr;
    return parent;
}

std::string enum_text(const Json* obj, const char* key, const char* def,
                      const char* const* allowed, std::size_t n) {
    const std::string text = lower(trim(as_text(obj ? obj->get(key) : nullptr, def)));
    return in_keys(text, allowed, n) ? text : std::string(def);
}

const char* const kModes[] = {"whitelist", "all", "blacklist"};
const char* const kKinds[] = {"socks5", "http", "direct"};

// ---------------------------------------------------------------- _normalize
// 入参是「已按 Python 方式合并」的 Json（形状 = config_to_json），逐键归一到 Config。
Config normalize(const Json& j) {
    Config c;

    c.listen_host = as_text(j.get("listen_host"), "127.0.0.1");
    if (c.listen_host.empty()) c.listen_host = "127.0.0.1";   // Python 的 `or "127.0.0.1"`
    c.listen_port = as_port(j.get("listen_port"), 7899, false);
    c.http_port = as_port(j.get("http_port"), 7900, true);    // 0 合法：不起 HTTP 入口
    c.mode = enum_text(&j, "mode", "whitelist", kModes, sizeof kModes / sizeof kModes[0]);
    c.connect_timeout = as_timeout_secs(j.get("connect_timeout"), 12);
    c.auto_node = as_bool(j.get("auto_node"), true);

    const Json* up = sub_obj(j.get("upstream"));
    c.upstream.kind = enum_text(up, "kind", "socks5", kKinds, sizeof kKinds / sizeof kKinds[0]);
    c.upstream.host = as_text(up ? up->get("host") : nullptr, "NODE_HOST");
    c.upstream.port = as_port(up ? up->get("port") : nullptr, 1080, false);
    c.upstream.user = as_text(up ? up->get("username") : nullptr, "");
    c.upstream.pass = as_text(up ? up->get("password") : nullptr, "");

    const Json* auth = sub_obj(j.get("auth"));
    c.auth.user = as_text(auth ? auth->get("username") : nullptr, "");
    c.auth.pass = as_text(auth ? auth->get("password") : nullptr, "");

    const Json* br = sub_obj(j.get("browser"));
    c.browser.apply_system_proxy = as_bool(br ? br->get("apply_system_proxy") : nullptr, true);
    c.browser.use_pac = as_bool(br ? br->get("use_pac") : nullptr, false);
    c.browser.bypass = as_text(br ? br->get("bypass") : nullptr, kDefaultBypass);

    const Json* cdn = sub_obj(j.get("cdn"));
    c.cdn.enabled = as_bool(cdn ? cdn->get("enabled") : nullptr, false);
    const double hours = as_number(cdn ? cdn->get("refresh_hours") : nullptr, 6);
    c.cdn.refresh_hours = (hours > 0) ? hours : 6;
    c.cdn.sni_port = as_port(cdn ? cdn->get("sni_port") : nullptr, 443, false);
    c.cdn.hosts = as_host_list(cdn ? cdn->get("hosts") : nullptr,
                               std::vector<std::string>(kCdnDomains, kCdnDomains + kCdnDomains_count));

    c.proxy_hosts = as_host_list(j.get("proxy_hosts"), kNoHosts);
    c.direct_hosts = as_host_list(j.get("direct_hosts"), kNoHosts);
    if (j.has("proxy_suffixes")) c.proxy_suffixes = as_host_list(j.get("proxy_suffixes"), kNoHosts);
    if (j.has("direct_suffixes")) c.direct_suffixes = as_host_list(j.get("direct_suffixes"), kNoHosts);
    return c;
}

}  // namespace

// ================================================================ 对外
Config default_config() {
    Config c;
    c.listen_host = "127.0.0.1";
    c.listen_port = 7899;
    c.http_port = 7900;
    c.mode = "whitelist";
    c.connect_timeout = 12;
    c.auto_node = true;

    c.upstream.kind = "socks5";
    c.upstream.host = "NODE_HOST";
    c.upstream.port = 1080;
    c.upstream.user.clear();
    c.upstream.pass.clear();

    c.auth.user.clear();
    c.auth.pass.clear();

    c.browser.apply_system_proxy = true;
    c.browser.use_pac = false;
    c.browser.bypass = kDefaultBypass;

    c.cdn.enabled = false;
    c.cdn.refresh_hours = 6;
    c.cdn.sni_port = 443;
    c.cdn.hosts.assign(kCdnDomains, kCdnDomains + kCdnDomains_count);
    return c;
}

Json config_to_json(const Config& cfg) {
    Json j = Json::make_obj();
    j.set("listen_host", Json::make_str(cfg.listen_host));
    j.set("listen_port", Json::make_num(cfg.listen_port));
    j.set("http_port", Json::make_num(cfg.http_port));
    j.set("mode", Json::make_str(cfg.mode));
    j.set("connect_timeout", Json::make_num(cfg.connect_timeout));
    j.set("auto_node", Json::make_bool(cfg.auto_node));

    Json up = Json::make_obj();
    up.set("kind", Json::make_str(cfg.upstream.kind));
    up.set("host", Json::make_str(cfg.upstream.host));
    up.set("port", Json::make_num(cfg.upstream.port));
    up.set("username", Json::make_str(cfg.upstream.user));
    up.set("password", Json::make_str(cfg.upstream.pass));
    j.set("upstream", std::move(up));

    Json auth = Json::make_obj();
    auth.set("username", Json::make_str(cfg.auth.user));
    auth.set("password", Json::make_str(cfg.auth.pass));
    j.set("auth", std::move(auth));

    Json br = Json::make_obj();
    br.set("apply_system_proxy", Json::make_bool(cfg.browser.apply_system_proxy));
    br.set("use_pac", Json::make_bool(cfg.browser.use_pac));
    br.set("bypass", Json::make_str(cfg.browser.bypass));
    j.set("browser", std::move(br));

    Json cdn = Json::make_obj();
    cdn.set("enabled", Json::make_bool(cfg.cdn.enabled));
    cdn.set("refresh_hours", Json::make_num(cfg.cdn.refresh_hours));
    cdn.set("sni_port", Json::make_num(cfg.cdn.sni_port));
    cdn.set("hosts", str_arr(cfg.cdn.hosts));
    j.set("cdn", std::move(cdn));

    j.set("proxy_hosts", str_arr(cfg.proxy_hosts));
    j.set("direct_hosts", str_arr(cfg.direct_hosts));
    // 高级键：不在模板里，写了才落盘。空列表语义上等价于「没写」（Router 会回落到内置名单），
    // 所以空就不写，保证「读进来再写出去」不漂移。
    if (!cfg.proxy_suffixes.empty()) j.set("proxy_suffixes", str_arr(cfg.proxy_suffixes));
    if (!cfg.direct_suffixes.empty()) j.set("direct_suffixes", str_arr(cfg.direct_suffixes));
    return j;
}

namespace {
std::vector<std::string> g_unknown_keys;
}

const std::vector<std::string>& unknown_config_keys() { return g_unknown_keys; }

Config merge_config(const Json* user) {
    g_unknown_keys.clear();
    Json merged = config_to_json(default_config());   // 等价于 Python 的 deepcopy(DEFAULT_CONFIG)
    if (!user || user->type != JType::Obj) {
        if (user) log_write("DEBUG", "config: 顶层不是对象，忽略");
        return normalize(merged);
    }
    for (const auto& kv : user->obj) {
        const std::string& key = kv.first;
        if (!key.empty() && key[0] == '_') {
            log_write("DEBUG", "config: 丢弃私有键 " + key);
            continue;
        }
        if (!is_top_key(key) && !is_extra_key(key)) {
            log_write("DEBUG", "config: 丢弃未知键 " + key);
            g_unknown_keys.push_back(key);
            continue;
        }
        Json* base = nullptr;
        for (auto& slot : merged.obj) {
            if (slot.first == key) {
                base = &slot.second;
                break;
            }
        }
        if (base && base->type == JType::Obj) {
            if (kv.second.type != JType::Obj) {
                log_write("DEBUG", "config: " + key + " 应为对象，忽略");
                continue;
            }
            for (const auto& sub : kv.second.obj) {
                const std::string& sk = sub.first;
                if (sk.empty() || sk[0] == '_' || !base->has(sk)) {
                    log_write("DEBUG", "config: 丢弃 " + key + " 下的未知键 " + sk);
                    continue;
                }
                base->set(sk, sub.second);
            }
        } else {
            merged.set(key, kv.second);
        }
    }
    return normalize(merged);
}

Config load_config(const Paths& p, std::string* err) {
    if (err) err->clear();
    // 文件不存在 = 用默认配置（Python: merge_config({})）
    std::error_code ec;
    if (p.config.empty() || !std::filesystem::exists(p.config, ec)) return merge_config(nullptr);

    std::ifstream f(p.config, std::ios::binary);
    if (!f) {
        if (err) *err = "config.json 无法读取";
        log_write("WARN", "config.json 无法读取，使用默认配置");
        return merge_config(nullptr);
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string perr;
    const Json user = Json::parse(text, &perr);   // Json::parse 自己跳过 UTF-8 BOM
    if (!perr.empty()) {
        const std::string msg = "config.json 解析失败: " + perr;
        if (err) *err = msg;
        log_write("WARN", msg + "，使用默认配置");
        return merge_config(nullptr);
    }
    return merge_config(&user);
}

bool save_config(const Paths& p, const Config& cfg) {
    if (p.config.empty()) {
        log_write("WARN", "config: 配置路径为空，跳过保存");
        return false;
    }
    std::error_code ec;
    const std::filesystem::path parent = p.config.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const std::string text = config_to_json(cfg).dump(2);   // 与 Python 一致：无尾随换行、无 BOM
    std::filesystem::path tmp = p.config;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            log_write("WARN", "config: 无法写入临时文件");
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            log_write("WARN", "config: 写临时文件失败");
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.wstring().c_str(), p.config.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long code = static_cast<unsigned long>(GetLastError());
        log_write("WARN", fmt("config: 原子替换落盘失败 (GetLastError=%lu)", code));
        std::filesystem::remove(tmp, ec);
        return false;
    }
    log_write("DEBUG", "config: 已保存");
    return true;
}

std::string config_note() {
    // 原文取自 config.example.json 的 `_note`（Python 版 canonical 模板说明）。
    return R"NOTE(本文件是配置的唯一 canonical 模板，键集与类型必须和 svpn/cli.py 的 DEFAULT_CONFIG 完全一致（tests/test_config_contract.py 守着）；所有 _ 开头的键（如本行）只作说明，load_config 加载时直接丢弃，不会进入运行配置。upstream.kind 可选 socks5 / http（host 填 127.0.0.1 就是你本机的 Clash / v2rayN / sing-box）；mode 可选 whitelist / all / blacklist。cdn.enabled=true 的真实行为是「启 SNI 透明转发」：svpn hosts apply 不写选边 IP，而是把 cdn.hosts 全部指向 127.0.0.1，由常驻的 svpn up 监听 cdn.sni_port（默认 443）按客户端 TLS SNI 拨到选边 IP —— 需要 svpn up 一直跑着，进程一停下载就走不了；cdn.enabled=false 则是老路子「hosts 直指选边 IP」，刷完 hosts 就生效、不用常驻进程。proxy_hosts / direct_hosts 是显式覆盖名单（分别代表「强制走节点」「强制直连」），优先于 svpn/rules.py 的内置 Steam 规则，空列表 = 不覆盖。)NOTE";
}

}  // namespace svpn
