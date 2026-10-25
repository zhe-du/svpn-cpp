// A1 自测：配置归一 + 分流规则。一次性程序，跑完即弃（不参与 build.cmd）。
// 编译：见 _crew/reports/A1-config.md
#include "../src/svpn.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace svpn;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
        std::printf("ok    %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

Config cfg_of(const char* text) {
    Json j = Json::parse(text);
    return merge_config(&j);
}

const char* b2s(bool v) { return v ? "true" : "false"; }

void dump_default(const Config& c) {
    std::printf("  listen_host      = %s\n", c.listen_host.c_str());
    std::printf("  listen_port      = %d\n", c.listen_port);
    std::printf("  http_port        = %d\n", c.http_port);
    std::printf("  mode             = %s\n", c.mode.c_str());
    std::printf("  connect_timeout  = %d\n", c.connect_timeout);
    std::printf("  auto_node        = %s\n", b2s(c.auto_node));
    std::printf("  upstream         = %s %s:%d user=[%s] pass=[%s]\n", c.upstream.kind.c_str(),
                c.upstream.host.c_str(), c.upstream.port, c.upstream.user.c_str(),
                c.upstream.pass.c_str());
    std::printf("  auth             = user=[%s] pass=[%s]\n", c.auth.user.c_str(), c.auth.pass.c_str());
    std::printf("  browser          = apply=%s use_pac=%s bypass.len=%zu\n",
                b2s(c.browser.apply_system_proxy), b2s(c.browser.use_pac), c.browser.bypass.size());
    std::printf("  cdn              = enabled=%s refresh=%.4g sni_port=%d hosts=%zu\n",
                b2s(c.cdn.enabled), c.cdn.refresh_hours, c.cdn.sni_port, c.cdn.hosts.size());
    std::printf("  proxy_hosts      = %zu\n", c.proxy_hosts.size());
    std::printf("  direct_hosts     = %zu\n", c.direct_hosts.size());
}

void test_default() {
    std::printf("\n=== 1. merge_config(nullptr) 逐字段 ===\n");
    const Config c = merge_config(nullptr);
    dump_default(c);
    check(c.listen_host == "127.0.0.1", "listen_host=127.0.0.1");
    check(c.listen_port == 7899, "listen_port=7899");
    check(c.http_port == 7900, "http_port=7900");
    check(c.mode == "whitelist", "mode=whitelist");
    check(c.connect_timeout == 12, "connect_timeout=12");
    check(c.auto_node, "auto_node=true");
    check(c.upstream.kind == "socks5", "upstream.kind=socks5");
    check(c.upstream.host == "NODE_HOST", "upstream.host=NODE_HOST");
    check(c.upstream.port == 1080, "upstream.port=1080");
    check(c.upstream.user.empty() && c.upstream.pass.empty(), "upstream 凭据为空");
    check(c.auth.user.empty() && c.auth.pass.empty(), "auth 凭据为空");
    check(c.browser.apply_system_proxy, "browser.apply_system_proxy=true");
    check(!c.browser.use_pac, "browser.use_pac=false");
    check(c.browser.bypass == kDefaultBypass, "browser.bypass == kDefaultBypass");
    check(c.browser.bypass.find("172.31.*") != std::string::npos, "bypass 含 172.31.*");
    check(c.browser.bypass.find("*.cn") != std::string::npos, "bypass 含 *.cn");
    check(!c.cdn.enabled, "cdn.enabled=false");
    check(c.cdn.refresh_hours == 6, "cdn.refresh_hours=6");
    check(c.cdn.sni_port == 443, "cdn.sni_port=443");
    check(c.cdn.hosts.size() == kCdnDomains_count, "cdn.hosts == kCdnDomains");
    check(c.proxy_hosts.empty() && c.direct_hosts.empty(), "proxy/direct_hosts 为空");
    check(c.proxy_suffixes.empty() && c.direct_suffixes.empty(), "高级键默认不出现");

    const Config d = default_config();
    check(config_to_json(c).dump(0) == config_to_json(d).dump(0), "merge_config(nullptr) == default_config");
    // 落盘/读回不漂移
    const Json as_json = config_to_json(c);
    const Config again = merge_config(&as_json);
    check(config_to_json(again).dump(0) == config_to_json(c).dump(0), "config_to_json → merge 往返不漂移");
    std::printf("  config_note() len = %zu\n", config_note().size());
    {
        // 与 config.example.json 逐键对照用（python 侧 diff）
        std::ofstream out("tests/out_default.json", std::ios::binary | std::ios::trunc);
        const std::string text = config_to_json(c).dump(2);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

void test_dirty() {
    std::printf("\n=== 2. 脏输入必须回落（不崩）===\n");
    const Config c = cfg_of(
        R"({"listen_port":null,"http_port":"","mode":"xxx","connect_timeout":-1,)"
        R"("cdn":{"refresh_hours":"abc"},"listen_host":null,"auto_node":"maybe"})");
    dump_default(c);
    check(c.listen_port == 7899, "listen_port:null → 7899");
    check(c.http_port == 7900, "http_port:\"\" → 7900");
    check(c.mode == "whitelist", "mode:xxx → whitelist");
    check(c.connect_timeout == 12, "connect_timeout:-1 → 12");
    check(c.cdn.refresh_hours == 6, "cdn.refresh_hours:\"abc\" → 6");
    check(c.listen_host == "127.0.0.1", "listen_host:null → 127.0.0.1");
    check(c.auto_node, "auto_node:\"maybe\" → true");

    // http_port=0 是合法值（不起 HTTP 入口），不能被抬回 7900
    check(cfg_of(R"({"http_port":0})").http_port == 0, "http_port:0 → 0（合法）");
    check(cfg_of(R"({"http_port":70000})").http_port == 7900, "http_port:70000 → 7900");
    check(cfg_of(R"({"listen_port":"8080"})").listen_port == 8080, "listen_port:\"8080\" → 8080");
    check(cfg_of(R"({"connect_timeout":999})").connect_timeout == 12, "connect_timeout:999 → 12（>300）");
    check(cfg_of(R"({"connect_timeout":"8.5"})").connect_timeout == 8, "connect_timeout:\"8.5\" → 8");
    check(cfg_of(R"({"connect_timeout":0})").connect_timeout == 12, "connect_timeout:0 → 12");
    check(cfg_of(R"({"mode":"  BLACKLIST "})").mode == "blacklist", "mode 大小写/空白归一");
    check(!cfg_of(R"({"auto_node":"off"})").auto_node, "auto_node:\"off\" → false");
    check(cfg_of(R"({"auto_node":"yes"})").auto_node, "auto_node:\"yes\" → true");
    const Config br = cfg_of(R"({"browser":{"apply_system_proxy":"no","bypass":"","use_pac":"on"}})");
    check(!br.browser.apply_system_proxy, "browser.apply_system_proxy:\"no\" → false");
    check(br.browser.bypass.empty(), "browser.bypass:\"\" → 空串（Python 语义）");
    check(br.browser.use_pac, "browser.use_pac:\"on\" → true");
    const Config bad = cfg_of(R"({"upstream":"not-an-object","cdn":[],"auth":5})");
    check(bad.upstream.host == "NODE_HOST" && bad.upstream.port == 1080, "upstream 非对象 → 全默认");
    check(bad.cdn.sni_port == 443 && !bad.cdn.hosts.empty(), "cdn 非对象 → 全默认");
    check(bad.auth.user.empty(), "auth 非对象 → 全默认");
    check(cfg_of(R"({"upstream":{"kind":"quic","port":null,"host":123}})").upstream.kind == "socks5",
          "upstream.kind:quic → socks5");
    check(cfg_of(R"({"upstream":{"port":0}})").upstream.port == 1080, "upstream.port:0 → 1080");

    // ---- 契约表里的脏值逐条过一遍 ----
    check(cfg_of(R"({"listen_port":null})").listen_port == 7899, "listen_port:null → 7899");
    check(cfg_of(R"({"listen_port":"abc"})").listen_port == 7899, "listen_port:\"abc\" → 7899");
    check(cfg_of(R"({"listen_port":70000})").listen_port == 7899, "listen_port:70000 → 7899（越界）");
    check(cfg_of(R"({"http_port":-1})").http_port == 7900, "http_port:-1 → 7900");
    check(cfg_of(R"({"mode":"xxx"})").mode == "whitelist", "mode:\"xxx\" → whitelist");
    check(cfg_of(R"({"connect_timeout":-5})").connect_timeout == 12, "connect_timeout:-5 → 12");
    check(cfg_of(R"({"connect_timeout":9999})").connect_timeout == 12, "connect_timeout:9999 → 12");
    check(cfg_of(R"({"cdn":{"refresh_hours":"abc"}})").cdn.refresh_hours == 6,
          "cdn.refresh_hours:\"abc\" → 6");
    check(cfg_of(R"({"browser":{"apply_system_proxy":"yes"}})").browser.apply_system_proxy,
          "browser.apply_system_proxy:\"yes\" → true");
}

void test_keys_and_lists() {
    std::printf("\n=== 3. 未知键/私有键丢弃 + 名单归一 ===\n");
    const Config c = cfg_of(R"({"proxy_suffixes":["a.com"],"bogus":1,"_note":"x","proxy_hosts":"A.COM, b.com"})");
    const Json j = config_to_json(c);
    check(!j.has("bogus"), "bogus 被丢弃");
    check(!j.has("_note"), "_note 被丢弃");
    check(c.proxy_suffixes.size() == 1 && c.proxy_suffixes[0] == "a.com", "proxy_suffixes 生效");

    // 未知键/私有键存在时，同批次的合法键仍必须生效
    const Config mix = cfg_of(R"({"_note":"x","bogus":1,"mode":"all"})");
    check(mix.mode == "all", "混入 _note/bogus 后 mode 仍生效");
    check(!config_to_json(mix).has("bogus") && !config_to_json(mix).has("_note"),
          "_note/bogus 不随 config_to_json 出现");

    // 高级键：数组 1 项 / 逗号字符串 2 项
    const Config adv = cfg_of(R"({"proxy_suffixes":["a.com"],"direct_suffixes":"b.com, c.com"})");
    check(adv.proxy_suffixes.size() == 1, "proxy_suffixes 数组 → 1 项");
    check(adv.direct_suffixes.size() == 2, "direct_suffixes 逗号字符串 → 2 项");
    check(adv.direct_suffixes.size() == 2 && adv.direct_suffixes[0] == "b.com" &&
              adv.direct_suffixes[1] == "c.com",
          "direct_suffixes 逐项 trim 正确");
    check(c.proxy_hosts.size() == 2, "proxy_hosts 逗号串 → 2 项");
    check(c.proxy_hosts[0] == "A.COM" && c.proxy_hosts[1] == "b.com", "名单保序、仅 trim（Python 不改大小写）");

    const Config s = cfg_of(R"({"direct_hosts":"x.com  y.com,z.com x.com","direct_suffixes":",,a.com,,"})");
    check(s.direct_hosts.size() == 3, "空格/逗号混合 + 去重 → 3 项");
    check(s.direct_suffixes.size() == 1 && s.direct_suffixes[0] == "a.com", "空项被丢掉");

    const Config bad = cfg_of(R"({"proxy_hosts":[null,["x"],{"a":1},42,"h.com"]})");
    check(bad.proxy_hosts.size() == 2, "数组里的 null/数组/对象被丢弃，数字转文本");
    check(bad.proxy_hosts[0] == "42", "数字项 → \"42\"");
    check(cfg_of(R"({"cdn":{"hosts":"a.com, b.com"}})").cdn.hosts.size() == 2, "cdn.hosts 字符串 → 2 项");
    check(cfg_of(R"({"cdn":{"hosts":[]}})").cdn.hosts.empty(), "cdn.hosts 空数组 → 空（不回落）");
    check(cfg_of(R"({"cdn":{"hosts":5}})").cdn.hosts.size() == kCdnDomains_count, "cdn.hosts 非名单 → 内置");
}

void test_router() {
    std::printf("\n=== 4. Router.decide ===\n");
    struct Case {
        const char* host;
        const char* whitelist;
        const char* blacklist;
        const char* all;
    };
    const Case cases[] = {
        {"store.steampowered.com", "upstream", "upstream", "upstream"},
        {"www.baidu.com", "direct", "direct", "upstream"},
        {"notsteampowered.com", "direct", "upstream", "upstream"},   // 关键：不能被 ends_with 骗到
        {"127.0.0.1", "direct", "direct", "direct"},
        {"192.168.1.1", "direct", "direct", "direct"},
        {"23.45.64.1", "upstream", "upstream", "upstream"},
        {"www.google.com", "direct", "upstream", "upstream"},
        {"localhost", "direct", "direct", "direct"},
        {"foo.localhost", "direct", "direct", "direct"},
        {"steamloopback.host", "direct", "direct", "direct"},
        {"172.17.0.1", "direct", "direct", "direct"},
        {"8.8.8.8", "direct", "upstream", "upstream"},
        {"", "direct", "upstream", "upstream"},   // 空 host：Python 在 blacklist/all 下判 upstream
    };
    for (const Case& cs : cases) {
        Config w = default_config();
        w.mode = "whitelist";
        Config bl = default_config();
        bl.mode = "blacklist";
        Config all = default_config();
        all.mode = "all";
        const std::string wl = Router(w).decide(cs.host);
        const std::string bk = Router(bl).decide(cs.host);
        const std::string al = Router(all).decide(cs.host);
        const char* label = cs.host[0] ? cs.host : "(empty)";
        std::printf("  %-24s whitelist=%-8s blacklist=%-8s all=%s\n", label, wl.c_str(), bk.c_str(),
                    al.c_str());
        check(wl == cs.whitelist, std::string("whitelist ") + label);
        check(bk == cs.blacklist, std::string("blacklist ") + label);
        check(al == cs.all, std::string("all ") + label);
    }

    // 显式名单压过内置规则
    Config ov = default_config();
    ov.direct_hosts = {"store.steampowered.com"};
    ov.proxy_hosts = {"www.baidu.com"};
    const Router r(ov);
    check(r.decide("store.steampowered.com") == "direct", "direct_hosts 压过 Steam 后缀");
    check(r.decide("STORE.SteamPowered.COM.") == "direct", "名单匹配前做 normalize");
    check(r.decide("www.baidu.com") == "upstream", "proxy_hosts 压过 whitelist 兜底");

    // 自定义后缀/网段
    Config cs = default_config();
    cs.mode = "whitelist";
    cs.proxy_suffixes = {"example.net"};
    const Router rs(cs);
    check(rs.decide("a.example.net") == "upstream", "自定义 proxy_suffixes 生效");
    check(rs.decide("store.steampowered.com") == "direct", "自定义后缀替换内置名单（Python 语义）");
}

void test_primitives() {
    std::printf("\n=== 5. 基础规则 ===\n");
    check(normalize_host("  Store.SteamPowered.COM.  ") == "store.steampowered.com", "normalize_host 小写/去点/去空白");
    check(normalize_host("[::1]") == "::1", "normalize_host 去方括号");
    check(normalize_host("...") == "", "normalize_host 全点 → 空");

    check(match_suffix("store.steampowered.com", "steampowered.com"), "子域匹配");
    check(match_suffix("steampowered.com", "steampowered.com"), "等值匹配");
    check(match_suffix("STEAMPOWERED.COM.", ".steampowered.com"), "大小写 + 前导点");
    check(!match_suffix("notsteampowered.com", "steampowered.com"), "notsteampowered.com 不匹配");
    check(!match_suffix("", "cn"), "空 host 不匹配");
    check(match_suffix("www.a.cn", "cn"), "顶级后缀 cn 匹配 .cn");
    check(!match_suffix("acn", "cn"), "acn 不匹配 cn");

    check(is_ip("1.2.3.4"), "v4");
    check(is_ip("0.0.0.0"), "v4 全零");
    check(!is_ip("256.1.1.1"), "v4 越界");
    check(!is_ip("1.2.3"), "v4 三节");
    check(!is_ip("1.2.3.4.5"), "v4 五节");
    check(!is_ip("010.1.1.1"), "v4 前导零（Python 3.9+ 拒绝）");
    check(!is_ip("1.2.3.4:80"), "host:port 不是 IP");
    check(is_ip("::1"), "v6 ::1");
    check(is_ip("[::1]"), "v6 带方括号");
    check(is_ip("2001:db8::1"), "v6 压缩");
    check(is_ip("::ffff:1.2.3.4"), "v6 内嵌 v4");
    check(is_ip("fe80::1"), "v6 链路本地");
    check(!is_ip(":::"), "v6 三个冒号非法");
    check(!is_ip("2001:db8::1::2"), "v6 两个 ::");
    check(!is_ip("fe80::1%eth0"), "v6 带 zone 非法");
    check(!is_ip("1:2:3:4:5:6:7:8:9"), "v6 九组非法");
    check(!is_ip(""), "空串不是 IP");

    check(in_cidr("23.45.64.1", "23.45.64.0/18"), "v4 段命中");
    check(!in_cidr("23.45.128.1", "23.45.64.0/18"), "v4 段边界外");
    check(in_cidr("23.45.127.255", "23.45.64.0/18"), "v4 段广播地址仍在段内");
    check(in_cidr("10.0.0.5", "10.0.0.5"), "无 / 按单主机");
    check(!in_cidr("10.0.0.6", "10.0.0.5"), "无 / 且不等");
    check(in_cidr("1.2.3.4", "0.0.0.0/0"), "/0 全命中");
    check(in_cidr("::1", "::1/128"), "v6 /128");
    check(in_cidr("2001:db8::1", "2001:db8::/32"), "v6 /32");
    check(!in_cidr("1.2.3.4", "::/0"), "v4 地址对 v6 段不匹配");
    check(!in_cidr("1.2.3.4", "1.2.3.4/33"), "非法前缀长度");
    check(!in_cidr("1.2.3.4", "1.2.3.4/33"), "非法前缀长度");
    check(!in_cidr("1.2.3.4", "abc"), "非法段");

    // ---- 契约表里的边界三元组 ----
    check(match_suffix("b.com", "b.com"), "match_suffix 等值 b.com/b.com");
    check(match_suffix("a.b.com", "b.com"), "match_suffix 子域 a.b.com/b.com");
    check(!match_suffix("xb.com", "b.com"), "match_suffix xb.com/b.com 不匹配");
    check(in_cidr("23.45.64.1", "23.45.64.0/18"), "in_cidr 23.45.64.1 ∈ 23.45.64.0/18");
    check(!is_ip("1.2.3"), "is_ip 1.2.3 false");
    check(is_ip("::1"), "is_ip ::1 true");
    check(is_ip("[::1]"), "is_ip [::1] true");
}

void test_io() {
    std::printf("\n=== 6. load/save（BOM、坏 JSON、原子替换）===\n");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "svpn_a1_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    Paths p;
    p.root = dir;
    p.config = dir / "config.json";
    p.runtime = dir / "runtime";

    fs::remove(p.config, ec);
    std::string err;
    const Config nofile = load_config(p, &err);
    check(err.empty() && nofile.listen_port == 7899, "文件不存在 → 默认，无错误");

    // 带 BOM + 部分键
    {
        std::ofstream f(p.config, std::ios::binary | std::ios::trunc);
        const std::string text = "\xEF\xBB\xBF{\"listen_port\": 1234, \"mode\": \"all\"}";
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    const Config bom = load_config(p, &err);
    check(err.empty(), "BOM 文件解析无错误");
    check(bom.listen_port == 1234 && bom.mode == "all", "BOM 文件读到的键生效");

    // 保存 → 再读回，必须完全一致且无 BOM
    Config saved = bom;
    saved.cdn.hosts = {"a.example", "b.example"};
    saved.proxy_suffixes = {"x.example"};
    check(save_config(p, saved), "save_config 成功");
    {
        std::ifstream f(p.config, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(text.size() >= 3 && static_cast<unsigned char>(text[0]) != 0xEF, "落盘无 BOM");
        check(text.find("\"listen_port\": 1234") != std::string::npos, "落盘是 2 空格缩进");
        check(text.find("\"_") == std::string::npos, "落盘不含 _ 开头的键");
    }
    const Config back = load_config(p, &err);
    check(config_to_json(back).dump(0) == config_to_json(saved).dump(0), "save → load 往返一致");
    check(back.proxy_suffixes.size() == 1, "高级键写盘后仍生效");
    check(!fs::exists(dir / "config.json.tmp", ec), "临时文件已被替换掉");

    // 坏 JSON：返回默认 + err，绝不抛
    {
        std::ofstream f(p.config, std::ios::binary | std::ios::trunc);
        const std::string text = "{\"listen_port\": 1234,,}";
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    err.clear();
    const Config badj = load_config(p, &err);
    check(!err.empty(), "坏 JSON 设置了 err");
    check(badj.listen_port == 7899, "坏 JSON → 全默认");
    std::printf("  err = %s\n", err.c_str());

    // 空文件 / 顶层不是对象
    {
        std::ofstream f(p.config, std::ios::binary | std::ios::trunc);
        f << "";
    }
    const Config empty = load_config(p, &err);
    check(!err.empty() && empty.listen_port == 7899, "空文件 → err + 默认");
    {
        std::ofstream f(p.config, std::ios::binary | std::ios::trunc);
        f << "[1,2,3]";
    }
    const Config arr = load_config(p, &err);
    check(err.empty() && arr.listen_port == 7899, "顶层数组 → 默认（不报错，Python 同）");

    fs::remove_all(dir, ec);
}

}  // namespace

int main(int argc, char** argv) {
    console_utf8();
    if (argc > 1 && std::string(argv[1]) == "--route") {
        // 差分探针：svpn_a1_probe --route <mode> <host...>，逐行打 "host\tdecision"
        Config c = default_config();
        if (argc > 2) c.mode = argv[2];
        const Router r(c);
        for (int i = 3; i < argc; ++i) {
            std::printf("%s\t%s\n", argv[i], r.decide(argv[i]).c_str());
        }
        return 0;
    }
    if (argc > 1) {
        // 差分探针：svpn_a1_probe <cfg.json>，把 merge_config 的结果按紧凑 JSON 打到 stdout，
        // 供 scripts 侧和 Python cli.merge_config 逐键对比。
        std::ifstream f(argv[1], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        const Json user = Json::parse(text, &err);
        if (!err.empty()) {
            std::printf("PARSE_ERROR %s\n", err.c_str());
            return 2;
        }
        std::printf("%s\n", config_to_json(merge_config(&user)).dump(-1).c_str());
        return 0;
    }
    std::printf("svpn A1 自测：config + rules\n");
    test_default();
    test_dirty();
    test_keys_and_lists();
    test_router();
    test_primitives();
    test_io();
    std::printf("\n%s PASS %d / FAIL %d（共 %d 项）\n", g_fail ? "[!]" : "[+]", g_pass, g_fail,
                g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
