# T1-tests 报告：svpn C++ 单测（config + SNI 解析）

- 日期：2026-10-25
- 只改 `tests/`，未触碰 `src/` 与 `tests/smoke_cpp.py`
- 编译器：`D:\LE\AI\tools\mingw64\bin\g++.exe`（16.2.0），一律 `-static`

## 结论

| 文件 | 结果 | 退出码 |
|---|---|---|
| `tests/test_config.cpp` | **PASS 178 / FAIL 0** | 0 |
| `tests/test_sni_parse.cpp` | **PASS 27 / FAIL 0** | 0 |
| `tests/test_cli.py`（按 V1 清单，含 ⑫ P1） | **PASS 113 / FAIL 0 / SKIP 0** | 0 |
| 被测二进制 | `svpn.exe` md5 `e634fe15067808ccf2ba0dad30d55d97`（10:46:27，V1 定的发布基线） | |

C++ 两组与任务给出的判定矩阵 **完全一致**，未发现疑似 bug。
`test_cli.py` 113 条全绿，包含 V1 挖到的唯一 P1（⑫ 失败的 up 抢删在跑实例的 pid），见文末第 III 节。

## 文件清单

**修改**（已存在，按「先跑一遍、只补缺的断言」处理）

- `tests/test_config.cpp`
  - 新增 `g_pass` 计数，结尾改为 `PASS n / FAIL m（共 k 项）`，失败返回 1
  - 第 2 组补：`listen_port` 的 `null` / `"abc"` / `70000`、`http_port:-1`、`mode:"xxx"`、
    `connect_timeout:-5` / `9999`、`cdn.refresh_hours:"abc"`、`browser.apply_system_proxy:"yes"`（+9 项）
  - 第 3 组补：`{"_note":"x","bogus":1,"mode":"all"}` 里 mode 仍生效、`bogus`/`_note` 不进 `config_to_json`；
    `direct_suffixes:"b.com, c.com"` → 2 项并逐项校验 trim（+5 项）
  - 第 4 组改：`Case` 结构加显式 `all` 列（原来是用 `local` 白名单「猜」出 all 的预期，容易自证），
    新增 `127.0.0.1` 行
  - 第 5 组补：`match_suffix("b.com","b.com")` / `("a.b.com","b.com")` / `("xb.com","b.com")`、
    `in_cidr("23.45.64.1","23.45.64.0/18")`、`is_ip("1.2.3")` / `("::1")` / `("[::1]")`（+7 项）

**新建**

- `tests/test_sni_parse.cpp`
- 产物（保留）：`tests/test_config.exe`、`tests/test_sni_parse.exe`

## 编译命令与结果

```bash
cd "D:/LE/AI/wbwrk/svpn-cpp"

D:/LE/AI/tools/mingw64/bin/g++.exe -std=c++20 -O2 -static -I src -o tests/test_config.exe \
  tests/test_config.cpp src/config.cpp src/rules.cpp src/json.cpp src/util.cpp src/rules_data.cpp -lws2_32
# exit 0，无警告

D:/LE/AI/tools/mingw64/bin/g++.exe -std=c++20 -O2 -static -I src -o tests/test_sni_parse.exe \
  tests/test_sni_parse.cpp src/tls.cpp src/net.cpp src/util.cpp src/json.cpp \
  -lws2_32 -lcrypt32 -lsecur32
# exit 0，无警告
```

两个 exe 都是 `-static`，直接双击/命令行跑都不缺 DLL（退出码不是 127）。

---

# 文件 1：`tests/test_config.cpp`

## 实际输出（原文，完整 234 行）

```
svpn A1 自测：config + rules

=== 1. merge_config(nullptr) 逐字段 ===
  listen_host      = 127.0.0.1
  listen_port      = 7899
  http_port        = 7900
  mode             = whitelist
  connect_timeout  = 12
  auto_node        = true
  upstream         = socks5 NODE_HOST:1080 user=[] pass=[]
  auth             = user=[] pass=[]
  browser          = apply=true use_pac=false bypass.len=187
  cdn              = enabled=false refresh=6 sni_port=443 hosts=8
  proxy_hosts      = 0
  direct_hosts     = 0
ok    listen_host=127.0.0.1
ok    listen_port=7899
ok    http_port=7900
ok    mode=whitelist
ok    connect_timeout=12
ok    auto_node=true
ok    upstream.kind=socks5
ok    upstream.host=NODE_HOST
ok    upstream.port=1080
ok    upstream 凭据为空
ok    auth 凭据为空
ok    browser.apply_system_proxy=true
ok    browser.use_pac=false
ok    browser.bypass == kDefaultBypass
ok    bypass 含 172.31.*
ok    bypass 含 *.cn
ok    cdn.enabled=false
ok    cdn.refresh_hours=6
ok    cdn.sni_port=443
ok    cdn.hosts == kCdnDomains
ok    proxy/direct_hosts 为空
ok    高级键默认不出现
ok    merge_config(nullptr) == default_config
ok    config_to_json → merge 往返不漂移
  config_note() len = 1019

=== 2. 脏输入必须回落（不崩）===
  listen_host      = 127.0.0.1
  listen_port      = 7899
  http_port        = 7900
  mode             = whitelist
  connect_timeout  = 12
  auto_node        = true
  upstream         = socks5 NODE_HOST:1080 user=[] pass=[]
  auth             = user=[] pass=[]
  browser          = apply=true use_pac=false bypass.len=187
  cdn              = enabled=false refresh=6 sni_port=443 hosts=8
  proxy_hosts      = 0
  direct_hosts     = 0
ok    listen_port:null → 7899
ok    http_port:"" → 7900
ok    mode:xxx → whitelist
ok    connect_timeout:-1 → 12
ok    cdn.refresh_hours:"abc" → 6
ok    listen_host:null → 127.0.0.1
ok    auto_node:"maybe" → true
ok    http_port:0 → 0（合法）
ok    http_port:70000 → 7900
ok    listen_port:"8080" → 8080
ok    connect_timeout:999 → 12（>300）
ok    connect_timeout:"8.5" → 8
ok    connect_timeout:0 → 12
ok    mode 大小写/空白归一
ok    auto_node:"off" → false
ok    auto_node:"yes" → true
ok    browser.apply_system_proxy:"no" → false
ok    browser.bypass:"" → 空串（Python 语义）
ok    browser.use_pac:"on" → true
ok    upstream 非对象 → 全默认
ok    cdn 非对象 → 全默认
ok    auth 非对象 → 全默认
ok    upstream.kind:quic → socks5
ok    upstream.port:0 → 1080
ok    listen_port:null → 7899
ok    listen_port:"abc" → 7899
ok    listen_port:70000 → 7899（越界）
ok    http_port:-1 → 7900
ok    mode:"xxx" → whitelist
ok    connect_timeout:-5 → 12
ok    connect_timeout:9999 → 12
ok    cdn.refresh_hours:"abc" → 6
ok    browser.apply_system_proxy:"yes" → true

=== 3. 未知键/私有键丢弃 + 名单归一 ===
ok    bogus 被丢弃
ok    _note 被丢弃
ok    proxy_suffixes 生效
ok    混入 _note/bogus 后 mode 仍生效
ok    _note/bogus 不随 config_to_json 出现
ok    proxy_suffixes 数组 → 1 项
ok    direct_suffixes 逗号字符串 → 2 项
ok    direct_suffixes 逐项 trim 正确
ok    proxy_hosts 逗号串 → 2 项
ok    名单保序、仅 trim（Python 不改大小写）
ok    空格/逗号混合 + 去重 → 3 项
ok    空项被丢掉
ok    数组里的 null/数组/对象被丢弃，数字转文本
ok    数字项 → "42"
ok    cdn.hosts 字符串 → 2 项
ok    cdn.hosts 空数组 → 空（不回落）
ok    cdn.hosts 非名单 → 内置

=== 4. Router.decide ===
  store.steampowered.com   whitelist=upstream blacklist=upstream all=upstream
ok    whitelist store.steampowered.com
ok    blacklist store.steampowered.com
ok    all store.steampowered.com
  www.baidu.com            whitelist=direct   blacklist=direct   all=upstream
ok    whitelist www.baidu.com
ok    blacklist www.baidu.com
ok    all www.baidu.com
  notsteampowered.com      whitelist=direct   blacklist=upstream all=upstream
ok    whitelist notsteampowered.com
ok    blacklist notste10:27:27 WARN    config.json 解析失败: expected string，使用默认配置
10:27:27 WARN    config.json 解析失败: unexpected end，使用默认配置
ampowered.com
ok    all notsteampowered.com
  127.0.0.1                whitelist=direct   blacklist=direct   all=direct
ok    whitelist 127.0.0.1
ok    blacklist 127.0.0.1
ok    all 127.0.0.1
  192.168.1.1              whitelist=direct   blacklist=direct   all=direct
ok    whitelist 192.168.1.1
ok    blacklist 192.168.1.1
ok    all 192.168.1.1
  23.45.64.1               whitelist=upstream blacklist=upstream all=upstream
ok    whitelist 23.45.64.1
ok    blacklist 23.45.64.1
ok    all 23.45.64.1
  www.google.com           whitelist=direct   blacklist=upstream all=upstream
ok    whitelist www.google.com
ok    blacklist www.google.com
ok    all www.google.com
  localhost                whitelist=direct   blacklist=direct   all=direct
ok    whitelist localhost
ok    blacklist localhost
ok    all localhost
  foo.localhost            whitelist=direct   blacklist=direct   all=direct
ok    whitelist foo.localhost
ok    blacklist foo.localhost
ok    all foo.localhost
  steamloopback.host       whitelist=direct   blacklist=direct   all=direct
ok    whitelist steamloopback.host
ok    blacklist steamloopback.host
ok    all steamloopback.host
  172.17.0.1               whitelist=direct   blacklist=direct   all=direct
ok    whitelist 172.17.0.1
ok    blacklist 172.17.0.1
ok    all 172.17.0.1
  8.8.8.8                  whitelist=direct   blacklist=upstream all=upstream
ok    whitelist 8.8.8.8
ok    blacklist 8.8.8.8
ok    all 8.8.8.8
  (empty)                  whitelist=direct   blacklist=upstream all=upstream
ok    whitelist (empty)
ok    blacklist (empty)
ok    all (empty)
ok    direct_hosts 压过 Steam 后缀
ok    名单匹配前做 normalize
ok    proxy_hosts 压过 whitelist 兜底
ok    自定义 proxy_suffixes 生效
ok    自定义后缀替换内置名单（Python 语义）

=== 5. 基础规则 ===
ok    normalize_host 小写/去点/去空白
ok    normalize_host 去方括号
ok    normalize_host 全点 → 空
ok    子域匹配
ok    等值匹配
ok    大小写 + 前导点
ok    notsteampowered.com 不匹配
ok    空 host 不匹配
ok    顶级后缀 cn 匹配 .cn
ok    acn 不匹配 cn
ok    v4
ok    v4 全零
ok    v4 越界
ok    v4 三节
ok    v4 五节
ok    v4 前导零（Python 3.9+ 拒绝）
ok    host:port 不是 IP
ok    v6 ::1
ok    v6 带方括号
ok    v6 压缩
ok    v6 内嵌 v4
ok    v6 链路本地
ok    v6 三个冒号非法
ok    v6 两个 ::
ok    v6 带 zone 非法
ok    v6 九组非法
ok    空串不是 IP
ok    v4 段命中
ok    v4 段边界外
ok    v4 段广播地址仍在段内
ok    无 / 按单主机
ok    无 / 且不等
ok    /0 全命中
ok    v6 /128
ok    v6 /32
ok    v4 地址对 v6 段不匹配
ok    非法前缀长度
ok    非法前缀长度
ok    非法段
ok    match_suffix 等值 b.com/b.com
ok    match_suffix 子域 a.b.com/b.com
ok    match_suffix xb.com/b.com 不匹配
ok    in_cidr 23.45.64.1 ∈ 23.45.64.0/18
ok    is_ip 1.2.3 false
ok    is_ip ::1 true
ok    is_ip [::1] true

=== 6. load/save（BOM、坏 JSON、原子替换）===
ok    文件不存在 → 默认，无错误
ok    BOM 文件解析无错误
ok    BOM 文件读到的键生效
ok    save_config 成功
ok    落盘无 BOM
ok    落盘是 2 空格缩进
ok    落盘不含 _ 开头的键
ok    save → load 往返一致
ok    高级键写盘后仍生效
ok    临时文件已被替换掉
ok    坏 JSON 设置了 err
ok    坏 JSON → 全默认
  err = config.json 解析失败: expected string
ok    空文件 → err + 默认
ok    顶层数组 → 默认（不报错，Python 同）

[+] PASS 178 / FAIL 0（共 178 项）
```

### 第 1 组：默认值（逐字段打印 vs `config.example.json`）

```
  listen_host      = 127.0.0.1
  listen_port      = 7899
  http_port        = 7900
  mode             = whitelist
  connect_timeout  = 12
  auto_node        = true
  upstream         = socks5 NODE_HOST:1080 user=[] pass=[]
  auth             = user=[] pass=[]
  browser          = apply=true use_pac=false bypass.len=187
  cdn              = enabled=false refresh=6 sni_port=443 hosts=8
  proxy_hosts      = 0
  direct_hosts     = 0
```

上面是程序打印的运行期值；`config_to_json()` 的键集/键名/类型与 `config.example.json` 逐键对上
（`upstream.username` / `auth.username` 这类同义名由 config.cpp 映射，未漂移）。
另外断言了 `merge_config(nullptr)` 与 `default_config()` 的 `dump(0)` 逐字节相等，
以及 `config_to_json → merge_config` 往返不漂移。

`connect_timeout` 单位是**秒**（12），与 socket 层的毫秒超时（`kDefaultTimeoutMs=12000`）
不是同一个量纲，这一点在断言里按秒校验，没有把它当毫秒。

### 第 2 组：脏输入必须回落（不崩）

| 输入 | 实际结果 |
|---|---|
| `listen_port:null` / `"abc"` / `70000` | 7899 |
| `listen_port:"8080"` | 8080（数字串被接受，与 Python 一致） |
| `http_port:-1` / `""` / `70000` | 7900 |
| `http_port:0` | **0**（合法：不启 HTTP 入口，没被抬回 7900） |
| `mode:"xxx"` | whitelist |
| `mode:"  BLACKLIST "` | blacklist（大小写/空白归一） |
| `connect_timeout:-5` / `9999` / `0` | 12 |
| `connect_timeout:"8.5"` | 8 |
| `cdn.refresh_hours:"abc"` | 6 |
| `browser.apply_system_proxy:"yes"` / `"no"` / `"on"` | true / false / true |
| `auto_node:"off"` / `"yes"` | false / true |
| `upstream:"not-an-object"` / `cdn:[]` / `auth:5` | 全默认（`upstream.host` 仍是 `NODE_HOST`） |
| `upstream.kind:"quic"` / `upstream.port:0` | socks5 / 1080 |

注意 `upstream.host == "NODE_HOST"` 等价于「没配节点」，测试里把它当默认占位符断言，
不是当一个真实主机名。

### 第 3~4 组：未知键 / `_` 前缀丢弃 + 高级键放行

- `{"_note":"x","bogus":1,"mode":"all"}` → `mode=all` 生效；`config_to_json()` 里既无 `bogus` 也无 `_note`
- `{"proxy_suffixes":["a.com"],"direct_suffixes":"b.com, c.com"}` → **1 项 / 2 项**，
  逐项 trim 后为 `b.com`、`c.com`（字符串形态也要能解析）
- 名单归一：`"x.com  y.com,z.com x.com"` → 3 项（去重）；`",,a.com,,"` → 1 项（空项丢弃）
- 数组里的 `null` / 嵌套数组 / 对象被丢，数字项转文本（`42` → `"42"`）
- `cdn.hosts:[]` → 空（不回落内置）；`cdn.hosts:5` → 回落内置 8 条；`cdn.hosts:"a.com, b.com"` → 2 条

### 第 5 组：`Router::decide` 判定矩阵 —— 逐行对上，无偏差

```
  store.steampowered.com   whitelist=upstream blacklist=upstream all=upstream
  www.baidu.com            whitelist=direct   blacklist=direct   all=upstream
  notsteampowered.com      whitelist=direct   blacklist=upstream all=upstream
  127.0.0.1                whitelist=direct   blacklist=direct   all=direct
  192.168.1.1              whitelist=direct   blacklist=direct   all=direct
  23.45.64.1               whitelist=upstream blacklist=upstream all=upstream
  www.google.com           whitelist=direct   blacklist=upstream all=upstream
  localhost                whitelist=direct   blacklist=direct   all=direct
  foo.localhost            whitelist=direct   blacklist=direct   all=direct
  steamloopback.host       whitelist=direct   blacklist=direct   all=direct
  172.17.0.1               whitelist=direct   blacklist=direct   all=direct
  8.8.8.8                  whitelist=direct   blacklist=upstream all=upstream
  (empty)                  whitelist=direct   blacklist=upstream all=upstream
```

任务表里的 5 行（`store.steampowered.com` / `www.baidu.com` / `notsteampowered.com` /
`127.0.0.1` / `23.45.64.1`）× 3 列 = 15 个判定全部命中。`notsteampowered.com` 在 whitelist 下
是 direct（不会被 `ends_with` 骗到），在 blacklist/all 下是 upstream。

额外覆盖：显式名单压过内置规则（`direct_hosts` 含 `store.steampowered.com` → direct；
`proxy_hosts` 含 `www.baidu.com` → upstream；`STORE.SteamPowered.COM.` 先 normalize 再比）；
自定义 `proxy_suffixes` 会**替换**内置 Steam 名单（`store.steampowered.com` 变 direct），
这是 Python 版语义，已在断言里固定。

### 第 6 组：边界三元组

- `match_suffix("b.com","b.com")` → true（等值）
- `match_suffix("a.b.com","b.com")` → true（子域）
- `match_suffix("xb.com","b.com")` → false（必须是 `.b.com` 结尾）
- `in_cidr("23.45.64.1","23.45.64.0/18")` → true
- `is_ip("1.2.3")` → false、`is_ip("::1")` → true、`is_ip("[::1]")` → true（方括号先剥掉）

IPv6/网段边界也顺带跑了一遍：`:::`、`2001:db8::1::2`、`fe80::1%eth0`、九组全 false；
`in_cidr` 的 `/0`、`/33`、跨族（v4 地址对 `::/0`）、v6 `/128`、`/32` 全部符合预期。

---

# 文件 2：`tests/test_sni_parse.cpp`

只测 `parse_client_hello_sni()`（纯字节解析，攻击面）。

## 测试装置：守卫页（为什么它能真的证明「不越界」）

样本不放在普通 `std::vector` 里，而是放在**末尾紧贴 `PAGE_NOACCESS` 的守卫页**上：
`VirtualAlloc` 两页 → `VirtualProtect` 第二页不可访问 → 数据从第一页页尾往前放。
解析器只要多读 1 字节就立刻 `0xC0000005`，由 `SetUnhandledExceptionFilter` 翻译成
`FAIL 解析器越界访问（地址 0x…，read）` + 退出码 3。

装置做过自检：临时探针故意越界读 1 字节 → `CAUGHT code=0xc0000005`，退出码 3（探针已删除）。
所以「无越界」不是「碰巧没崩」，而是真的没读出去。

## 实际输出（原文，完整 48 行）

```
svpn T1 自测：parse_client_hello_sni

=== 1. 真实 ClientHello ===
  样本长度 = 517 字节，record_len = 512
ok    样本长度 517（内嵌 hex 未截断）
ok    样本头 = 16 03 01 / handshake=client_hello
  parsed ok=1 sni=[store.steampowered.com]
ok    真实样本解析成功
ok    SNI == store.steampowered.com
ok    重复解析结果稳定
ok    sni=nullptr 时不崩且返回 true
ok    大写样本与被替换串等长
ok    样本里能定位到 SNI 字面量
  大写样本 → ok=1 sni=[store.steampowered.com]
ok    大写 SNI 归一化成小写
  末尾带点 → ok=1 sni=[store.steampowered.co]
ok    SNI 末尾的 . 被去掉

=== 2. 截断：长度 1..N-1 必须全部 false ===
ok    截断 1..516 全部返回 false（共 516 例）

=== 3. 随机模糊：mt19937 seed=20261025，500 段 ===
  500 段随机字节：解析 true 0 次，最大长度 2047，无崩溃
ok    随机样本若返回 true，sni 必为合法小写域名
ok    长度覆盖到 1..2048 区间（实测最大 2047）

=== 4. 畸形样本 ===
  全 0(2048B) → ok=0
ok    全 0 → false
  16 03 01 + 509B 垃圾 → ok=0
ok    16 03 01 + 垃圾 → false
  record_len=65535 但只有 10B → ok=0
ok    record_len 超出实到长度 → false（分片语义）
ok    record_len=3（< 4）→ false
ok    record type=0x17 → false
ok    record buf[1] != 0x03 → false
ok    handshake type=0x02（server_hello）→ false
ok    handshake body 长度超出 record → false
ok    定位到扩展段总长字段
ok    扩展总长 0xFFFF 越界 → false
ok    定位到 server_name 扩展类型字段
  无 server_name 扩展 → ok=0
ok    没有 SNI 扩展 → false
ok    name_len=0 → false
ok    SNI 含控制字符 → false
  （守卫页大小 = 4096 字节；任何越界读都会被捕获）

[+] PASS 27 / FAIL 0（共 27 项）
```

### 第 1 组：合法样本

样本用 Python 3.13 真机生成（`ssl.MemoryBIO` + `wrap_bio`，
`server_hostname="store.steampowered.com"`，为压长度加了 `set_ecdh_curve('X25519')`）：
**517 字节、单条 TLS record、`record_len=512`、不分片**，内嵌为 hex 常量。

结果：解析出 `store.steampowered.com`。顺带断言了 4 件事：

- 重复解析结果稳定
- `sni == nullptr` 时不崩且仍返回 true（出参可空）
- 大写主机名 `STORE.STEAMPOWERED.COM` → 输出归一化成小写
- 末尾带点（`...co.`）→ 输出去掉点（`...co`）

> 补充事实：不加 `set_ecdh_curve` 时 Python 3.13 默认发 X25519MLKEM768（混合 PQ），
> ClientHello 是 1529 字节（record_len=1524）。两种都能被解析（实现只走扩展链，与 key_share 无关），
> 只是 1529 字节内嵌太长，测试里用 517 字节那份。

### 第 2 组：截断（1..N-1）

同一份样本从长度 1 到 516 **逐字节**喂进去，**516 例全部 false**，无崩溃、无越界。
这一组同时固定了「record 没读满就返回 false」的约定（见下）。

### 第 3 组：随机模糊

`std::mt19937 seed=20261025`，500 段长度 1..2048 的随机字节（实测最大 2047）：

- 无崩溃、无越界
- **0 次**误判为 true
- 断言「若返回 true，则 sni 必须是非空、≤253、全小写可打印 ASCII」——万一将来误判成 true，
  也不会静默放过一个畸形域名

### 第 4 组：畸形

| 样本 | 结果 |
|---|---|
| 全 0（2048 字节） | false |
| `16 03 01` + 509 字节垃圾 | false |
| `record_len` 声称 65535，实际只有 10 字节 | false |
| `record_len = 3`（< 4） | false |
| record type = `0x17`（application_data） | false |
| `buf[1] != 0x03` | false |
| handshake type = `0x02`（server_hello） | false |
| handshake body 长度 0x00FFFF 超过 record | false |
| 扩展总长改成 `0xFFFF`（扩展段被截断） | false |
| 没有 `server_name` 扩展（把 0x0000 改成 0x0012） | false |
| `name_len = 0` | false |
| SNI 内容含控制字符（0x07） | false |

## 一条给 Lead 的约定（不是 bug）

`src/tls.cpp:616` 是 `if (n < 5 + rec_len) return false;`，注释是「分片：调用方继续读」。
即：**截断的 record 一律 false，不做「尽力解析」**，调用方必须先把整条 record 读满再调本函数。
测试 4.3（`record_len=65535` 但只有 10 字节）把这个语义钉住了 —— 如果以后改成「能解多少解多少」，
这条用例会立刻变红，属于**预期内的红**，改实现时记得同步改断言。

---

# 与任务判定表不符之处

**无。** 5 行 × 3 列共 15 个判定全部踩中，`match_suffix` / `in_cidr` / `is_ip` 边界同理。
也没有为了通过而改动 `src/` 的任何一行。

# 没做完 / 已知限制

- 守卫页只有 4096 字节，样本上限因此是 4096（模糊上限设的 2048 够用）。
  若以后要 fuzz 更长报文（如 1529 字节的 PQ ClientHello 可以，但 8KB 不行），需要改成多页 + 末页守卫。
- 没做与 Python 版的**自动差分**：判定矩阵的期望值是手写在测试里的（值来自任务给的基准表），
  不是运行时调 Python `rules.Router.decide` 逐条比对。若要长期守住一致性，建议再加一个
  `--route` 式的差分脚本（`test_config.exe` 已保留 `--route <mode> <host...>` 和
  `<cfg.json>` 两个探针入口，可直接喂给 Python 侧做 diff）。
- 没测 SSLv2-兼容 ClientHello（`buf[0]==0x80`）。当前实现直接 false；这种握手不带 SNI，行为无影响。
- 两个 exe 都没加 `-Wall -Wextra`（按任务给的命令行编译，零警告但不是最严格档）。

---

# 文件 3：`tests/test_cli.py`（按 V1 的 CLI 审计清单 + ⑫ P1）

`tests/test_cli.py` 纯标准库（PAC 那组额外用 node），**113 条断言全绿**，没有 xfail / skip。
分组：A 用法 / B system / C node / D hosts / E doctor / F probe --json / G pac /
H up 覆盖+⑫ / I 生命周期 / J probe --lenient / K cdn 实测列 / L down 兜底 / M status 节点行。

## 跑法

```bash
cd "D:/LE/AI/wbwrk/svpn-cpp"
python tests/test_cli.py            # 全跑，约 45 秒（cdn 那组要联网）
python tests/test_cli.py --no-net   # 跳过 probe / cdn，其余照旧
```

## 安全边界（重要，别改回去）

1. **不碰真实 hosts**：`hosts clear` 一律带 `--no-elevate` 且 `SVPN_HOSTS_FILE=<临时文件>`，
   同时记录真实 hosts 的 md5 并事后比对。
2. **不碰系统代理**：只跑只读的 `system show`，绝不 `system on/off`、`pac --apply`、`up --system-proxy`。
3. **不在工程根目录跑 exe**：复制成 `<temp>/svpn_t1.exe`，config/runtime/pid/effective 全落在 temp 里。
4. **生命周期在一条命令内跑完**：`up → up → status → down` 全在同一个 python 进程里。
5. **端口每轮动态挑**（`bind(0)`），**listen_port 与 http_port 都挑**，且互不相同、也不与覆盖用的
   alt/node 端口重复 —— 环境里有别人手工跑的实例占着 7900，只挑 listen_port 会在 http_port 上撞
   「已在运行」，⑫ 就会假绿。
6. **记录 md5**：开跑打印被测二进制 md5/size/mtime，结论必须带版本。

## 实际输出（原文，完整）

```
svpn CLI 回归（T1）
  被测二进制: D:\LE\AI\wbwrk\svpn-cpp\svpn.exe
  md5=e634fe15067808ccf2ba0dad30d55d97  size=1678848  mtime=2026-10-25 10:46:27
  端口: SOCKS 62384 / HTTP 62385（避开 7899/7900）
  工作目录: C:\Users\zhedu\AppData\Local\Temp\svpn_t1_main_k8z_h699

[A] 用法与退出码
  PASS  无参数 → rc=2
  PASS  无参数 → 打印用法
  PASS  未知命令 → rc=2
  PASS  未知命令 → 报未知命令
  PASS  --help → rc=0 且打印用法（契约）
  PASS  -h → rc=0 且打印用法（契约）
  PASS  help → rc=0 且打印用法（契约）
  PASS  probe -h → rc=0 且打印用法（不该真跑 probe）
  PASS  help 文案不出现 pac --write（防文档把空开关写回去）
  PASS  help 的 pac 行提到 --apply

[B] system（只读，不 on/off）
  PASS  system show → rc=0
  PASS  system show → 含 ProxyEnable
  PASS  system show → 含 ProxyServer/ProxyOverride
  PASS  system（无子命令）→ rc=0
  PASS  system（无子命令）→ 等同 show

[C] node set / node clear
  PASS  node clear → rc=0
  PASS  node clear → 输出「已清空节点」
  PASS  node clear → config upstream.host == NODE_HOST
  PASS  node clear → config upstream.port 回到 1080
  PASS  node set（无 URL）→ rc=2
  PASS  node set（无 URL）→ 打印用法
  PASS  node set 非法 scheme → rc=2

[D] hosts clear --no-elevate（SVPN_HOSTS_FILE 测缝，不碰真实 hosts）
  PASS  hosts clear --no-elevate → rc=0
  PASS  hosts clear → 输出「hosts 已清理」
  PASS  托管块 begin 标记被移除
  PASS  托管块 end 标记被移除
  PASS  托管块里的 2 条记录被移除
  PASS  块外用户行逐行保留
  PASS  块外字节未变（去掉托管块与尾部分隔空行后逐字节相等）
  PASS  BOM 被保留
  PASS  真实 hosts 未被改动

[E] doctor
  PASS  doctor 缺 config.json → rc=1
  PASS  doctor 缺 config.json → 提示跑 svpn init
  PASS  未知键 config → doctor rc≠0
  PASS  未知键 config → 有一行 [!] 同时点名 config.json 与 broken
  PASS  只写部分已知键（缺键）→ doctor rc=0
  PASS  只写部分已知键 → 没发现问题
  PASS  _note 等下划线键被容忍 → doctor rc=0
  PASS  下划线键不被当成未知键点名
  PASS  真语法错误 config → doctor rc=1
  PASS  真语法错误 config → 有一行 [!] 说明 config.json 解析失败

[F] probe --json
  PASS  probe --lenient → rc ∈ {0,1}
  PASS  probe --json → 表格不混进 stdout
  PASS  probe --json → stdout 整体可 json.loads（契约）
  PASS  probe --json → 含 rows 数组
  PASS  probe --json → rows 有 9 个端点
  PASS  probe --json → 含 ok/unusable 字段

[J] probe --lenient 的语义不变量（④）
    strict : 直连 8 / 需节点 1 / 死 0 / 不可用 1  rc=1
    lenient: 直连 8 / 需节点 1 / 死 0 / 不可用 1  rc=0
  PASS  每轮都有 9 个端点计入桶
  PASS  D（当前不可用）== B（需节点）+ C（死端点）
  PASS  strict rc == (D>0)
  PASS  lenient rc == (C>0)
  PASS  lenient 不比 strict 更严（rc_l <= rc_s）
  PASS  --node none 下死端点恒为 0（直连失败记「需节点」）

[G] pac（语法 + 分流判定）
  PASS  pac → rc=0
  PASS  pac 文件已生成 runtime/svpn.pac
  PASS  PAC 里代理串是 SOCKS5 127.0.0.1:<端口>
  PASS  PAC 是合法 JS（new Function 不抛）
    判定: {"store.steampowered.com": "SOCKS5 127.0.0.1:62384; DIRECT", "www.baidu.com": "DIRECT", "example.org": "DIRECT", "notsteam.example.com": "DIRECT", "steampowered.com.evil.cn": "DIRECT"}
  PASS  store.steampowered.com → SOCKS5…; DIRECT
  PASS  www.baidu.com → DIRECT
  PASS  example.org → DIRECT
  PASS  notsteam.example.com → DIRECT
  PASS  steampowered.com.evil.cn → DIRECT

[K] cdn 实测列格式（⑧）
  PASS  cdn 行至少 5 列（域名/最优IP/握手/实测/候选）
  PASS  --no-bench 时实测列是 '-'（不是 0.00MB/s）
  PASS  实测列格式合法（'-' 或 N.NNMB/s）

[H] up -p/-m 覆盖 + ⑫ 失败的 up 不得抢删在跑实例的 pid
  PASS  up -b -p 61263 -m blacklist → rc=0
  PASS  up -b -p → 输出「后台已启动」
  PASS  pidfile 已生成且内容为数字
  PASS  -p 覆盖的端口 61263 已监听
  PASS  effective.json 记下覆盖端口
  PASS  effective.json 记下覆盖模式
  PASS  status 用 effective.json 而非 config.json 报端口
  PASS  status 报 模式 blacklist（来自 -m 覆盖）
  PASS  ⑫ 二次 up -b（端口冲突）→ rc=0
  PASS  ⑫ 二次 up -b → 输出「已在运行」
  PASS  ⑫ pidfile 没被删掉
  PASS  ⑫ pidfile 内容仍是 A
  PASS  ⑫ effective.json 没被失败实例的参数覆盖
  PASS  ⑫ effective.json 逐字节未变
  PASS  ⑫ 仍然只有 1 个实例
  PASS  down 能按 pid 正常停掉 A
  PASS  down 后-p 覆盖端口 61263 连接被拒（10061）
  PASS  down 后config 端口 62384 连接被拒（10061）
  PASS  down 后http 端口 62385 连接被拒（10061）
  PASS  down 后 pidfile 被清理
  PASS  down 后 effective.json 被清理

[I] up -b → up -b → status → down（必须同一条命令内跑完）
  PASS  首次 up -b → rc=0
  PASS  首次 up -b → 输出「后台已启动」
  PASS  pidfile 内容为数字
  PASS  端口 62384 已监听
  PASS  端口 62385 已监听
  PASS  二次 up -b → rc=0
  PASS  二次 up -b → 输出「已在运行」
  PASS  二次 up -b 不换 pid
  PASS  status → rc=0
  PASS  status → 显示运行中且 pid 一致
  PASS  只有 1 个 svpn 进程在跑
  PASS  down → rc=0
  PASS  down → 输出「已停止」
  PASS  down 后 62384 连接被拒（10061）
  PASS  down 后 62385 连接被拒（10061）
  PASS  二次 down → rc=0
  PASS  二次 down → 输出「没有在运行的实例」

[M] up --node 覆盖后 status 的节点行（⑮）
  PASS  up -b --node socks5://127.0.0.1:55131 → rc=0
  PASS  effective.json upstream_configured == true
  PASS  effective.json upstream == 127.0.0.1:55131
  PASS  status 节点行显示覆盖的节点（不再说「未配置」）
  PASS  status 节点行带 socks5
  PASS  ⑮ 用例收尾：down 成功

[L] down 兜底提示：没 pidfile 但端口被占（⑩）
  PASS  前置条件：测试进程已占住端口
  PASS  down 无 pidfile → 报「没有在运行的实例」
  PASS  down 无 pidfile 但 62384 被占 → 提示仍被占用
  PASS  down 此时仍返回 rc=0

PASS 113 / FAIL 0 / SKIP 0（其中硬失败 0、契约待修 0）
```

## V1 清单 → 当前状态（全部绿）

| V1 的用例 | 测试里的断言 | 结果 |
|---|---|---|
| `system show` / `svpn system` 无子命令 | `test_system` | 绿 |
| `node clear` / `node set` 无 URL | `test_node` | 绿 |
| `hosts clear --no-elevate` 字节保真 | `test_hosts_clear` | 绿（8 条） |
| `probe --json` → stdout 整体 `json.loads` | `test_probe` | 绿 |
| `--help` / `-h` / `help` / `probe -h` | `test_usage` | 绿 |
| doctor：未知键 / 缺键 / `_note` / 语法错 | `test_doctor` | 绿（10 条） |
| `up -b -p X -m blacklist` → status 端口+模式 | `test_up_override` | 绿 |
| **⑫ 失败的 up 不得抢删在跑实例的 pid** | `test_up_override` | **绿** |
| `up -b` 二次幂等 / `down` 端口 10061 / 二次 down | `test_lifecycle` | 绿 |
| PAC 语法 + 5 个 host 分流 | `test_pac` | 绿 |
| **④ `--lenient` 语义** | `test_probe_lenient` | 绿（不变量） |
| **⑧ cdn 实测列格式** | `test_cdn_tput` | 绿 |
| **⑨ help 不再宣传 `pac --write`** | `test_usage` | 绿 |
| **⑩ down 的「端口仍被占用」兜底** | `test_down_port_hint` | 绿 |
| **⑮ `up --node` 覆盖后 status 的节点行** | `test_node_override` | 绿（已修，见下） |

## ⑫ 这个 P1 用例（最值钱的一条，别删）

```python
up -b -p <ALT> -m blacklist     # → [+] 后台已启动 pid=A，effective.json 记 ALT/blacklist
status                          # → 报 ALT 监听中 + 模式 blacklist
up -b                           # → 必须 rc=0 + [=] 已在运行（只有 http_port 冲突）
assert runtime/svpn.pid == A                       # 旧版这里文件直接消失
assert effective.json 与之前逐字节相同                # 失败实例不许写状态
assert 只有 1 个 svpn_t1.exe
down                            # → [+] 已停止 pid=A
assert ALT / config listen_port / http_port 全 10061
assert pidfile 与 effective.json 都被清理
```

为什么「只有 http_port 被占」：旧版只查 SOCKS5 端口 → 第二个 up「先成功」→ 子进程 bind HTTP 失败 →
把在跑实例的 pid 带走。现在 `src/main.cpp:255` 两个端口都查，直接 rc=0 且不碰 pid。

## ④ 写成不变量，不写死 rc

```
strict : 直连 8 / 需节点 1 / 死 0 / 不可用 1  rc=1
lenient: 直连 8 / 需节点 1 / 死 0 / 不可用 1  rc=0
```

断言：`A+B+C == 9`、`D == B + C`、`strict rc == (D>0)`、`lenient rc == (C>0)`、
`lenient rc <= strict rc`、`--node none` 时 `C` 恒 0。
**没有**断言「lenient 一定 0」（C>0 时是 1，那样会 flaky）。

## ⑮ 已修，现在是正向用例（此前是 P3，现在不能写 xfail）

`e634fe15` 起 `status` 也读 `effective.json` 的 upstream：

```
up -b -p <ALT> --node socks5://127.0.0.1:<NODE>
→ effective.json: "upstream_configured": true, "upstream": "127.0.0.1:<NODE>"
→ status: 节点     127.0.0.1:<NODE>（socks5）      # 此前一直显示「未配置（纯直连）」
```

断言 5 条：effective.json 两个字段 + status 含节点串 + 不再出现「未配置」+ 带 socks5。

### ⑮b（P4，只记录、没写成失败断言）

`--node http://…` 时 `status` 仍显示 `（socks5）`：

```
effective.json: {"upstream_configured": true, "upstream": "127.0.0.1:61730"}   ← 只有 host:port，没有 kind
status:          节点     127.0.0.1:61730（socks5）                            ← kind 取自 config.json
```

即 kind 没进 effective.json。影响很小（只影响 status 那一行的显示），V1 未列、按 P4 处理，
**我没有把它写成失败断言**，免得用 P4 卡发布。要修的话：effective.json 加 `"upstream_kind"`，
status 有就用、没有就退回 config。

## ⑧⑨⑩ 怎么做成离线可判的

- **⑧ cdn**：`cdn --refresh --no-bench --domain cdn.akamai.steamstatic.com --timeout 4`，
  只验「实测」列是 `-`（不是 `0.00MB/s`），接受 `N.NNMB/s` 形态；**不**断言 >0；拿不到行时 SKIP。
- **⑨ pac --write**：不比 md5（恒真、没信息量），改测文档契约 —— `--help` 全文不得出现 `--write`，
  且 pac 行必须提到 `--apply`。
- **⑩ down 兜底**：不造「pid 属于别人」的场景（防误杀），改成自己占住 config 的 listen_port、
  不放 pidfile，断言 down 打 `[=] 没有在运行的实例` **且** `[!] 但端口 N 仍被占用`，rc 仍 0。

## doctor 的契约（已由实现定死）

```
未知键 {"broken":true,...}          → [!] …config.json…broken…      rc=1
缺键   {"listen_port":X,"http_port":Y} → [+] 没发现问题              rc=0
说明键 {"_note":"说明文字", ...}       → [+] 没发现问题 且不点名 _note  rc=0
语法   {"listen_port": 1234,,}       → [!] …config.json…解析失败…    rc=1
```

断言按**行**匹配（`[!]` 开头 + 含 `config.json` + 含键名），不依赖措辞与行序 —— 两者在不同构建间都变过。

## 二进制版本轨迹（复现时以 md5 为准）

| md5 | 时间 | 子命令 | probe --json | doctor | ⑫ |
|---|---|---|---|---|---|
| `f3a8fa7b` | ~10:2x | 红 | 红 | 红 | （未测） |
| `10460aa7` | 10:30:41 | 绿 | 红 | 红 | 红（pid 被删） |
| `9feff113` | 10:34:21 | 绿 | 绿 | 红 | （未测） |
| `cd13a321` | 10:36:17 | 绿 | 绿 | 红 | 红 |
| `b5c97728` | 10:40:03 | 绿 | 绿 | 红 | 绿 |
| `439fd9dc` | 10:40:29 | 绿 | 绿 | 绿 | 绿 |
| `702be7f8` | 10:42:03 | 绿 | 绿 | 绿 | 绿 |
| `ec6d47eb` | 10:43:27 | 绿 | 绿 | 绿 | 绿 |
| **`e634fe15`** | 10:46:27 | 绿 | 绿 | 绿 | 绿 ← **发布基线（113/113）** |

`f3a8fa7b`~`ec6d47eb` 的 doctor/⑫ 两列由 V1 实测补齐；`e634fe15` 是我完整跑通的版本。

## 踩到的四个坑（都写进测试注释了）

1. **`connect_ex` 在 `settimeout()` 之后返回 10035（WSAEWOULDBLOCK），不是 10061。**
   正解：`s.settimeout(None)` 用阻塞 socket。
2. **固定端口会被上一轮残留污染**：残留监听会让 `doctor` 假绿、`up -b` 假绿。现在每轮 `bind(0)` 现挑。
3. **`listen(1)` 的 backlog 会吃掉自己的探针**：⑩ 那组先用 `connect_ex` 探端口，这一步就占了唯一
   backlog 名额，C++ 侧 300ms 的 `port_open` 连不进来 → `down` 误判端口是空的、提示不出现。
   改 `listen(64)` 后稳定。
4. **`up -b` 返回与写 pidfile 之间有窗口**（V1 踩过）：现在统一用 `wait_pidfile()` 轮询到数字为止，
   不再「读完立刻断言」。`down` 后的端口释放是异步的，也一律轮询 ≤8s 到 10061，不一次性断言。
