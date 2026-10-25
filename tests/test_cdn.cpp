// A5 cdn 自测：缓存兼容 + DoH 实网 + 坏 provider + 选边实测。
// 编译（单行）：
//   g++ -std=c++20 -O2 -Wall -Wextra src/cdn.cpp src/json.cpp src/util.cpp src/rules.cpp src/rules_data.cpp src/net.cpp src/tls.cpp tests/test_cdn.cpp -o build/test_cdn.exe -lws2_32 -lcrypt32 -lsecur32 -lwinhttp
// SVPN_TEST_NET=0 只跑离线用例。
#include "../src/svpn.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

using namespace svpn;

namespace {

int g_fail = 0;

void check(bool cond, const char* what, int line) {
    if (cond) {
        std::printf("  [v] %s\n", what);
    } else {
        std::printf("  [x] %s  (line %d)\n", what, line);
        ++g_fail;
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

std::filesystem::path temp_dir() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "svpn-a5-cdn";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void write_raw(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"wb") == 0 && f != nullptr) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

Paths paths_at(const std::filesystem::path& dir) {
    Paths p;
    p.root = dir;
    p.runtime = dir / "runtime";
    p.cdn_cache = p.runtime / "cdn-cache.json";
    return p;
}

DomainResult sample_result(const std::string& domain) {
    DomainResult r;
    r.domain = domain;
    r.source = "DoH:doh.pub+system";
    r.candidates = 4;
    r.tested = 4;
    r.best_ms = 156.8;
    r.best_mbps = 1.58;
    r.ts = static_cast<std::int64_t>(std::time(nullptr));

    Edge a;
    a.ip = "1.2.3.4";
    a.ms = 156.8;
    a.mbps = 1.58;
    a.code = 206;
    Edge b;
    b.ip = "5.6.7.8";
    b.ms = 210.4;
    b.err = "HTTP 403";
    b.code = 403;
    r.top = {a, b};
    r.best_ip = a.ip;
    return r;
}

// ---------------------------------------------------------------- 缓存
void test_cache_roundtrip(const std::filesystem::path& dir) {
    std::printf("[1] 缓存往返（父目录自动创建 / 字段不丢）\n");
    Paths p = paths_at(dir / "rt");
    Cache c;
    c["steamcdn-a.akamaihd.net"] = sample_result("steamcdn-a.akamaihd.net");
    CHECK(save_cache(p, c));
    CHECK(std::filesystem::exists(p.cdn_cache));
    CHECK(!std::filesystem::exists(std::filesystem::path(p.cdn_cache.wstring() + L".tmp")));

    Cache back = load_cache(p, 6 * 3600);
    CHECK(back.size() == 1);
    const DomainResult& r = back["steamcdn-a.akamaihd.net"];
    CHECK(r.domain == "steamcdn-a.akamaihd.net");
    CHECK(r.source == "DoH:doh.pub+system");
    CHECK(r.candidates == 4);
    CHECK(r.tested == 4);
    CHECK(r.best_ip == "1.2.3.4");
    CHECK(r.best_ms > 156.7 && r.best_ms < 156.9);
    CHECK(r.best_mbps > 1.57 && r.best_mbps < 1.59);
    CHECK(r.top.size() == 2);
    CHECK(r.top[1].code == 403);
    CHECK(r.top[1].err == "HTTP 403");

    const double age = cache_age_hours(p);
    CHECK(age >= 0.0 && age < 0.01);

    auto m = best_ip_map(p, 6 * 3600);
    CHECK(m.size() == 1 && m["steamcdn-a.akamaihd.net"] == "1.2.3.4");

    Config cfg;
    apply_ip_map(p, cfg, 6 * 3600);
    CHECK(cfg.ip_map.size() == 1 && cfg.ip_map["steamcdn-a.akamaihd.net"] == "1.2.3.4");

    std::printf("      文件头 160 字节: ");
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.cdn_cache.wstring().c_str(), L"rb") == 0 && f != nullptr) {
        char buf[161] = {0};
        const std::size_t n = std::fread(buf, 1, 160, f);
        buf[n] = '\0';
        std::printf("%s\n", buf);
        std::fclose(f);
    }
}

void test_cache_expiry(const std::filesystem::path& dir) {
    std::printf("[2] 缓存过期（16 小时前的假缓存必须作废）\n");
    Paths p = paths_at(dir / "old");
    std::error_code ec;
    std::filesystem::create_directories(p.runtime, ec);
    const long long old = static_cast<long long>(std::time(nullptr)) - 16 * 3600;
    write_raw(p.cdn_cache,
              "{\"updated\": " + std::to_string(old) +
                  ", \"domains\": {\"a.steamstatic.com\": {\"domain\": \"a.steamstatic.com\", "
                  "\"best_ip\": \"23.45.64.5\", \"ts\": " + std::to_string(old) + "}}}");
    CHECK(load_cache(p, 6 * 3600).empty());
    CHECK(load_cache(p, 64 * 3600).size() == 1);
    const double age = cache_age_hours(p);
    CHECK(age > 15.9 && age < 16.1);
    CHECK(best_ip_map(p, 6 * 3600).empty());

    std::printf("[3] 坏文件 / 缺文件 / max_age=0\n");
    Paths bad = paths_at(dir / "bad");
    CHECK(load_cache(bad, 3600).empty());                       // 文件不存在
    CHECK(cache_age_hours(bad) < 0);
    write_raw(bad.cdn_cache, "{not json");
    CHECK(load_cache(bad, 3600).empty());
    CHECK(cache_age_hours(bad) < 0);
    write_raw(bad.cdn_cache, "{\"updated\": 0, \"domains\": {}}");
    CHECK(load_cache(bad, 3600).empty());                       // updated=0 → 过期
    write_raw(bad.cdn_cache, "[]");
    CHECK(load_cache(bad, 3600).empty());                       // 顶层不是对象

    Paths zero = paths_at(dir / "zero");
    Cache c;
    c["a.com"] = sample_result("a.com");
    CHECK(save_cache(zero, c));
    CHECK(load_cache(zero, 0).empty());
}

// Python tests/test_logic.py::TestCdnCache::test_best_ip_map 的同款用例
void test_best_ip_filter(const std::filesystem::path& dir) {
    std::printf("[4] best_ip_map 过滤（v6 收 / 脏值弃 / null 条目弃）\n");
    Paths p = paths_at(dir / "filter");
    std::error_code ec;
    std::filesystem::create_directories(p.runtime, ec);
    write_raw(p.cdn_cache,
              "{\"updated\": 9999999999, \"domains\": {"
              "\"good.steamstatic.com\": {\"best_ip\": \"23.45.64.5\", \"best_ms\": 30},"
              "\"v6.steamstatic.com\": {\"best_ip\": \"2408::1\"},"
              "\"junk.steamstatic.com\": {\"best_ip\": \"not-an-ip\"},"
              "\"empty.steamstatic.com\": {\"best_ms\": 10},"
              "\"null.steamstatic.com\": null}}");
    auto m = best_ip_map(p, 6 * 3600);
    CHECK(m.size() == 2);
    CHECK(m.count("good.steamstatic.com") == 1 && m["good.steamstatic.com"] == "23.45.64.5");
    CHECK(m.count("v6.steamstatic.com") == 1 && m["v6.steamstatic.com"] == "2408::1");
    CHECK(m.count("junk.steamstatic.com") == 0);
    CHECK(m.count("empty.steamstatic.com") == 0);
    CHECK(m.count("null.steamstatic.com") == 0);
    CHECK(load_cache(p, 6 * 3600).size() == 4);                 // null 条目被跳过
}

// ---------------------------------------------------------------- DoH
void test_doh_live() {
    std::printf("[5] DoH 实网（doh.pub → store.steampowered.com）\n");
    std::vector<std::string> ips;
    std::string err;
    const bool ok = doh_query("https://doh.pub/dns-query", "store.steampowered.com", 5000, &ips,
                              &err);
    std::printf("      ok=%d err=\"%s\" n=%zu:", static_cast<int>(ok), err.c_str(), ips.size());
    for (const std::string& ip : ips) std::printf(" %s", ip.c_str());
    std::printf("\n");
    CHECK(ok);
    CHECK(!ips.empty());
    CHECK(err.empty());
    for (const std::string& ip : ips) CHECK(ip.find(':') == std::string::npos);

    // 同一个域名问第二家：应一致地拿到结果
    std::vector<std::string> ips2;
    std::string err2;
    const bool ok2 =
        doh_query("https://dns.alidns.com/dns-query", "store.steampowered.com", 5000, &ips2, &err2);
    std::printf("      alidns: ok=%d n=%zu\n", static_cast<int>(ok2), ips2.size());
    if (!ok2) std::printf("      alidns 不可用（可接受）: %s\n", err2.c_str());
}

void test_doh_bad() {
    std::printf("[6] 坏 provider / 坏域名（必须 false，不许崩）\n");
    std::vector<std::string> ips;
    std::string err;

    CHECK(!doh_query("https://127.0.0.1:1/x", "store.steampowered.com", 3000, &ips, &err));
    std::printf("      127.0.0.1:1 → \"%s\"\n", err.c_str());
    CHECK(!err.empty());
    CHECK(ips.empty());

    err.clear();
    CHECK(!doh_query("http://doh.pub/dns-query", "a.com", 2000, &ips, &err));
    std::printf("      http:// → \"%s\"\n", err.c_str());

    err.clear();
    CHECK(!doh_query("https://doh.pub/dns-query", "", 2000, &ips, &err));
    std::printf("      空域名 → \"%s\"\n", err.c_str());

    err.clear();
    CHECK(!doh_query("https://doh.pub/dns-query", "bad domain\",\"x\":\"1", 2000, &ips, &err));
    std::printf("      注入域名 → \"%s\"\n", err.c_str());

    err.clear();
    CHECK(!doh_query("https://doh.pub/dns-query", ".", 2000, &ips, &err));
    std::printf("      单个点 → \"%s\"\n", err.c_str());

    err.clear();
    CHECK(!doh_query("", "a.com", 2000, &ips, &err));
    CHECK(!err.empty());

    err.clear();
    std::string longname(300, 'a');
    CHECK(!doh_query("https://doh.pub/dns-query", longname, 2000, &ips, &err));
    CHECK(!err.empty());

    // NXDOMAIN：Status=3 也必须 false
    err.clear();
    const bool nx = doh_query("https://doh.pub/dns-query",
                              "no-such-host-a5-test.steampowered.com", 5000, &ips, &err);
    std::printf("      NXDOMAIN → ok=%d \"%s\"\n", static_cast<int>(nx), err.c_str());
    CHECK(!nx);
}

// ---------------------------------------------------------------- 选边
void test_rank_live() {
    std::printf("[7] rank_domain 实网（steamcdn-a.akamaihd.net, bench=true, 8s）\n");
    const DomainResult r = rank_domain("steamcdn-a.akamaihd.net", true, 8000);
    std::printf("      source=%s candidates=%d tested=%d\n", r.source.c_str(), r.candidates,
                r.tested);
    std::printf("      best_ip=%s best_ms=%.1f best_mbps=%.2f ts=%lld\n", r.best_ip.c_str(),
                r.best_ms, r.best_mbps, static_cast<long long>(r.ts));
    for (const Edge& e : r.top) {
        std::printf("      top: %-16s ms=%7.1f mbps=%6.2f code=%d err=%s\n", e.ip.c_str(), e.ms,
                    e.mbps, e.code, e.err.c_str());
    }
    CHECK(!r.source.empty() && r.source != "none");
    CHECK(r.candidates > 0);
    CHECK(r.tested > 0);
    CHECK(!r.best_ip.empty());
    CHECK(r.top.size() <= 5);
    // 排序契约：有吞吐的排前面，吞吐降序
    for (std::size_t i = 1; i < r.top.size(); ++i) {
        CHECK(r.top[i - 1].score() <= r.top[i].score());
        if (r.top[i - 1].mbps > 0 && r.top[i].mbps > 0) CHECK(r.top[i - 1].mbps >= r.top[i].mbps);
    }

    std::printf("[8] rank_domain 无实测（bench=false）+ 缓存落盘\n");
    const DomainResult r2 = rank_domain("media.steampowered.com", false, 5000);
    std::printf("      %s → %s ms=%.1f candidates=%d tested=%d\n", r2.domain.c_str(),
                r2.best_ip.c_str(), r2.best_ms, r2.candidates, r2.tested);
    CHECK(r2.candidates > 0);
    if (!r2.best_ip.empty()) CHECK(r2.best_mbps == 0.0);

    Paths p = paths_at(std::filesystem::temp_directory_path() / "svpn-a5-cdn" / "live");
    Cache c;
    c[r.domain] = r;
    c[r2.domain] = r2;
    CHECK(save_cache(p, c));
    const Cache back = load_cache(p, 6 * 3600);
    CHECK(back.size() == 2);
    std::printf("      落盘: %s\n", p.cdn_cache.string().c_str());
}

}  // namespace

int main() {
    console_utf8();
    const std::filesystem::path dir = temp_dir();
    std::printf("== A5 cdn 自测 ==  临时目录 %s\n", dir.string().c_str());

    // SVPN_TEST_NET=0：只跑离线用例（缓存/过滤），不碰网络
    const char* net = std::getenv("SVPN_TEST_NET");
    const bool live = !(net != nullptr && std::string(net) == "0");

    test_cache_roundtrip(dir);
    test_cache_expiry(dir);
    test_best_ip_filter(dir);
    if (live) {
        test_doh_live();
        test_doh_bad();
        test_rank_live();
    } else {
        std::printf("[5][6][7][8] 已跳过（SVPN_TEST_NET=0）\n");
    }

    std::printf("\n== %s ==  失败 %d 项\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
