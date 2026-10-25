// A6 自测：hosts 托管块（字节保真）/ PAC 生成 / WinINET 只读与备份。
//
// 跑法（在 svpn-cpp 根目录）：
//   g++ ... tests/test_winsys.cpp src/winsys.cpp src/pac.cpp src/util.cpp src/json.cpp
//        src/rules_data.cpp -ldnsapi -lwininet -lshell32 -o build_test_a6.exe
//   build_test_a6.exe _crew/tmp_a6
//
// 注意：本测试**绝不**碰真实 hosts（用 SVPN_HOSTS_FILE 改道到临时目录），
// 也**绝不**改本机系统代理（只调用 read_wininet / backup_wininet）。
#include "../src/svpn.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void check(bool cond, const std::string& what) {
    std::printf("%-58s %s\n", what.c_str(), cond ? "OK" : "FAIL");
    if (!cond) ++g_fail;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return out;
}

void write_all(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void set_hosts_env(const fs::path& p) {
    _wputenv_s(L"SVPN_HOSTS_FILE", p.wstring().c_str());
}

svpn::Paths temp_paths(const fs::path& dir) {
    svpn::Paths p;
    p.root = dir;
    p.runtime = dir / "runtime";
    p.hosts_backup = p.runtime / "hosts.backup";
    p.wininet_backup = p.runtime / "wininet-backup.json";
    p.log = p.runtime / "svpn.log";
    return p;
}

// apply → 存下产物 → clear → 与原始字节逐字节比对
void hosts_case(const std::string& name, const fs::path& dir,
                const std::map<std::string, std::string>& entries) {
    const fs::path hostf = dir / (name + ".bin");
    const fs::path cpp_applied = dir / (name + ".cppapplied");

    std::string orig = read_all(hostf);
    if (orig.empty()) {
        std::printf("%-58s %s\n", (name + " 样例缺失").c_str(), "FAIL");
        ++g_fail;
        return;
    }
    set_hosts_env(hostf);
    const svpn::Paths paths = temp_paths(dir);

    const svpn::HostsResult a = svpn::apply_hosts(entries, paths);
    std::printf("[%s] apply ok=%d entries=%d rejected=%zu reason=%s\n", name.c_str(),
                static_cast<int>(a.ok), a.entries, a.rejected.size(), a.reason.c_str());
    check(a.ok, name + " apply ok");
    check(a.entries == static_cast<int>(entries.size()), name + " entries == IP 个数");
    const std::string applied = read_all(hostf);
    write_all(cpp_applied, applied);

    const int counted = svpn::hosts_managed_entries(paths);
    check(counted == static_cast<int>(entries.size()), name + " hosts_managed_entries == 2");

    // 二次 apply：必须仍然只有一个托管块（幂等）
    const svpn::HostsResult a2 = svpn::apply_hosts(entries, paths);
    check(a2.ok, name + " 二次 apply ok");
    const std::string applied2 = read_all(hostf);
    check(applied2 == applied, name + " 二次 apply 幂等（字节相同）");
    const int counted2 = svpn::hosts_managed_entries(paths);
    check(counted2 == counted, name + " 二次 apply 后条目数不变");

    const svpn::HostsResult c = svpn::clear_hosts(paths);
    check(c.ok, name + " clear ok");
    const std::string cleared = read_all(hostf);

    std::printf("[%s] orig=%zu applied=%zu cleared=%zu\n", name.c_str(), orig.size(),
                applied.size(), cleared.size());
    check(cleared == orig, name + " clear 后与原始字节完全一致");
    check(svpn::hosts_managed_entries(paths) == 0, name + " clear 后托管条目 == 0");
}

// 原文件末尾没有换行：apply 必须在块前补两个换行，那一个字节找不回来
// （Python 版恒补一个换行，方向相反）。单独测，别让它污染「字节完全一致」的用例。
void hosts_no_trailing_nl_case(const std::string& name, const fs::path& dir,
                               const std::map<std::string, std::string>& entries) {
    const fs::path hostf = dir / (name + ".bin");
    const std::string orig = read_all(hostf);
    set_hosts_env(hostf);
    const svpn::Paths paths = temp_paths(dir);

    // ① 没有托管块时 clear 必须一个字节都不动
    const svpn::HostsResult c0 = svpn::clear_hosts(paths);
    check(c0.ok && read_all(hostf) == orig, name + " 无托管块时 clear 一个字节都没动");

    // ② apply → clear
    check(svpn::apply_hosts(entries, paths).ok, name + " apply ok");
    write_all(dir / (name + ".cppapplied"), read_all(hostf));
    check(svpn::clear_hosts(paths).ok, name + " clear ok");
    const std::string cleared = read_all(hostf);
    std::printf("[%s] orig=%zu cleared=%zu\n", name.c_str(), orig.size(), cleared.size());
    check(cleared.size() == orig.size() + 1, name + " clear 后多出 1 个换行（已在报告记录）");
    check(cleared.compare(0, orig.size(), orig) == 0, name + " 除末尾那个换行外逐字节相同");
}

void pac_case(const std::string& name, const fs::path& dir, const std::string& text) {
    write_all(dir / name, text);
    std::printf("[pac] %-24s %zu bytes\n", name.c_str(), text.size());
}

}  // namespace

int main(int argc, char** argv) {
    svpn::console_utf8();
    fs::path dir = (argc > 1) ? fs::path(argv[1]) : fs::path("_crew/tmp_a6");
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::map<std::string, std::string> entries = {
        {"127.0.0.1", "cdn-a.example.com cdn-b.example.com"},   // 顺序：std::map 按 key 升序
        {"203.0.113.7", "steam.example.org"},
    };

    std::puts("=== hosts（BOM+CRLF 中文 / GBK+LF 中文 / 末尾无换行）===");
    hosts_case("caseA_utf8bom_crlf", dir, entries);
    hosts_case("caseB_gbk_lf", dir, entries);
    hosts_no_trailing_nl_case("caseC_no_trailing_nl", dir, entries);

    std::puts("\n=== sanitize_entries（脏条目必须被拒）===");
    {
        std::string long_host(254, 'a');
        long_host[0] = 'b';
        std::map<std::string, std::string> dirty = {
            {"127.0.0.1", "good.example.com bad_host! 1.2.3.4 evil#cmt " + long_host},
            {"999.1.1.1", "whatever.example.com"},        // 非法 IP
            {"010.1.1.1", "zero.example.com"},            // 前导零非法
            {"2001:db8::5", "v6.example.com"},            // 合法 IPv6
            {"1.2.3.4.5", "notanip.example.com"},         // 也不是合法 IP
        };
        const fs::path hostf = dir / "sanitize.bin";
        write_all(hostf, "127.0.0.1 localhost\n");
        set_hosts_env(hostf);
        const svpn::Paths paths = temp_paths(dir);
        const svpn::HostsResult r = svpn::apply_hosts(dirty, paths);
        std::printf("ok=%d entries=%d rejected=%zu reason=%s\n", static_cast<int>(r.ok), r.entries,
                    r.rejected.size(), r.reason.c_str());
        for (const auto& x : r.rejected) std::printf("  rejected: %s\n", x.c_str());
        check(r.ok, "sanitize: 仍有合法条目时 ok");
        check(r.entries == 2, "sanitize: 只留下 127.0.0.1 / 2001:db8::5 两个 IP");
        check(r.rejected.size() == 6, "sanitize: 6 个脏条目被拒");
        std::printf("--- 写出的文件 ---\n%s---\n", read_all(hostf).c_str());
    }
    {
        // 全部非法 → ok=false + Python 同款文案
        std::map<std::string, std::string> all_bad = {{"999.1.1.1", "x.example.com"},
                                                      {"127.0.0.1", "bad host!"}};
        const svpn::HostsResult r = svpn::apply_hosts(all_bad, temp_paths(dir));
        check(!r.ok, "sanitize: 全非法 → ok=false");
        check(r.reason == "没有合法的 hosts 条目（IP/域名校验未通过）", "sanitize: 文案与 Python 一致");
        std::printf("reason=%s\n", r.reason.c_str());
    }
    {
        // 空文件 → apply 后应能写块；无托管块时 clear 不该破坏内容
        const fs::path hostf = dir / "empty.bin";
        write_all(hostf, "");
        set_hosts_env(hostf);
        const svpn::HostsResult r = svpn::apply_hosts(entries, temp_paths(dir));
        check(r.ok, "空 hosts: apply ok");
        std::printf("--- 空文件 apply 后 ---\n%s---\n", read_all(hostf).c_str());
        check(svpn::clear_hosts(temp_paths(dir)).ok, "空 hosts: clear ok");
        std::printf("空文件 clear 后长度=%zu\n", read_all(hostf).size());
    }

    std::puts("\n=== PAC（三种 mode + 恶意后缀）===");
    {
        const std::vector<std::string> ps = {"b.example", "a.example", "b.example"};
        pac_case("pac_whitelist.js", dir,
                 svpn::build_pac("127.0.0.1", 7899, ps, {}, true, "whitelist"));
        pac_case("pac_all.js", dir, svpn::build_pac("127.0.0.1", 7899, ps, {}, true, "all"));
        pac_case("pac_blacklist.js", dir,
                 svpn::build_pac("127.0.0.1", 7899, ps, {}, true, "blacklist"));
        pac_case("pac_localnet_off.js", dir,
                 svpn::build_pac("127.0.0.1", 7899, ps, {"m.example", "z.example"}, false,
                                 "whitelist"));
        // 默认参数：空 proxy_suffixes → 内置 Steam 名单；blacklist 空 direct → 内置国内名单
        pac_case("pac_default.js", dir, svpn::build_pac("127.0.0.1", 7899, {}, {}, true, "whitelist"));
        // 恶意后缀：引号 / 换行 / 反斜杠 / 中文 / 控制字符
        const std::vector<std::string> evil = {"a\"b.example", "line\nbreak.example",
                                               "back\\slash.example", "中文.example",
                                               std::string("ctl\x01.example")};
        pac_case("pac_evil.js", dir, svpn::build_pac("10.1.2.3", 18080, evil, evil, true, "all"));
    }

    std::puts("\n=== WinINET（只读 + 备份，绝不改本机代理）===");
    {
        const svpn::WininetState st = svpn::read_wininet();
        std::printf("ok=%d enable=%d server=[%s] override=[%s] autoconfig=[%s] reason=%s\n",
                    static_cast<int>(st.ok), static_cast<int>(st.enable), st.server.c_str(),
                    st.override_list.c_str(), st.autoconfig.c_str(), st.reason.c_str());
        check(st.ok, "read_wininet ok");
        const svpn::Paths paths = temp_paths(dir);
        check(svpn::backup_wininet(paths), "backup_wininet ok");
        check(fs::exists(paths.wininet_backup), "wininet-backup.json 已生成");
        std::printf("--- wininet-backup.json ---\n%s\n---\n", read_all(paths.wininet_backup).c_str());

        // 没有备份时必须拒绝，而不是清空用户设置
        svpn::Paths empty = temp_paths(dir / "nobackup");
        const svpn::WininetState r = svpn::restore_wininet(empty);
        check(!r.ok && r.reason == "no backup", "restore_wininet 无备份 → ok=false / reason=no backup");
        std::printf("reason=%s\n", r.reason.c_str());
    }

    std::puts("\n=== 提权 ===");
    std::printf("is_admin=%d\n", static_cast<int>(svpn::is_admin()));
    std::puts("（不调用 elevate：会弹 UAC）");

    std::printf("\n%s 失败 %d 项\n", g_fail == 0 ? "全部通过，" : "有失败，", g_fail);
    return g_fail == 0 ? 0 : 1;
}
