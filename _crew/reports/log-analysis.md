# 日志与运行期数据分析（svpn 1.1.1）

状态：**终态（可照着做）**

> 对象：`D:\LE\AI\wbwrk\svpn-cpp\runtime\` 下的全部文件。
> 所有样本都是从本机磁盘**原样粘贴**的，不是示意；所有字段含义都对着源码行号核过，
> 源码与样本冲突时以源码为准并在文中标出。
> 排障顺序沿用总日志 §8.4（`status` → `probe` → `svpn.log` → `stats.json`），
> 本文补上每一步「看什么、什么算异常、异常了怎么办」。

---

## 1. 排障顺序：四步，先看结论再看细节

顺序不能换。`status` 是**一眼定性**，`probe` 是**定位到域名**，日志是**定位到事件**，
`stats.json` 是**量化**。反过来先翻日志，会在几千行里找一个可能根本不存在的错误。

### 第 1 步 `svpn status`——先判「在不在跑」「代理有没有生效」

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
./svpn.exe status
```

实测原文（2026-10-25 12:53，未运行时）：

```
svpn 1.1.1  ·  Steam 分流/加速
进程     未运行
端口     127.0.0.1:7899  未监听
端口     127.0.0.1:7900  未监听
节点     未配置（纯直连）
模式     whitelist
系统代理 ProxyEnable=0
CDN缓存  8 个域名，147 分钟前更新
hosts    无 svpn 记录
上次统计 2 连接 / 下行 0.93MB / 错误 0
rc=0
```

10 行（标题 1 行 + 9 个字段行），逐行判据（源码 `src/main.cpp:496-568`）：

| 行 | 正常 | 什么算异常 | 异常意味着 |
|---|---|---|---|
| 进程 | `运行中 pid=<N>` | `未运行` 但端口「监听中」 | pid 文件丢了/被删，实例还在跑（→ §3 症状 3） |
| 端口 ×2 | 与配置一致且「监听中」 | 显示 7899 而实际在 17899 跑 | 缺 `effective.json`（1.1.0 的 ⑭，1.1.1 已修） |
| 节点 | `未配置（纯直连）` 或 `host:port（kind）` | 配了节点仍显示「未配置」 | `up -b --node` 的覆盖没落盘（`upstream_kind` 是 1.1.1 才补的） |
| 模式 | `whitelist` / `blacklist` / `all` | 与启动参数不符 | 同上，`effective.json` 没读到 |
| 系统代理 | `ProxyEnable=0` 或 `1 127.0.0.1:7899` | 你点了接管但这里是 0 | `--system-proxy`/`system on` 没生效，或 `down` 已还原 |
| CDN缓存 | `N 个域名，M 分钟前更新` | 带「（已过期）」或「无」 | 过期 → hosts 里的边缘 IP 是旧的（→ §2.4） |
| hosts | `无 svpn 记录` / `N 条 svpn 记录` | 有记录但你已 `down` | 没带 `--clear-hosts`，残留指向 127.0.0.1 的记录 |
| 上次统计 | 一定有这一行 | **整行缺失** | `stats.json` 不是合法 JSON（→ §3 症状 1） |

最后一行缺失**不是**「还没统计过」，而是 `status` 读 JSON 失败后的静默跳过
（`src/main.cpp:558-567`：只有解析出 `connections` 才打印）。这是 1.1.0 的一个真实故障，
见 §3 症状 1。

### 第 2 步 `svpn probe`——定位到具体域名

```bash
./svpn.exe probe            # 人读表格
./svpn.exe probe --json     # stdout 只有 JSON（表格改走 stderr）
echo "rc=$?"
```

先看**退出码**，再看 `unusable`，最后才看每行（源码 `src/main.cpp:624-702`）：

| 退出码 | 条件 | 含义 |
|---|---|---|
| 0 | `unusable == 0` | 9 个端点当前都可用 |
| 1 | `unusable > 0` | **至少一个端点当前不可用**（没节点时天然会 ≥1，见下） |
| 1（`--lenient`） | `dead > 0` | 只看「配了节点也不通」的真死端点，不看「没节点所以不通」 |
| 2 | 参数错（`--node` 不是 socks5/http） | 命令没跑起来 |

**最常见的误读**：本机没配节点时 `probe` 必然 `rc=1`，因为社区/头像类端点在无节点时
被归类为「需节点 · SNI 阻断」，计入 `unusable`（`src/main.cpp:666-668`）。
这不是故障，是**当前网络位置的客观事实**。想只要「有没有一条路」的冒烟结论，用 `--lenient`。

实测样本（`runtime/probe.json`，1,268 B，mtime 2026-10-25 12:46）：

```json
{
  "ok": false,
  "rows": [
    { "host": "avatars.steamstatic.com", "verdict": "直连可用", "ms": 250, "usable": true },
    { "host": "steamcdn-a.akamaihd.net", "verdict": "直连可用", "ms": 453, "usable": true },
    { "host": "community.akamai.steamstatic.com", "verdict": "直连可用", "ms": 531, "usable": true },
    { "host": "help.steampowered.com", "verdict": "直连可用", "ms": 547, "usable": true },
    { "host": "cdn.akamai.steamstatic.com", "verdict": "直连可用", "ms": 532, "usable": true },
    { "host": "login.steampowered.com", "verdict": "直连可用", "ms": 1360, "usable": true },
    { "host": "api.steampowered.com", "verdict": "需节点 · SNI 阻断", "ms": 4218, "usable": false },
    { "host": "steamcommunity.com", "verdict": "需节点 · SNI 阻断", "ms": 0, "usable": false },
    { "host": "store.steampowered.com", "verdict": "需节点 · SNI 阻断", "ms": 0, "usable": false }
  ],
  "unusable": 3
}
```

（原文是 2 空格缩进的多行 JSON，字段顺序与取值逐字未改；上面只为阅读压成一行。
**这是 12:46 那一刻的快照**：本机 `runtime/` 被多个任务共写，下一次 `probe`
（12:57 又跑过一次，文件变成 1,254 B）就会覆盖它。现读到的字节数与条目不同属正常，
判读方法不变。）

读法：`ok:false` + `unusable:3` → 对应 `rc=1`；被点名的是 `api.steampowered.com`、
`steamcommunity.com`、`store.steampowered.com`，其余 6 个直连可用。
这一份正好演示了两种失败形态：`ms:4218` 是**握手跑了 4.2 秒才失败**，
`ms:0` 是**TCP 都没连上**（两者的 `verdict` 相同，靠 `ms` 区分）。

### 第 3 步 `runtime/svpn.log`——定位到事件

```bash
tail -n 20 runtime/svpn.log
grep -c "WARN" runtime/svpn.log
grep -n "listening\|失败\|失败:" runtime/svpn.log | tail -5
```

看三件事：**有没有 listening 行**（进程到底有没有起来）、**WARN 说了什么**、
**最后一条是什么时候**（日志不更新 = 进程没在写）。

### 第 4 步 `runtime/stats.json`——量化，不是定性

```bash
python -c "import json;d=json.load(open(r'runtime/stats.json',encoding='utf-8'));print(d)"
```

能 `json.load` 通过是第一步；第二步才是看 `connections`/`errors`/`top_hosts` 对不对得上
你刚才的操作。数字对不上通常说明**读的是上一次实例的残留**（`down` 是 `TerminateProcess`，
不给收尾机会，见 §2.2）。

---

## 2. 每个运行期文件，逐字段

文件路径全部由 `Paths::resolve()` 定死：**exe 所在目录**下的 `runtime\`
（`src/util.cpp:130-146`），不是当前工作目录。把 exe 拷到别处，`runtime\` 就跟着在别处。

| 文件 | 谁写 | 谁读 | 生命周期 |
|---|---|---|---|
| `svpn.log` | `log_write()`（每行追加）+ stdout/stderr 重定向 | 人、`up -b` 失败时 `tail_file` | 只追加，**无轮转** |
| `stats.json` | `Stats::dump()`，每 5 秒 + 退出时 | `status` 的「上次统计」 | 覆盖写，跨实例保留 |
| `probe.json` | `cmd_probe()`，每次 `probe` | 人、脚本 | 覆盖写 |
| `cdn-cache.json` | `save_cache()`（`cdn --refresh` / 启动选边） | 启动时 `apply_ip_map`、`status` | 覆盖写（原子替换） |
| `effective.json` | `_serve` 启动时 | `status`、`down` | `down` 时删除 |
| `wininet-backup.json` | `backup_wininet()` | `down` 还原系统代理 | 还原后不删（留证） |
| `svpn.pid` | `write_pid()` | `down`、`status` | `down` 时删除 |
| `hosts.backup` | `apply_hosts()` 前 | `hosts clear` 还原 | 仅 `hosts apply` 后存在 |

### 2.1 `svpn.log`（样本 2,304 B / 45 行）

行格式来自 `src/util.cpp:107`：

```cpp
std::fprintf(f, "%s %-7s %s\n", now_hms().c_str(), level, msg.c_str());
```

即 `HH:MM:SS` + 空格 + **左对齐补到 7 列的级别** + 空格 + 消息。级别只有三级：
`INFO` / `WARN` / `DEBUG`；`DEBUG` 只在 verbose 时写（`src/util.cpp:103`，
后台 `_serve` 固定 `log_init(lp, true)`，所以后台日志**含 DEBUG**）。

样本前 12 行原文：

```
20:00:37 INFO    SOCKS5   listening 127.0.0.1:7899
20:00:37 INFO    HTTP     listening 127.0.0.1:7900
20:00:37 INFO    HTTP   listening 127.0.0.1:7900
20:00:37 INFO    SOCKS5 listening 127.0.0.1:7899
09:57:36 INFO    SOCKS5   listening 127.0.0.1:7899
09:57:36 INFO    HTTP     listening 127.0.0.1:7900
09:59:44 INFO    SOCKS5   listening 127.0.0.1:7899
09:59:44 INFO    HTTP     listening 127.0.0.1:7900
[i] svpn 1.1.0 已启动 socks5://127.0.0.1:7899 http://127.0.0.1:7900
[i] CDN 模式: hosts 直指最优 IP
[i] 上游: 无（纯直连，whitelist）
[i] svpn 1.1.0 已启动 socks5://127.0.0.1:17899 http://127.0.0.1:7900
```

**两种行混在同一个文件里，这是设计结果，不是错乱**：

| 形态 | 来源 | 有没有时间戳 |
|---|---|---|
| `20:00:37 INFO SOCKS5 listening …` | `log_write()` → stderr | 有 |
| `[i] svpn 1.1.0 已启动 …` | `std::cout` → 被重定向进文件 | **无** |

后台启动时 `up -b` 把自己重启成 `_serve --log-to-file`（`src/main.cpp:280`），
`_serve` 把 stdout / stderr 双双 `_wfreopen_s` 到同一个 `svpn.log`（`src/main.cpp:356-357`），
于是 `printf` 风格的行（无时间戳）和 `log_write` 的行（有时间戳）落到同一文件。

同一行里 `INFO` 后面空格数不一样（`INFO    SOCKS5   listening` 与 `INFO    HTTP   listening`）
是**消息体自带的对齐**，不是格式错。

#### `--log-to-file` 的重开陷阱（已修，但看日志时会遇到它的后果）

`--log-to-file` 是内部参数，只由 `up -b` 自动拼，不要手敲。它的实现有一个关键约束
（`src/main.cpp:375-383` 注释原文）：

```cpp
// stderr 已经指向日志文件，就别再让 log_write 每行重开一次文件
// （两个流抢同一个文件会互相锁住，外部 tail 直接 "resource busy"）。
Paths lp = p;
lp.log.clear();
log_init(lp, true);
```

也就是说：**stderr 已经写这个文件时，`log_write` 必须停止自己开文件**，否则两个文件句柄
争锁，`tail -f` 会报 `resource busy`，日志看起来「卡住不更新」。
如果你看到日志停在某一行不动，先怀疑「是不是有人在同目录手工跑了两份实例」。

其他与日志有关的实测事实：

- **行尾是 CRLF**（实测 `cat -A` 每行末尾 `^M`）：文件以文本模式打开。
- **无 BOM**（实测前 8 字节 `32 30 3a 30 30 3a 33 37` = `20:00:37`）。
- **无轮转**：源码里没有任何 rotate/按大小切分逻辑，文件只增不减。日志变大只能 `down` 后手工删。
- 打开方式是 `"a, ccs=UTF-8"`（追加 + UTF-8 编码），每写一行开一次关一次，不长期占句柄。

### 2.2 `stats.json`（样本 223 B）

样本原文：

```json
{
  "uptime_s": 10,
  "connections": 2,
  "errors": 0,
  "via_upstream": 0,
  "via_direct": 2,
  "up_mb": 0.0006494522095,
  "down_mb": 0.929101944,
  "top_hosts": [
    [
      "store.steampowered.com",
      2
    ]
  ]
}
```

字段全部来自 `Stats::dump()`（`src/util.cpp:174-209`）：

| 键 | 单位 | 含义 | 怎么累加 |
|---|---|---|---|
| `uptime_s` | 秒 | 本实例已运行时长；`started_ms==0` 时写 0 | `now_ms() - started_ms` 取整 |
| `connections` | 条 | 成功解析出目标后计的请求数（SOCKS5 CONNECT / HTTP CONNECT / 绝对 URI 都算） | `conns++`（`proxy.cpp:655/812/856`，SNI 通道 `sni.cpp:123`） |
| `errors` | 次 | 拨号失败、握手失败、回写失败等 | 各失败分支 `errors++` |
| `via_upstream` | 条 | 走节点的连接数 | `proxy.cpp:553` |
| `via_direct` | 条 | 直连的连接数 | `proxy.cpp:556` |
| `up_mb` | MiB | 客户端→上游字节数 / 1048576 | `relay_stream` 回填（`proxy.cpp:536`） |
| `down_mb` | MiB | 上游→客户端字节数 / 1048576 | 同上（`proxy.cpp:537`） |
| `top_hosts` | 数组 | `[域名, 次数]` 对，按次数降序，最多 8 条 | `Stats::hit()`，域名表上限 **64** 个（`util.cpp:159`），落盘取前 8（`util.cpp:190`） |

三个容易踩的统计口径：

1. **`via_upstream + via_direct ≠ connections`**。SNI 通道（CDN 的 443 转发）只加
   `conns` / `bytes` / `hit`，**不加** `via_*`（`sni.cpp:123,186-194` 没有对 `via_*` 的写入）。
   所以开了 CDN 时 `connections` 会大于两者之和，这是正常的。
2. **`down_mb` 是字节数换算，不是"网速"**。样本 0.929 MB / 10 秒看着像 0.09 MB/s，
   但那 10 秒里大部分时间在握手。
3. **`top_hosts` 只统计出过目标域名的请求**；IP 直连（SOCKS5 ATYP=IPv4）也会以点分 IP 作为
   "host" 进表（`proxy.cpp:656` 的 `hit(host)` 传的是解析后的目标），所以表里可能出现纯 IP。

#### 为什么是 5 秒一次，而不是退出时写

`_serve` 起了一个 dumper 线程：50 × `Sleep(100)` = **5 秒**一轮，每轮 `st.dump(p)`
（`src/main.cpp:421-429`）；进程正常结束时再写一次（`main.cpp:435`）。

原因很直接：`down` 是 `TerminateProcess(h, 0)`（`src/main.cpp:447`），**不给进程任何收尾机会**。
如果只在退出时写，那么每次正常使用（`up -b` → 用 → `down`）落盘的永远是上一份旧数据，
`status` 的「上次统计」会一直是 0 或者很早以前的数字。周期落盘是这个架构下的唯一可行解。

### 2.3 `probe.json`（样本 1,268 B）

顶层 3 个键（`src/main.cpp:677-689`）：

| 键 | 类型 | 含义 |
|---|---|---|
| `ok` | bool | `unusable == 0`。**注意它不等于退出码**：加了 `--lenient` 时，`ok:false` 也可能 `rc=0` |
| `rows` | 数组 | 9 个端点，每个 4 个字段：`host` / `verdict` / `ms` / `usable` |
| `unusable` | 数字 | 当前不可用的端点数（`rc` 就是由它决定，除非 `--lenient`） |

**常见误解：`rows` 里没有 `direct_ok` / `node_ok`。**
这两个值只存在于内存里的 `ProbeRow`（`src/main.cpp:572-576`），写 JSON 时只挑了
`host/verdict/ms/usable` 四个字段（`main.cpp:680-687`）。
想区分「直连可用」还是「靠节点可用」，只能看 `verdict` 文案。

`verdict` 只有 4 种取值，判定顺序在 `probe_one()`（`src/main.cpp:609-620`）：

| verdict | 条件 | `usable` | 含义 |
|---|---|---|---|
| `直连可用` | 直连 TLS 握手成功 | true | 不用节点也行 |
| `需节点 · 可用` | 直连失败、走节点成功 | true | 被阻断但有路 |
| `需节点 · SNI 阻断` | 直连失败、**且没配节点** | false | 没节点没法判断，只能算"需要节点" |
| `不通 · 待排查` | 直连失败、配了节点、节点也失败 | false | 真死（或被节点挡住） |

`ms` 是**直连那次 TLS 尝试的耗时**，**成功失败都记**（`main.cpp:599-601`；
`tls_ok_via` 在失败路径上也会回填 `*ms`，`main.cpp:588`）。走节点那次的耗时算了但**没写进
JSON**（`main.cpp:602-608` 里 `t2` 只用于判定）。

所以 `ms` 要这样读：

| `ms` | 含义 |
|---|---|
| `0` | `dial_direct` 直接失败（TCP 都没建起来），`tls_ok_via` 提前返回，`ms` 保持初值 0 |
| `>0` 且 `usable:true` | 握手成功，数值就是握手耗时 |
| `>0` 且 `usable:false` | **连上了但握手失败**，数值是失败前的等待时间（样本里 `api.steampowered.com` 的 `4218` 就是这种情况，别误读成"4.2 秒可用"） |

9 个端点固定（`src/main.cpp:30-40`，顺序即 `rows` 顺序无关——是并发采集后 push 的，
**顺序不稳定**）：

```
store.steampowered.com            443  商店 / 购买
login.steampowered.com            443  登录
api.steampowered.com              443  API / 令牌
steamcommunity.com                443  社区 / 创意工坊
community.akamai.steamstatic.com  443  社区静态资源
avatars.steamstatic.com           443  头像 / 图片
steamcdn-a.akamaihd.net           443  游戏下载 CDN
cdn.akamai.steamstatic.com        443  客户端更新
help.steampowered.com             443  客服 / 找回
```

**判读组合表**：

| 现象 | 判据 | 结论 |
|---|---|---|
| 商店 4 项 `直连可用`，社区/头像 `需节点 · SNI 阻断` | `unusable:2`、`ok:false`、`rc=1` | 本机常态：商店能开、社区要节点 |
| 9 项全 `直连可用` | `unusable:0`、`ok:true`、`rc=0` | 当前出口干净，不需要节点 |
| 某项 `不通 · 待排查` 且配了节点 | `unusable` 含该项 | 节点也救不了，查节点本身或该域名被特别处理 |
| 某项 `需节点 · 可用` | 配了节点 | 节点确实能过，别急着换节点 |

### 2.4 `cdn-cache.json`（样本 3,178 B）

结构（`save_cache()`，`src/cdn.cpp:670-710`）：顶层 `updated` + `domains` 映射。

```json
{
  "updated": 1792895140,
  "domains": {
    "cdn.akamai.steamstatic.com": {
      "domain": "cdn.akamai.steamstatic.com",
      "source": "DoH:doh.pub+system",
      "candidates": 3,
      "best_ip": "23.55.179.9",
      "best_ms": 250,
      "best_mbps": 1.12,
      "tested": 3,
      "ts": 1792895133,
      "top": [
        { "ip": "23.55.179.9",   "ms": 250, "mbps": 1.12, "code": 206, "err": "" },
        { "ip": "23.11.207.108", "ms": 531, "mbps": 0.52, "code": 206, "err": "" },
        { "ip": "23.11.207.109", "ms": 625, "mbps": 0.48, "code": 206, "err": "" }
      ]
    }
  }
}
```

（上面只保留一个域名，原文有 8 个；其余 6 个都是 `source:"none"`、`best_ip:""`、`top:[]`。）

| 字段 | 含义 | 依据 |
|---|---|---|
| `updated` | 整份缓存的写入时刻，**Unix 秒（取整）** | `cdn.cpp:681` `floor(now_sec())` |
| `domains.<域名>` | 以域名为键，值是该域名的选边结果 | `cdn.cpp:684-709` |
| `source` | 候选 IP 从哪来：`DoH:<provider>` / `DoH:<provider>+system` / `system` / `none` | `cdn.cpp:497-527` |
| `candidates` | 候选 IP 总数（DoH + 系统解析去重后） | 同上 |
| `best_ip` | 竞速后选出的边缘 IP；空串 = 没选出 | `cdn.cpp:530-560` |
| `best_ms` | 该 IP 的 TLS 握手延迟（ms） | `best_ms` |
| `best_mbps` | 该 IP 的吞吐（Mbps 口径，实测值）；`0` = 测速没成功 | `cdn.cpp:385-400` |
| `tested` | 实际参与竞速的 IP 数 | `kMaxCandidates` 上限内 |
| `ts` | 该域名这一轮结果的时刻（Unix 秒） | 与顶层 `updated` 通常差几秒 |
| `top` | 竞速明细，最多 **5** 条（`kMaxTop`，`cdn.cpp:53`） | `top[i].code`/`err` |
| `top[i].code` | 探活用的 HTTP 状态码；`200/206/301/302/403/404` 算"活着" | `cdn.cpp:354` |
| `top[i].err` | 非空 = 该 IP 失败原因（如 `HTTP 403`） | `cdn.cpp:355` |

样本里的两个真实细节，能直接当判读练习：

1. `store.akamai.steamstatic.com` 的 `best_ip:"23.55.179.16"` 对应 `top[0].code:403`、
   `best_mbps:0`。403 在探活判据里算「活着」（`cdn.cpp:354`），但吞吐测量只接受
   `200/206`（`cdn.cpp:391`），所以 `mbps` 保持 0。**`best_mbps:0` 不等于"这个 IP 慢"**，
   可能是"这个 IP 不让测速"。
2. 8 个域名里 6 个 `source:"none"`、`best_ip:""`。这不是缓存坏了——代表那 6 个域名**本轮
   既没解析出候选、也没选边**，`status` 的「8 个域名」数的是 key 的个数，不是"8 个都可用"。

#### 怎么判断过期（这是出过事故的地方）

判据有两条，必须同时看：

```bash
# 1) 缓存写于何时、距今多久
date -d @$(python -c "import json;print(json.load(open('runtime/cdn-cache.json'))['updated'])") "+%F %T"
python -c "import json,time;u=json.load(open('runtime/cdn-cache.json'))['updated'];print('age_h=%.2f'%((time.time()-u)/3600))"

# 2) TTL 是多少
grep -n "refresh_hours" config.json config.example.json src/svpn.hpp
```

实测（样本 `updated=1792895140`）：

```
$ date -d @1792895140 "+%F %T"
2026-10-25 10:25:40
$ python -c "import json,time;…"
age_h=2.45
```

默认 TTL 是 `cdn.refresh_hours` = **6 小时**（`src/svpn.hpp` 的 `CdnCfg`），
过期判定是 `now - updated >= max_age_s`（`cdn.cpp:631`），**整份作废**（不是逐条判断）。
所以 `age_h=2.45` → 未过期；`status` 也印证：

```
CDN缓存  8 个域名，147 分钟前更新
```

（147 分钟 = 2.45 小时，同一份数据两条独立路径算出来一致。）

**1.1.0 的事故**：`apply_ip_map` 的 `max_age` 被写成 `10**9` 秒（约 31.7 年），
于是 `updated` 已在 **16.3 小时**前的边缘 IP 依然被写进 hosts —— 域名解析被钉在一个可能
早已下线的 Akamai 边缘上，表现为「下载慢/连不上但 ping 得通」。这类 bug
**任何静态审计、模糊测试、集成测试都抓不到**：类型合法、无告警、覆盖率满分，
只有把「代码里的 TTL」和「磁盘上的 `updated`」对齐才看得见（总日志 §7.5）。
现在的调用点是 `apply_ip_map(p, cfg, cfg.cdn.refresh_hours * 3600.0)`
（`src/main.cpp:363`），TTL 跟随配置。

### 2.5 `effective.json`

1.1.1 的字段（`src/main.cpp:386-404`）：

```json
{
  "listen_host": "127.0.0.1",
  "listen_port": 17899,
  "http_port": 7900,
  "mode": "blacklist",
  "upstream_configured": true,
  "upstream": "127.0.0.1:1080",
  "upstream_kind": "socks5"
}
```

| 键 | 说明 |
|---|---|
| `listen_host` / `listen_port` / `http_port` | 本实例**实际生效**的监听地址与端口 |
| `mode` | 实际生效的分流模式 |
| `upstream_configured` | 是否配了上游（`host` 非空且不是 `NODE_HOST` 占位符，`util.cpp:125-127`） |
| `upstream` | `host:port` 字符串，**仅在上游已配置时存在** |
| `upstream_kind` | `socks5` / `http`，**1.1.1 新增**，同样仅在上游已配置时存在 |

**为什么需要这个文件**：`up -b -p 17899 -m blacklist --node socks5://…` 的覆盖值只存在于
命令行，`config.json` 里还是旧值。`up` 会把这些覆盖项拼成参数传给子进程
（`src/main.cpp:277-290`：`--listen-port` / `--mode` / `--node`），子进程再把它们写进
`effective.json`，`status`/`down` 才有真值可读（`main.cpp:496-531`）。

这是 1.1.0 的 P1 之一（V1 ⑭）：没有它时 `up -b -p 17899` 之后 `status` 显示
「127.0.0.1:7899 未监听」，而实际在 17899 上跑着——用户会以为没起来，再 `up` 一次。

**1.1.1 补 `upstream_kind` 的原因**（`main.cpp:398-400` 注释原文）：

```
// kind 也必须落盘：`--node http://...` 只改了内存里的 cfg，config.json 仍是旧
// kind，status 读回后会把「节点」行显示成 socks5（链路实际走 HTTP = 假信息）
```

`down` 会删掉这个文件（`main.cpp:462-465`），所以「有 `effective.json`」本身就等于
「有过一个正常启动的实例」。文件用 `std::ofstream` 文本模式写 → **行尾 CRLF**。

### 2.6 `wininet-backup.json`

样本（95 B）：

```json
{
  "ProxyEnable": 0,
  "ProxyServer": null,
  "ProxyOverride": null,
  "AutoConfigURL": null
}
```

四个键就是 Windows「Internet 选项 → 连接 → 局域网设置」的四个注册表值
（`HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings`）。

| 时机 | 动作 |
|---|---|
| `up --system-proxy` / `system on` / `pac --apply` 之前 | `backup_wininet()` 把**当前**四值原样存下来 |
| `down`（默认） | `restore_wininet()` 按备份写回，然后打印「[i] 系统代理已还原」 |
| `down --keep-system` | 不还原（保留你手动设的代理） |
| 没有备份文件 | 打印「[!] 系统代理没动过：没有备份（可能从没接管过）」，并回显当前 `ProxyEnable` |

样本 `ProxyEnable:0` + 三个 `null` 说明：备份那一刻本机**没有**设系统代理。
这是最容易误导人的状态——「备份存在」不等于「备份里有代理要还原」。

排查「关掉 svpn 后浏览器还在走代理」时，直接读这个文件，与当前值比对：

```bash
./svpn.exe system show
cat runtime/wininet-backup.json
```

两边的 `ProxyEnable`/`ProxyServer` 不一致，就是「没还原成功」。

### 2.7 `svpn.pid`

内容是一个十进制数字，**没有换行**（`write_pid`，`main.cpp:125-129`：`out << pid`）。

```bash
cat runtime/svpn.pid
```

判定链（`read_pid` + `pid_alive`，`main.cpp:106-123`）：

1. 读不出、`<=0` → 视为没有 pid；
2. `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` 失败 → 视为已死；
3. `GetExitCodeProcess != STILL_ACTIVE` → 视为已死。

**单实例判定靠的不是 pid 文件，而是端口**：`up` 先 `port_open()` 探两个端口，
被占就打印「已在运行」直接 `rc=0` 返回（`main.cpp:255-261`）。
pid 文件只服务于 `down`（按 pid 杀）和 `status`（显示「运行中 pid=…」）。

两个配套细节：

- `remove_pid_owned(p, self)` 只在「文件里的 pid 就是自己」时才删（`main.cpp:133-139`）。
  这是为了修 1.1.0 的 ⑫：第二个实例启动失败时无条件删 pid，会把正在运行实例的记录抹掉，
  之后 `down` 找不到它，端口永久被占。
- pid 文件丢了但进程还活着 → `down` 打印「[=] 没有在运行的实例」，然后**补一句**
  「[!] 但端口 N 仍被占用（可能是上次异常退出的残留）」（`main.cpp:453-460`）。
  这是设计内的兜底提示，见到它就去 `netstat -ano` 找 PID。

### 2.8 `hosts.backup`

只在 `hosts apply` 之后存在，是**原始 hosts 的逐字节副本**（含 BOM，若有）。
`hosts clear` 用它还原。托管块靠两行标记识别（`src/winsys.cpp:29-30`）：

```
# >>> steam-vpn managed block (do not edit) >>>
# <<< steam-vpn managed block <<<
```

`status` 的「hosts N 条 svpn 记录」就是数这两个标记之间的行数（`hosts_managed_entries`，
`winsys.cpp:577-610`）。判断「有没有残留」不需要读 hosts 全文：

```bash
./svpn.exe status | grep hosts
```

---

## 3. 典型症状 → 判据 → 处置

| # | 症状 | 判据（命令 + 期望） | 处置 |
|---|---|---|---|
| 1 | `stats.json` 内容是乱码；`status` **没有**「上次统计」这一行 | `python -c "import json;json.load(open('runtime/stats.json',encoding='utf-8'))"` → 抛 `JSONDecodeError`；文件很小甚至只有 BOM | **1.1.1 已修**。病因：`_wfopen_s(..., L"a, ccs=UTF-8")` 配 `fwrite` 写窄字节串，每两个字节被当成一个 UTF-16 码元重新编码（`util.cpp:202-203` 注释）。修法是改用 `"wb"`（`util.cpp:204`）。旧版本现场：删掉坏的 `stats.json`，重启即可重新生成 |
| 2 | 设了系统代理，浏览器却直连 | `./svpn.exe status` 的 `ProxyEnable=1 127.0.0.1:7899`；但浏览器仍直连 → 用 `pac` 模式时：`./svpn.exe pac` 后 `cp runtime/svpn.pac /tmp/t.js && node --check /tmp/t.js` 报语法错 | PAC 生成非法 JS，浏览器**静默回退直连**（不报错、不弹窗）。`node --check` 不认 `.pac` 扩展名（实测 `ERR_UNKNOWN_FILE_EXTENSION`），必须先复制成 `.js`。重跑 `svpn pac --write`；若报错就是代码 bug，别怪浏览器 |
| 3 | `up -b` 报「端口被占用」，`down` 却说「没有在运行的实例」 | `netstat -ano \| grep -E ":(7899\|7900) "` → 有 LISTENING + PID；`tasklist \| grep -i svpn` | 残留进程（pid 文件丢了但进程活着）。用 netstat 的 PID 确认是自己的 `svpn.exe` 后再 `taskkill /PID <N>`。这就是 1.1.0 的 ⑫ 场景，1.1.1 的 `down` 会额外提示端口仍被占用 |
| 4 | 流量绕回自身，连接数暴涨、句柄打满 | `./svpn.exe status` 的「节点」显示 `127.0.0.1:7899`（或本机任一 svpn 端口）；`stats.json` 的 `via_upstream` 与 `connections` 同步暴涨 | `node scan` 把 svpn 自己的监听端口当成上游节点了（1.0.0 Python 轮的真实 bug）。处置：`./svpn.exe node clear`，再 `node set <真节点>` |
| 5 | 商店/下载正常，社区/头像/创意工坊打不开 | `./svpn.exe probe` → 汇总行形如 `直连可用 6 / 需节点 3 / 死端点 0`（计数随网络变，**关键是 `死端点 0` 且 `unusable` 非空**），被点名的就是 `steamcommunity.com`、`avatars.steamstatic.com` 这类 | SNI 阻断，必须走节点：`./svpn.exe node set socks5://host:port`（或 `node scan`）。没有节点时这不叫故障，见 §1 第 2 步 |
| 6 | `probe` 返回 `rc=0`，但社区确实打不开 | `cat runtime/probe.json` → `"ok": false` 而 shell 的 `$?` 是 0 | 用了 `--lenient`：它只在「配了节点也不通」（`dead`）时才返回 1（`main.cpp:700`）。要严格语义就去掉 `--lenient`；要判「有没有路」才用它 |
| 7 | 下载慢、`steamcdn-a` 连不上，但 `ping` 通 | `date -d @<updated>` 看缓存时间；`./svpn.exe status \| grep CDN缓存` 是否带「（已过期）」；`grep -c "# >>> steam-vpn" /c/Windows/System32/drivers/etc/hosts` | 缓存过期或 hosts 钉了旧边缘 IP。处置：`./svpn.exe cdn --refresh` 重选边；hosts 有记录就 `hosts apply` 重新写（要 UAC），或 `hosts clear` 回到纯 DNS |
| 8 | 连接数长时间不下降 | `stats.json` 的 `connections` 只增不减，但浏览器标签早关了 | **已知取舍**：TCP 中继要**两个方向都 EOF** 才收工（`net.cpp:504-590`），有 idle 上限 1 小时兜底（`kRelayIdleMs=3600000`，`proxy.cpp:34`）；**SNI 通道传的是 `idle_ms=0`，即无限等**（`sni.cpp:192`）。半开连接会一直挂着，直到系统 TCP 超时或 1 小时兜底 |
| 9 | 日志一行不增 | `tail -5 runtime/svpn.log` 最后一行时间戳远早于现在；`./svpn.exe status` 说「未运行」 | 多半是 `_serve` 根本没起来：`up -b` 失败时会打印日志尾部（`tail_file(p.log, 8)`，`main.cpp:315`）；日志为空则提示「多半是端口已被占用」。先查端口（症状 3） |
| 10 | `status` 显示的端口和 `config.json` 不一致 | `cat runtime/effective.json` 里的 `listen_port` 与 `status` 一致，但与 `config.json` 不同 | **不是 bug**：上一次是 `up -b -p 17899` 起的，覆盖值只存在于 `effective.json`。`down` 后该文件被删，就回到 `config.json` 的值 |
| 11 | GUI 点了按钮没反应 | GUI 底部状态行显示「任务执行中」；日志区有「上一个任务还在执行，本次「停止」未提交」 | `run_async` 用 `g_busy` 做互斥，同一时刻只允许一个子命令（`gui.cpp:171-194`）。等底部变「完成」再点。GUI 的「状态」面板就是跑一次 `svpn.exe status` 并把整块输出贴进编辑框（`gui.cpp:149-163`） |
| 12 | `down` 之后浏览器还在走代理 | `./svpn.exe system show` 与 `cat runtime/wininet-backup.json` 不一致；或提示「没有备份（可能从没接管过）」 | 没备份就没法自动还原（`down` 只能提示当前值）。手工 `./svpn.exe system off`；若还不行，去「Internet 选项 → 局域网设置」清掉代理与 PAC 地址 |
| 13 | 端口 443 被占，CDN 选边成功但流量没被接管 | `grep "SNI 转发未启动" runtime/svpn.log` 有命中；`netstat -ano \| grep ":443 .*LISTENING"` 有别的进程 | `cdn.sni_port` 实际必须是 443（hosts 只能表达 IP）。让出 443 或关掉 `cdn.enabled`；注意这是**静默降级**，主流程仍 rc=0（`main.cpp:414-418`） |

---

## 4. 本文所有数字与出处

| 数字 | 值 | 出处（可复核） |
|---|---|---|
| probe 端点数 | 9 | `src/main.cpp:30-40` `kEndpoints[]`；`grep -c '", 443, "' src/main.cpp` |
| `test_config` 用例 | 178 | 实测 `./tests/test_config.exe` → `PASS 178 / FAIL 0` |
| `test_sni_parse` 用例 | 27 | 实测 `./tests/test_sni_parse.exe` → `PASS 27 / FAIL 0` |
| `test_cli` 用例 | 113+ | SPEC §5（Lead 本轮复跑 113/0/0） |
| 源文件数 | 15 | 实测 `ls src/*.cpp src/*.hpp \| wc -l` → 15 |
| `stats.json` 落盘周期 | 5 秒 | `src/main.cpp:421-429`（50 × `Sleep(100)`） |
| `top_hosts` 跟踪上限 | 64 个域名 | `src/util.cpp:159` |
| `top_hosts` 落盘条数 | 8 | `src/util.cpp:190` |
| `cdn-cache` 每域名 `top` 条数 | 5 | `src/cdn.cpp:53` `kMaxTop` |
| CDN 默认 TTL | 6 小时 | `src/svpn.hpp` `CdnCfg::refresh_hours{6}` |
| 中继空闲上限（TCP） | 3600000 ms | `src/proxy.cpp:34` `kRelayIdleMs` |
| SNI 通道 idle | 0（无限） | `src/sni.cpp:192` 传 `0` |
| 中继缓冲区 | 32 KiB | `src/net.cpp:38` `kRelayBuf` |
| `svpn.log` 样本 | 2,304 B / 45 行 | 实测 `wc -l runtime/svpn.log` |
| `stats.json` 样本 | 223 B | 实测 `ls -l runtime/stats.json` |
| `probe.json` 样本 | 1,268 B，`unusable=3` | 实测 `ls -l`；样本原文见 §1 |
| `cdn-cache.json` 样本 | 3,178 B，8 个域名 | 实测 `ls -l`；`python -c "…len(d['domains'])"` |
| 缓存样本时间 | `updated=1792895140` = 2026-10-25 10:25:40 | 实测 `date -d @1792895140` |
| 缓存样本年龄 | 2.45 小时（147 分钟） | 实测算术 + `status` 输出「147 分钟前更新」 |
| `status` 输出行数 | 10 | 实测 `./svpn.exe status \| wc -l`（`gui.cpp:214` 注释写「9 行」，那是把标题行漏算了——以实测为准） |

复核命令打包（一次跑完）：

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
grep -c '", 443, "' src/main.cpp                                   # 9
ls src/*.cpp src/*.hpp | wc -l                                   # 15
./tests/test_config.exe | tail -1                                # PASS 178
./tests/test_sni_parse.exe | tail -1                             # PASS 27
./svpn.exe status | wc -l                                        # 10
wc -c runtime/svpn.log runtime/stats.json runtime/probe.json runtime/cdn-cache.json
python -c "import json;d=json.load(open('runtime/cdn-cache.json'));print(len(d['domains']),d['updated'])"
```

---

## 5. 已知局限（读数据时必须知道）

1. **`stats.json` 的 `via_*` 不覆盖 SNI 通道**，所以 `connections ≥ via_upstream + via_direct`。
   别用三者之和做校验。
2. **`probe.json` 不含 `direct_ok` / `node_ok`**，只有 `verdict` 文案。要机械判断「是否靠节点」，
   只能字符串匹配 `需节点`。
3. **`probe` 的「直连可用」只是"竞速里至少一个 IP 成功"**，不是成功率（总日志 §6.2.2）。
   它适合当"有没有路"的冒烟，不能当质量指标。
4. **`rows` 顺序不稳定**：9 个端点并发采集后按完成顺序 push（`main.cpp:646-652`），
   同一台机器两次运行的顺序可能不同。做 diff 前先按 `host` 排序。
5. **日志无轮转、无大小上限**；长时间运行会持续增长。没有内置清理命令。
6. **`stats.json` 跨实例保留**：`down` 不删它，所以「上次统计」永远是**上一次**的数字，
   不区分是哪一次实例。判断是否当前实例，看 `uptime_s` 是否远小于你的使用时长。
7. **ASCII/编码**：`svpn.log`/`probe.json`/`effective.json` 是 CRLF；`stats.json`/`cdn-cache.json`
   是 LF（`"wb"` + `fwrite` 的副作用）。做逐字节比对时别用统一的行尾假设。
8. **`svpn-gui.exe` 不写自己的日志**：它把 `svpn.exe status` 的输出贴进面板
   （`gui.cpp:149-163`），按钮走进程内 `dispatch_cli`（`gui.cpp:171-194`）。
   所以 GUI 的问题最终仍要回到 `svpn.log` 与 `stats.json` 上看。
