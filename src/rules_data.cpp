// 自动生成，勿手改：由 svpn(Python) 导出，保证两版规则完全一致。
// 生成源：svpn/rules.py + svpn/cdn.py + svpn/cli.py::DEFAULT_CONFIG
#include "svpn.hpp"

namespace svpn {

const char* const kSteamSuffixes[] = {
    "steampowered.com", "steamcommunity.com", "steamstatic.com", "steamcontent.com",
    "steamusercontent.com", "steam-chat.com", "steamgames.com", "steamdeck.com",
    "steam.tv", "s.team", "steamserver.net", "steamconnecttest.com",
    "steamcdn-a.akamaihd.net", "steamcommunity-a.akamaihd.net", "steamusercontent-a.akamaihd.net", "valvesoftware.com",
    "valve.net", "valve.steamstatic.com", "underlords.com", "dota2.com",
    "counter-strike.net", "csgo.com", "tf2.com", "artifact.com",
    "steampowered.com.edgesuite.net", "akamai.steamstatic.com", "cdn.akamai.steamstatic.com", "community.akamai.steamstatic.com",
    "shared.akamai.steamstatic.com", "store.akamai.steamstatic.com", "client-update.akamai.steamstatic.com", "media.steampowered.com",
    "api.steampowered.com", "login.steampowered.com", "help.steampowered.com", "checkout.steampowered.com",
    "store.steampowered.com", "partner.steampowered.com", "partner.steamgames.com", "support.steampowered.com",
};
const size_t kSteamSuffixes_count = sizeof(kSteamSuffixes) / sizeof(kSteamSuffixes[0]);

const char* const kCnDirectSuffixes[] = {
    "cn", "qq.com", "gtimg.com", "myqcloud.com",
    "tencent.com", "baidu.com", "bdstatic.com", "bilibili.com",
    "hdslb.com", "bilivideo.com", "taobao.com", "tmall.com",
    "alicdn.com", "aliyun.com", "alipay.com", "jd.com",
    "360buyimg.com", "163.com", "126.net", "sina.com.cn",
    "weibo.com", "zhihu.com", "douyin.com", "byteimg.com",
    "kuaishou.com", "xiaomi.com", "mi.com", "huawei.com",
    "hinova.com", "meituan.com", "pinduoduo.com", "douban.com",
    "csdn.net", "gitee.com", "jianshu.com", "microsoft.com",
    "windowsupdate.com", "msftconnecttest.com", "apple.com", "icloud.com",
    "mzstatic.com",
};
const size_t kCnDirectSuffixes_count = sizeof(kCnDirectSuffixes) / sizeof(kCnDirectSuffixes[0]);

const char* const kSteamCidrs[] = {
    "23.45.64.0/18", "45.121.184.0/24", "103.10.124.0/23", "103.28.54.0/23",
    "146.66.152.0/21", "155.133.224.0/19", "162.254.192.0/18", "185.25.182.0/23",
    "192.69.96.0/22", "205.196.6.0/24", "208.64.200.0/22", "209.197.0.0/19",
};
const size_t kSteamCidrs_count = sizeof(kSteamCidrs) / sizeof(kSteamCidrs[0]);

const char* const kCdnDomains[] = {
    "steamcdn-a.akamaihd.net", "cdn.akamai.steamstatic.com", "client-update.akamai.steamstatic.com", "shared.akamai.steamstatic.com",
    "store.akamai.steamstatic.com", "community.akamai.steamstatic.com", "media.steampowered.com", "avatars.steamstatic.com",
};
const size_t kCdnDomains_count = sizeof(kCdnDomains) / sizeof(kCdnDomains[0]);

const char* const kDefaultBypass = "<local>;localhost;127.*;10.*;172.16.*;172.17.*;172.18.*;172.19.*;172.20.*;172.21.*;172.22.*;172.23.*;172.24.*;172.25.*;172.26.*;172.27.*;172.28.*;172.29.*;172.30.*;172.31.*;192.168.*;*.cn";

}  // namespace svpn
