// T1：parse_client_hello_sni（纯字节解析，攻击面）单元测试。
// 编译：
//   g++ -std=c++20 -O2 -static -I src -o tests/test_sni_parse.exe tests/test_sni_parse.cpp \
//       src/tls.cpp src/net.cpp src/util.cpp src/json.cpp -lws2_32 -lcrypt32 -lsecur32
//
// 关键设计：所有样本都放进「末尾贴着 PAGE_NOACCESS 的守卫页」的缓冲区里，
// 解析器只要多读一个字节就立刻触发访问违例 —— 用 SetUnhandledExceptionFilter
// 把它翻译成 FAIL + 退出码 3，这样「越界」不再是靠运气才崩。
#include "../src/svpn.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
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
    std::fflush(stdout);
}

// ---------------------------------------------------------------- 守卫页缓冲区
struct GuardedBuf {
    unsigned char* base{nullptr};
    std::size_t page{0};

    bool init() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page = si.dwPageSize;
        base = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (base == nullptr) return false;
        DWORD old = 0;
        return VirtualProtect(base + page, page, PAGE_NOACCESS, &old) != 0;
    }
    // 把 n 字节放到页尾，第 n+1 个字节起就是不可访问区
    unsigned char* place(std::size_t n) const { return base + page - n; }
    void release() {
        if (base != nullptr) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
    }
};

LONG WINAPI on_access_violation(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const auto& va = ep->ExceptionRecord->ExceptionInformation;
        std::printf("\nFAIL  解析器越界访问（地址 0x%llx，%s）\n",
                    static_cast<unsigned long long>(va[1]),
                    va[0] == 0 ? "read" : "write");
        std::fflush(stdout);
        std::_Exit(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- hex → bytes
std::vector<unsigned char> from_hex(const char* hex) {
    std::vector<unsigned char> out;
    int hi = -1;
    for (const char* p = hex; *p != '\0'; ++p) {
        int v = -1;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;   // 允许空白/换行分隔
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<unsigned char>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

// Python 3.13 真实抓包（ssl.MemoryBIO + wrap_bio，server_hostname=store.steampowered.com）
// 单条 TLS record，record_len=512，总长 517 —— 不分片，正好适合逐字节截断。
const char* const kHelloHex =
    "1603010200010001fc0303ecf10c5b45a90464350a84b54e81036fdfcf929611901c53cb46c263abc623122000448d3d"
    "422dc02acd1a9f4f440b4ee1afefb842d8926b71602992d3d3b99c030022130213031301c02cc030c02bc02fcca9cca8"
    "c024c028c023c027009f009e006b006701000191ff010001000000001b001900001673746f72652e737465616d706f77"
    "657265642e636f6d000b000403000102000a00040002001d002300000016000000170000000d00360034090509060904"
    "04030503060308070808081a081b081c0809080a080b080408050806040105010601030303010302040205020602002b"
    "00050403040303002d00020101003300260024001d002006bdeb8976e180b43b8cd7308a6ac60e6199971fc2865c77b6"
    "aab401eae87365001500da00000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000"
    ;

const char* const kExpect = "store.steampowered.com";

// 一次性把样本拷进守卫页再解析；返回是否解析成功，sni 通过出参带出。
bool parse_guarded(GuardedBuf& gb, const std::vector<unsigned char>& bytes, std::string* sni) {
    if (bytes.empty() || bytes.size() > gb.page) return false;
    unsigned char* p = gb.place(bytes.size());
    std::memcpy(p, bytes.data(), bytes.size());
    return parse_client_hello_sni(p, bytes.size(), sni);
}

// ---------------------------------------------------------------- 1. 合法样本
void test_real_hello(GuardedBuf& gb) {
    std::printf("\n=== 1. 真实 ClientHello ===\n");
    const std::vector<unsigned char> hello = from_hex(kHelloHex);
    std::printf("  样本长度 = %zu 字节，record_len = %u\n", hello.size(),
                static_cast<unsigned>(hello.size() >= 5 ? ((hello[3] << 8) | hello[4]) : 0));
    check(hello.size() == 517, "样本长度 517（内嵌 hex 未截断）");
    check(!hello.empty() && hello[0] == 0x16 && hello[1] == 0x03 && hello[5] == 0x01,
          "样本头 = 16 03 01 / handshake=client_hello");

    std::string sni;
    const bool ok = parse_guarded(gb, hello, &sni);
    std::printf("  parsed ok=%d sni=[%s]\n", ok ? 1 : 0, sni.c_str());
    check(ok, "真实样本解析成功");
    check(sni == kExpect, std::string("SNI == ") + kExpect);

    // sni 出参可空：解析器不应解引用它
    std::string dummy;
    check(parse_guarded(gb, hello, &dummy) && dummy == kExpect, "重复解析结果稳定");
    {
        unsigned char* p = gb.place(hello.size());
        std::memcpy(p, hello.data(), hello.size());
        check(parse_client_hello_sni(p, hello.size(), nullptr), "sni=nullptr 时不崩且返回 true");
    }

    // 大写主机名 → 输出小写
    {
        std::vector<unsigned char> up = hello;
        std::string upper = "STORE.STEAMPOWERED.COM";
        check(upper.size() == std::strlen(kExpect), "大写样本与被替换串等长");
        const auto it = std::search(up.begin(), up.end(), kExpect, kExpect + std::strlen(kExpect));
        check(it != up.end(), "样本里能定位到 SNI 字面量");
        if (it != up.end()) {
            std::memcpy(&*it, upper.data(), upper.size());
            std::string s2;
            const bool ok2 = parse_guarded(gb, up, &s2);
            std::printf("  大写样本 → ok=%d sni=[%s]\n", ok2 ? 1 : 0, s2.c_str());
            check(ok2 && s2 == kExpect, "大写 SNI 归一化成小写");
        }
    }

    // 末尾带点 → 去掉
    {
        std::vector<unsigned char> dot = hello;
        const auto it = std::search(dot.begin(), dot.end(), kExpect, kExpect + std::strlen(kExpect));
        if (it != dot.end()) {
            *(it + std::strlen(kExpect) - 1) = '.';   // "...co." 而不是 "...com"
            std::string s3;
            const bool ok3 = parse_guarded(gb, dot, &s3);
            std::printf("  末尾带点 → ok=%d sni=[%s]\n", ok3 ? 1 : 0, s3.c_str());
            check(ok3 && s3 == "store.steampowered.co", "SNI 末尾的 . 被去掉");
        }
    }
}

// ---------------------------------------------------------------- 2. 逐字节截断
void test_truncation(GuardedBuf& gb) {
    std::printf("\n=== 2. 截断：长度 1..N-1 必须全部 false ===\n");
    const std::vector<unsigned char> hello = from_hex(kHelloHex);
    int bad = 0;
    std::size_t first_bad = 0;
    std::string first_sni;
    for (std::size_t len = 1; len < hello.size(); ++len) {
        std::vector<unsigned char> prefix(hello.begin(), hello.begin() + static_cast<long>(len));
        std::string sni;
        sni.clear();
        if (parse_guarded(gb, prefix, &sni)) {
            if (bad == 0) {
                first_bad = len;
                first_sni = sni;
            }
            ++bad;
        }
    }
    if (bad != 0) {
        std::printf("  首个误判：len=%zu → sni=[%s]\n", first_bad, first_sni.c_str());
    }
    check(bad == 0, "截断 1..516 全部返回 false（共 516 例）");
}

// ---------------------------------------------------------------- 3. 随机模糊
void test_fuzz(GuardedBuf& gb) {
    std::printf("\n=== 3. 随机模糊：mt19937 seed=20261025，500 段 ===\n");
    std::mt19937 rng(20261025u);
    std::uniform_int_distribution<int> len_dist(1, 2048);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    int trues = 0;
    int bad_name = 0;
    std::size_t max_len = 0;
    for (int i = 0; i < 500; ++i) {
        const std::size_t len = static_cast<std::size_t>(len_dist(rng));
        if (len > max_len) max_len = len;
        std::vector<unsigned char> bytes(len);
        for (std::size_t k = 0; k < len; ++k) bytes[k] = static_cast<unsigned char>(byte_dist(rng));
        std::string sni;
        if (parse_guarded(gb, bytes, &sni)) {
            ++trues;
            // 随机字节理论上几乎不可能命中，但既然返回 true 就必须给出合法域名
            bool sane = !sni.empty() && sni.size() <= 253;
            for (char ch : sni) {
                const unsigned char u = static_cast<unsigned char>(ch);
                if (u < 0x21 || u > 0x7e || (ch >= 'A' && ch <= 'Z')) sane = false;
            }
            if (!sane) {
                ++bad_name;
                std::printf("  样本 #%d len=%zu 返回 true 但 sni 不合法：[%s]\n", i, len, sni.c_str());
            }
        }
    }
    std::printf("  500 段随机字节：解析 true %d 次，最大长度 %zu，无崩溃\n", trues, max_len);
    check(bad_name == 0, "随机样本若返回 true，sni 必为合法小写域名");
    check(max_len > 2000 && max_len <= 2048,
          "长度覆盖到 1..2048 区间（实测最大 " + std::to_string(max_len) + "）");
}

// ---------------------------------------------------------------- 4. 畸形样本
void test_malformed(GuardedBuf& gb) {
    std::printf("\n=== 4. 畸形样本 ===\n");

    // 4.1 全 0
    {
        std::vector<unsigned char> zeros(2048, 0x00);
        std::string sni;
        const bool ok = parse_guarded(gb, zeros, &sni);
        std::printf("  全 0(2048B) → ok=%d\n", ok ? 1 : 0);
        check(!ok, "全 0 → false");
    }

    // 4.2 16 03 01 之后全是垃圾
    {
        std::vector<unsigned char> junk = {0x16, 0x03, 0x01};
        std::mt19937 rng(20261025u);
        std::uniform_int_distribution<int> d(0, 255);
        for (int i = 0; i < 509; ++i) junk.push_back(static_cast<unsigned char>(d(rng)));
        std::string sni;
        const bool ok = parse_guarded(gb, junk, &sni);
        std::printf("  16 03 01 + 509B 垃圾 → ok=%d\n", ok ? 1 : 0);
        check(!ok, "16 03 01 + 垃圾 → false");
    }

    // 4.3 record_len 声称 65535，实际只有 10 字节
    {
        std::vector<unsigned char> claim = {0x16, 0x03, 0x01, 0xFF, 0xFF,
                                            0x01, 0x00, 0x00, 0x05, 0x03};
        std::string sni;
        const bool ok = parse_guarded(gb, claim, &sni);
        std::printf("  record_len=65535 但只有 10B → ok=%d\n", ok ? 1 : 0);
        check(!ok, "record_len 超出实到长度 → false（分片语义）");
    }

    // 4.4 record_len < 4
    {
        const std::vector<unsigned char> tiny = {0x16, 0x03, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00};
        std::string sni;
        check(!parse_guarded(gb, tiny, &sni), "record_len=3（< 4）→ false");
    }

    // 4.5 非 handshake 记录类型 / 非 TLS 版本号
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        std::string sni;
        std::vector<unsigned char> appdata = hello;
        appdata[0] = 0x17;   // application_data
        check(!parse_guarded(gb, appdata, &sni), "record type=0x17 → false");
        std::vector<unsigned char> ssl3 = hello;
        ssl3[1] = 0x02;      // 非 0x03
        check(!parse_guarded(gb, ssl3, &sni), "record buf[1] != 0x03 → false");
        std::vector<unsigned char> nothello = hello;
        nothello[5] = 0x02;  // server_hello
        check(!parse_guarded(gb, nothello, &sni), "handshake type=0x02（server_hello）→ false");
    }

    // 4.6 handshake body 长度声称超过 record
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        hello[6] = 0x00;
        hello[7] = 0xFF;
        hello[8] = 0xFF;     // body = 65535 > record_len
        std::string sni;
        check(!parse_guarded(gb, hello, &sni), "handshake body 长度超出 record → false");
    }

    // 4.7 扩展总长声称超过 body（扩展段被截断）
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        // 定位扩展总长（0x0191 = 401）而不是硬算偏移
        bool patched = false;
        for (std::size_t i = 5; i + 3 < hello.size(); ++i) {
            if (hello[i] == 0x01 && hello[i + 1] == 0x91 && i + 2 < hello.size() &&
                hello[i + 2] == 0xFF && hello[i + 3] == 0x01) {
                hello[i] = 0xFF;
                hello[i + 1] = 0xFF;
                patched = true;
                break;
            }
        }
        std::string sni;
        check(patched, "定位到扩展段总长字段");
        if (patched) check(!parse_guarded(gb, hello, &sni), "扩展总长 0xFFFF 越界 → false");
    }

    // 4.8 没有 SNI 扩展（把 server_name 扩展类型改成别的）
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        bool patched = false;
        for (std::size_t i = 5; i + 1 < hello.size(); ++i) {
            if (hello[i] == 0x00 && hello[i + 1] == 0x00 && hello[i + 2] == 0x00 &&
                hello[i + 3] == 0x1b) {
                hello[i + 1] = 0x12;   // 0x0012 = signed_certificate_timestamp，不再是 server_name
                patched = true;
                break;
            }
        }
        std::string sni;
        check(patched, "定位到 server_name 扩展类型字段");
        if (patched) {
            const bool ok = parse_guarded(gb, hello, &sni);
            std::printf("  无 server_name 扩展 → ok=%d\n", ok ? 1 : 0);
            check(!ok, "没有 SNI 扩展 → false");
        }
    }

    // 4.9 宿主名长度 1 但内容为空（name_len=0）
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        const auto it = std::search(hello.begin(), hello.end(), kExpect,
                                    kExpect + std::strlen(kExpect));
        std::string sni;
        if (it != hello.end()) {
            // 把 name_len(0x0016) 改成 0x0000：前面 3 字节是 00 00 16
            std::size_t off = static_cast<std::size_t>(it - hello.begin());
            hello[off - 2] = 0x00;
            hello[off - 1] = 0x00;
            check(!parse_guarded(gb, hello, &sni), "name_len=0 → false");
        } else {
            check(false, "定位 SNI 内容失败");
        }
    }

    // 4.10 宿主名含控制字符
    {
        std::vector<unsigned char> hello = from_hex(kHelloHex);
        const auto it = std::search(hello.begin(), hello.end(), kExpect,
                                    kExpect + std::strlen(kExpect));
        std::string sni;
        if (it != hello.end()) {
            *(it + 3) = 0x07;   // BEL
            check(!parse_guarded(gb, hello, &sni), "SNI 含控制字符 → false");
        } else {
            check(false, "定位 SNI 内容失败");
        }
    }
}

}  // namespace

int main() {
    console_utf8();
    SetUnhandledExceptionFilter(on_access_violation);

    std::printf("svpn T1 自测：parse_client_hello_sni\n");

    GuardedBuf gb;
    if (!gb.init()) {
        std::printf("FATAL 无法申请守卫页内存\n");
        return 2;
    }
    const std::size_t page = gb.page;
    test_real_hello(gb);
    test_truncation(gb);
    test_fuzz(gb);
    test_malformed(gb);
    gb.release();
    std::printf("  （守卫页大小 = %zu 字节；任何越界读都会被捕获）\n", page);

    std::printf("\n%s PASS %d / FAIL %d（共 %d 项）\n", g_fail ? "[!]" : "[+]", g_pass, g_fail,
                g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
