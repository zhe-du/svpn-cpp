# svpn（C++ 版）— Steam 专用本地分流 / 加速

一句话：在本机起一个**只服务 Steam 的代理**（SOCKS5 + HTTP），按域名把 Steam 流量交给你的节点，再带一个 CDN 选边器把下载/更新钉到当前最快的 Akamai 边缘。**没有节点也能用。**

Windows 10/11 x64，**单个 `svpn.exe`（约 1.6MB，静态链接，零 DLL 依赖）**。不需要 Python、不需要 .NET、不需要装运行库——拷一个文件就能跑。

这是 [svpn](https://github.com/zhe-du/svpn)（Python 版）的 Windows 原生重写：命令同名、参数同义、`config.json` 与 `runtime/cdn-cache.json` 双向兼容。

> 当前二进制指纹：**md5 `e634fe15067808ccf2ba0dad30d55d97` · 1,678,848 字节**（2026-10-25 10:46 构建，版本 `1.1.0`）。本文所有行为描述以它为准。
> 同一份源码重编会得到不同 md5，行为不变；对不上就按 [docs/BUILD.md](docs/BUILD.md) 重编。

## 它不做什么

- **不是 VPN，没有虚拟网卡（TUN）**。只有 SOCKS5 / HTTP 代理入口（默认 `127.0.0.1:7899` / `:7900`），不认代理的程序用不上它。
- **不内置任何服务器或节点**。`upstream` 出厂是占位符 `NODE_HOST:1080`，你不填就永远是纯直连。它解决「Steam 该走哪条路」，不解决「怎么出境」。
- **不代理 UDP**。SOCKS5 只实现了 CONNECT（`cmd != 0x01` 直接拒绝），QUIC/HTTP3 与走 UDP 的游戏联机用不上。
- **不做 TLS 中间人**。不装根证书、不碰证书库；hosts 与 SNI 转发都是纯转发，TLS 端到端。
- **不装服务、不加开机自启、不注入进程、不加载驱动**。

---

## 快速开始（4 条命令）

```bat
:: 生成 config.json（已存在则跳过，--force 覆盖）
svpn.exe init

:: 后台起 SOCKS5 127.0.0.1:7899 + HTTP 127.0.0.1:7900
svpn.exe up -b

:: 把浏览器 / 下载器的代理指到 socks5://127.0.0.1:7899 或 http://127.0.0.1:7900
:: （不认代理的程序用不上它，见「它不做什么」）

:: 停进程 + 还原系统代理
svpn.exe down
```

想让**系统代理**（WinINET，浏览器/Steam 客户端跟着走）也指向本机，两条路选一条：

```bat
:: ① 起服务时顺手接管（会先备份原设置，down 时还原）
svpn.exe up -b --system-proxy

:: ② 服务已经起着，再补接管
svpn.exe system on

:: 看当前 ProxyEnable / ProxyServer / AutoConfigURL
svpn.exe system show

:: 按备份还原
svpn.exe system off

:: down 也会顺手还原
svpn.exe down
```

输出前缀：`[+]` 成功、`[i]` 信息、`[!]` 需要你处理、`[=` 无需动作。

也可以直接双击 **`run.cmd`**：没有 `config.json` 就先 `init`，然后 `up -b`、等 2 秒、打一张 `status`，窗口 `pause` 不闪退。

---

## 子命令一览（13 个）

`svpn.exe` 不带参数会打印用法并返回 `2`；任意位置出现 `-h` / `--help` / `help` 只打印用法并返回 `0`；`svpn.exe -v`（或 `--version`）打印版本号。

| 命令 | 参数 | 作用 | 退出码 |
|---|---|---|---|
| `init` | `--force` | 写 `config.json`（默认模板）；已存在则跳过并提示 | `0` / `1`（写失败） |
| `up` | `-b` `--background`<br>`-p` `--port <n>`<br>`-m` `--mode <whitelist\|all\|blacklist>`<br>`--node <socks5://…\|http://…>`<br>`--system-proxy` | 起本地代理。不加 `-b` 是前台运行（Ctrl+C 退出）；加 `-b` 拉起后台进程并写 `runtime/svpn.pid` | `0` 成功或已在运行 / `1` 端口冲突·启动失败 / `2` 参数非法 |
| `_serve` | `--log-to-file`<br>`--system-proxy`<br>`--listen-port` `--mode` `--node` | **内部命令**：`up -b` 拉起的常驻体，接收从父进程透传下来的覆盖项，别手动调 | `0` / `1` |
| `down` | `--keep-system`<br>`--clear-hosts` | 按 `runtime/svpn.pid` 结束进程；默认还原系统代理；`--clear-hosts` 同时清 hosts 托管块（会弹 UAC） | 恒 `0` |
| `status` | — | 进程、两个端口、节点、模式（**端口/模式/节点都取实际生效值**）、系统代理、CDN 缓存年龄、hosts 条目数、上次统计 | `0` |
| `probe` | `--node <url>` / `--node none`<br>`--timeout <秒>`（默认 6）<br>`--json`<br>`--lenient` | 9 个 Steam 端点 × 直连 / 节点两条路，做 TCP+TLS 探测 | `0` 全可用 / `1` 有「当前不可用」 |
| `node` | `scan`<br>`set <url>`<br>`clear` | 扫本机常见代理端口 / 写入节点（会先试连 Steam 验证）/ 清空回纯直连 | `0` / `1` 不可用 / `2` 参数非法 |
| `cdn` | `--refresh`<br>`--no-bench`<br>`--domain <域名>`<br>`--timeout <秒>`（默认 8） | DoH 解析 + TLS 握手竞速（+1MB 实测），结果写 `runtime/cdn-cache.json` | `0` |
| `hosts` | `apply` / `clear`（**省略即 `apply`**）<br>`--no-elevate`<br>`--timeout <秒>`（默认 8） | 写 / 删 hosts 里带标记的托管块；非管理员会自动请求提权 | `0` / `1` |
| `system` | `on` / `off` / `show` | 接管 / 还原 / 查看 WinINET 代理（只写 `HKCU`） | `0` / `1` / `2` |
| `pac` | `--apply` | 生成 `runtime/svpn.pac`；加 `--apply` 同时写进 `AutoConfigURL` | `0` / `1` |
| `steam` | `--system`<br>`--exe <路径>` | 拉起 Steam（`--system` 先把系统代理指过来）；找不到 `steam.exe` 返回 `2`，属正常 | `0` / `1` / `2` |
| `doctor` | — | 体检：`config.json` 是否存在**且能否解析**、有没有未知键、端口占用、CDN 缓存过期、节点连通性、管理员权限 | `0` 无问题 / `1` 有问题 |

细节（都对着当前构建核过，不是推测）：

- **⚠ `hosts` 不带子命令等于 `hosts apply`**，会直接改 `C:\Windows\System32\drivers\etc\hosts`（非管理员先弹 UAC），**没有二次确认**。只想看效果别乱敲；撤销用 `hosts clear`。
- **`up -b` 的后台进程会继承覆盖项**：`-p/--port`、`-m/--mode`、`--node` 由父进程以 `--listen-port/--mode/--node` 显式传给常驻进程，所以 `up -b -p 17899` 真的监听 17899（父进程会等到那个端口就绪才返回）。常驻进程会把**实际生效**的端口/模式/节点写进 `runtime/effective.json`，`status` 与 `down` 读它，所以 `status` 报的是**实际在跑**的端口、模式与节点，而不是 `config.json` 里的旧值。
- **覆盖项不写回 `config.json`**：`-p/-m/--node` 只对本次运行生效，下次不带参数 `up` 会回到配置文件里的值。要长期生效请改 `config.json` 或用 `node set`（它落盘）。
- **`up -b --system-proxy` 会真的接管系统代理**：先备份 WinINET，再把 `ProxyServer` 写成 `127.0.0.1:<listen_port>`，打印 `[+] 系统代理已指向 127.0.0.1:<port>`；`down` / `system off` 按备份还原。
- **`-h` / `--help` / `help` 在任意位置都只打用法**（rc=0），不会顺带执行命令——`svpn.exe probe -h` 不会跑探测。
- **`up` 不做自动发现节点**。`config.json` 里的 `auto_node` 键会被读入但不参与决策；要自动找本机代理请显式跑 `svpn.exe node scan`。
- `probe --node` 接受 `socks5://…` / `http://…`（解析规则同 `node set`）或字面量 `none`（只看直连）；**只影响这一次探测，不写回配置**。想长期换节点用 `svpn.exe node set`。
- **`pac` 本身就会写 `runtime/svpn.pac`**；`--apply` 才会把 `AutoConfigURL` 一起改掉。程序**不认识的 flag 一律静默忽略**，所以历史写法 `pac --write` 不报错、也不多做任何事，等价于 `pac`（现在的帮助文本里已经不列它了）。
- 已经在跑时 `up` 只提示「已在运行」并返回 `0`，**不会重启**——改配置要先 `down` 再 `up`。
- 所有需要写 hosts 的命令（`hosts` / `hosts apply` / `hosts clear` / `down --clear-hosts`）在非管理员下会弹一次 UAC；拒绝就自己用管理员终端加 `--no-elevate` 再跑。

### `probe` 的 9 个端点与退出码

`store.steampowered.com` / `login.steampowered.com` / `api.steampowered.com` / `steamcommunity.com` / `community.akamai.steamstatic.com` / `avatars.steamstatic.com` / `steamcdn-a.akamaihd.net` / `cdn.akamai.steamstatic.com` / `help.steampowered.com`（全部 443）。

每行结论只有四种：

| 行内结论 | 含义 |
|---|---|
| `直连可用` | 直连 TLS 握手成功 |
| `需节点 · 可用` | 直连不通，但经配置的上游能通 |
| `需节点 · SNI 阻断` | 直连不通、且**没配节点**——这行只是告诉你「得上节点」，不是故障诊断 |
| `不通 · 待排查` | 配了节点、直连和节点都不通 |

汇总行是四个数，三桶互斥、第四个是换算：

```
[i] 直连可用 7 / 需节点 2 / 死端点 0 / 当前不可用 2
```

- `直连可用` —— 直连就通；
- `需节点` —— 靠节点才行，**包含两种情况**：配了节点且经节点通了、以及压根没配节点（这时直连不通只能算「需要节点」）；
- `死端点` —— 配了节点，但直连、节点都不通；
- `当前不可用 = 需节点 + 死端点`，是「现在这套配置下有几个端点用不了」的换算，不是第四个桶。

退出码：`0` = 全部可用；`1` = 存在「当前不可用」的端点。**`--lenient` 只按死端点判**（`dead > 0` 才返回 1），所以没配节点时 `--lenient` 返回 `0` —— 这正是它和默认模式的区别，适合「允许社区/头像因无节点而不通」的 CI。

> `--json` 的契约是**stdout 只有 JSON**（可以直接 `svpn.exe probe --json | jq .`），表格、`[i] 节点:` 提示、汇总行全部改走 **stderr**；想同时看人类可读的输出就 `2>&1`。落盘的 `runtime/probe.json` 是同一份纯 JSON。

---

## 配置（`config.json`）

`svpn.exe init` 生成的 `config.json` 放在 **exe 所在目录**。顶层**恰好 12 个键**，与 Python 版逐键一致：

| 键 | 默认 | 说明 |
|---|---|---|
| `listen_host` | `127.0.0.1` | 只监听回环；空值也回落回环。改 `0.0.0.0` 前先配 `auth` |
| `listen_port` | `7899` | SOCKS5 端口 |
| `http_port` | `7900` | HTTP 端口（CONNECT + 绝对 URI）；写 `0` = 不起 HTTP 入口 |
| `mode` | `whitelist` | `whitelist` 只有 Steam/Valve 名单走节点；`all` 全走节点；`blacklist` 国内站点直连、其余走节点 |
| `connect_timeout` | `12` | 建连与握手超时（秒），非正 / >300 回落默认 |
| `auto_node` | `true` | 保留键（当前版本不参与决策，见上一节） |
| `upstream` | `{kind:"socks5", host:"NODE_HOST", port:1080, username:"", password:""}` | `kind` = `socks5` / `http` / `direct`；`host` 为 `NODE_HOST` 视为未配置 |
| `auth` | 空 | 本地鉴权：SOCKS5 用户名密码（RFC 1929）+ HTTP Basic；空 = 不鉴权 |
| `browser` | `{apply_system_proxy:true, use_pac:false, bypass:"<local>;…;*.cn"}` | WinINET 接管开关、PAC 开关、绕过表 |
| `cdn` | `{enabled:false, refresh_hours:6, sni_port:443, hosts:[8 个域名]}` | 两种 CDN 接法 + 选边缓存 TTL + SNI 白名单 |
| `proxy_hosts` | `[]` | **强制走节点**的域名（精确匹配，优先于内置规则） |
| `direct_hosts` | `[]` | **强制直连**的域名（精确匹配，优先级最高，与 `proxy_hosts` 冲突时走直连） |

规范化规则（`merge_config()` 的契约）：

- 顶层键以 `_` 开头（如 `_note`）或不在上面 12 键里 → **丢弃**，只记一条 debug 日志，不报错；
- 嵌套对象按子键合并（`{"cdn":{"refresh_hours":2}}` 只动这一项），嵌套里未知子键同样丢弃；
- 数值键写 `null` / `""` / `"abc"` / `NaN` / 越界 → **回落默认值**，不抛异常；
- 布尔键认 `true/false/1/0/yes/no/on/off` 字符串（大小写、空白不敏感）；
- 名单（`proxy_hosts` / `direct_hosts` / `cdn.hosts`）写成逗号或空格分隔的字符串也认，会归一成数组。

### 两个可选高级键（不在模板里，写了才生效）

| 键 | 默认 | 说明 |
|---|---|---|
| `proxy_suffixes` | 内置 Steam 后缀表 | **替换**「走节点」的后缀名单（分流判定与生成的 PAC 都跟着变） |
| `direct_suffixes` | 内置国内直连表（仅 `blacklist` 模式参与默认策略） | **替换**「直连」的后缀名单 |

- 是**替换**不是追加：写 `["corp.example.com"]` 就等于「只有它走节点」；
- 留空或 `[]` 等于没写，回落到内置表；
- `mode: blacklist` 下 `direct_suffixes` 为空会自动回落到内置国内清单（否则「国内直连」是空话）；
- `init --force` 会用模板重写 `config.json`，把它们丢掉；改完要 `down` 再 `up`。

**优先级**（命中即短路）：

```
direct_hosts（精确）> proxy_hosts（精确）> 回环/本地名 > 字面量 IP 的 Valve 网段 > *_suffixes > 内置 Steam 后缀/网段 > mode 兜底
```

### CDN 加速的两种接法（`cdn.enabled`）

| | `false`（默认） | `true`（SNI 透明转发） |
|---|---|---|
| `hosts apply` 写什么 | 每个 CDN 域名 → 该域名实测最优边缘 IP | `cdn.hosts` 全部 → `127.0.0.1` |
| 谁在干活 | 操作系统直接解析到边缘 | 常驻 `svpn.exe up` 里的 SNI 转发器：读 ClientHello 的 SNI，命中选边缓存就直连最优 IP，否则走上游 |
| 要不要常驻 | 不要，写完 hosts 就能关 | **要**。`up` 一停这些域名全断 |
| 占不占 443 | 不占 | 占 `cdn.sni_port`（实际必须是 443，hosts 条目没有端口位） |
| IP 漂了 | 手动 `cdn --refresh` + `hosts apply` | 只换缓存，不用重写 hosts |

SNI 模式的边界（都在代码里）：只绑回环、只放行 `cdn.hosts` 白名单（空表 = 全部拒绝）、非 TLS 首包直接断、读头上限 16KB / 10 秒、ClientHello 原样转发不做中间人。

---

## 运行期文件（都在 exe 同级目录）

| 路径 | 内容 |
|---|---|
| `config.json` | 配置（上游账号明文，别提交） |
| `runtime/svpn.log` | 运行日志（后台模式 stdout/stderr 都进这里） |
| `runtime/svpn.pid` | 后台进程 pid，`down`/`status`/`doctor` 用它判断 |
| `runtime/effective.json` | 常驻进程**实际生效**的端口/模式/节点（`up -b -p` 这类覆盖的结果），`status`/`down` 读它；`down` 时删除 |
| `runtime/stats.json` | 连接数、上下行字节、按域名 top |
| `runtime/probe.json` | `probe` 的结构化结果 |
| `runtime/cdn-cache.json` | 选边缓存，**与 Python 版逐字节兼容**，两边可互读 |
| `runtime/svpn.pac` | `pac` 生成的代理自动配置 |
| `runtime/hosts.backup` | 写 hosts 前的原始内容（含你本机 hosts 全文） |
| `runtime/wininet-backup.json` | 接管前的系统代理设置 |

除 `config.json` 外全部落在 `runtime/`，整个目录已在 `.gitignore` 里。

---

## 与 Python 版的关系

- **行为基准是 Python 版**。分流规则、CDN 选边、probe 判定、hosts/WinINET 处理都是逐函数对照重写的。
- **两边可以互换 `config.json`**：同样是 12 键 + 可选的 `proxy_suffixes` / `direct_suffixes`，规范化规则一致（脏值回落、`_` 键丢弃、名单可写成字符串）。`runtime/cdn-cache.json` 也能直接互读。
- **C++ 版不需要 Python 运行时**，也不复用 Python 版的任何 `runtime/` 之外的东西；反过来 Python 版也不依赖这个 exe。
- 想两边并存时注意：它们默认用**同一组端口**（7899/7900）和**同一份 hosts 托管块**，同一时间只能跑一个。

---

## 已知差异与限制（诚实版）

1. **`auto_node` 键读了不用**：当前只做解析与回写，没有「自动扫本机端口并写回」的逻辑；要自动找本机代理得显式 `node scan` / `node set`。
2. **半开连接会一直等到系统 TCP 超时**（与 Python 版一致）：中继语义是「等两个方向都自然结束」，一侧结束不会取消另一侧。内部有 1 小时空闲兜底上限，不是几分钟。
3. **只有两个 CDN 域名做真实吞吐实测**：`cdn` 的「实测」列只有 `steamcdn-a.akamaihd.net`、`cdn.akamai.steamstatic.com` 有值（1MB Range 下载），其余域名只做 HEAD，显示 `-` 是设计如此，不是「测了但 0 速」。`--no-bench` 会跳过实测，所有域名都显示 `-`，只留握手延迟。
4. **`cdn.enabled: true` 要占用 443**（`cdn.sni_port`）。被 Watt Toolkit 之类的程序占用时，起服务只打一条 `WARN SNI 转发未启动（端口被占用？）` 就继续跑，此时 hosts 已指向 127.0.0.1 的那些域名会连不上——要么腾出 443，要么把 `cdn.enabled` 关掉、走「hosts 直指最优 IP」。
5. **`probe` 的表格没有证书 CN 列**：SChannel 层用 `SECPKG_ATTR_REMOTE_CERT_CONTEXT` 已经能取到对端 CN（`TlsProbe::cert_cn`），CLI 还没输出；Python 版因为用 `CERT_NONE`，这个字段恒为空。
6. **`probe` 偶发一条 `不通 · 待排查`**：`store` 可能解析到异常边缘，重跑即恢复；代理侧拨号会对候选 IP 做 failover。
7. **`node scan` 只扫本机**：固定 16 个常见代理端口（7890/7891/7897/1080/1081/10808/10809/10810/8889/8080/8118/9910/20171/20172/2080/2081），不读环境变量、不读系统代理设置。
8. **上游只认 `socks5://` 与 `http://`**：`ss://` / `vmess://` / `trojan://` 需要 sing-box / xray 之类先转成本地 socks5，再 `node set`。
9. **写 hosts 需要管理员**；UAC 被拒只能手动用管理员终端加 `--no-elevate`。另外 **`hosts` 省略子命令就是 `apply`**（直接改真实 hosts，无二次确认）。
10. **只有 Windows**：依赖 WinINET 注册表、hosts、SChannel，macOS / Linux 不支持。

---

## 编译

工具链、完整命令、`-static` / `-municode` 的原因、常见链接错误对照，见 **[docs/BUILD.md](docs/BUILD.md)**。

一句话：MinGW-w64 `g++ -std=c++20 -O2 -static -municode` + `-lws2_32 -lcrypt32 -lsecur32 -lwinhttp -ladvapi32 -luser32 -lshell32 -liphlpapi -lole32 -luuid -ldnsapi -lwininet`，或直接跑 `build.cmd`。

源码 13 个 `.cpp` + 1 个 `.hpp`：`json` / `util` / `rules_data` / `config` / `rules` / `net` / `tls` / `proxy` / `cdn` / `winsys` / `pac` / `sni` / `main`。TLS 走 SChannel，并发模型是**线程 + 阻塞 socket + select 双向中继**，没有 asyncio。

---

## 声明

### 用途

做一件事：**给 Steam 的流量做本地分流**。商店、登录、下载走你指定的上游节点或 CDN 最优边缘，国内站点直连。附带三个小功能：CDN 选边（写 hosts）、端点探测（`probe`）、PAC 生成。

它不是 VPN、没有 TUN、不内置节点、不代理 UDP、不做 TLS 中间人（见文首「它不做什么」）。

### 数据与隐私

- **会联网**，只在这几件事上：DoH 查 CDN 域名的 A 记录（`doh.pub` / `dns.alidns.com` / `1.12.12.12` / `223.5.5.5`）、对候选边缘 IP 做 TLS 握手与 1MB 测速、`probe` 连 Steam 各域名、`node scan` 连本机常见代理端口。
- **不上传任何数据**。没有账号、没有遥测、没有中心服务器；所有结果都落在本地 `runtime/`。
- **会写本地文件**：`runtime/svpn.log`、`stats.json`、`probe.json`、`cdn-cache.json`、`svpn.pac`。
- **两个备份文件里有你的系统信息**，默认不随仓库发布：`runtime/hosts.backup`（写 hosts 前的原始内容，含你本机 hosts 全文）、`runtime/wininet-backup.json`（接管前的系统代理设置，可能含公司内网代理地址）。
- **`config.json` 里上游账号是明文**（SOCKS5/HTTP 认证决定的，本机代理协议本身没有更好的存法）。该文件已列入 `.gitignore`，别提交、别同步网盘；贴 `node set` 输出前先看一眼——那行里是完整 URL（含账号密码）。

### 安全

- **写 hosts 需要一次管理员授权（UAC）**。只增删带标记的托管块（`# >>> steam-vpn managed block (do not edit) >>>` … `# <<< steam-vpn managed block <<<`），块外字节**原样保留**（含 BOM 与编码，全程按字节处理不解码）；写前备份，写时先落 `hosts.tmp` 再原子替换。
- **改系统代理不需要管理员**（写的是 `HKCU`）。只在有备份时才还原；没备份就明说「没动过」，绝不猜默认值去清空你的设置。
- **不装服务、不加开机自启、不注入进程、不加载驱动**。`up` 起的只是一个普通后台进程，`down` 就结束。
- **默认只监听回环**。把 `listen_host` 改成 `0.0.0.0` 等于把它变成开放代理——真要共享，先把 `auth.username/password` 配好（SOCKS5 RFC 1929 + HTTP Basic）。
- **SNI 转发不是开放代理**：只绑回环、只放行 `cdn.hosts` 白名单、白名单为空即全部拒绝、非 TLS 首包直接断。
- **只接受合法 hosts 条目**：IP 必须合法，主机名不许出现空格 / `#` / 伪造标记；`cdn-cache.json` 是可被替换的文件，里面的字符串一律不信任。
- **探测阶段不校验证书**：为了区分「握手被 RST」和「证书错误」，探测不校验证书链，只用于诊断，不参与转发。

### 兼容性

- **Windows 10 / 11 x64**。依赖 WinINET 注册表、`System32\drivers\etc\hosts`、SChannel；macOS / Linux 不支持。
- **单 exe 静态链接，零 DLL 依赖**（不需要 libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll），也不需要 Python 或 .NET 运行时。
- **`config.json` 与 `runtime/cdn-cache.json` 与 Python 版双向兼容**，两边可以换着跑（同一时间只能跑一个，端口与 hosts 托管块是共用的）。
- **只代理 TCP**；半开连接会等系统 TCP 超时（见「已知差异」第 2 条）。
- 上游只认 `socks5://` 与 `http://`；`kind: "direct"` 表示不走上游。

### 免责

软件按「现状」提供，不附带任何明示或暗示的担保。因使用或无法使用本工具造成的直接或间接损失（包括但不限于误写 hosts 导致某些网站打不开、改了系统代理忘了还原、下载加速无效），作者不承担责任。

工具本身不提供任何网络出口，也不做任何绕过；你用什么样的上游、访问什么内容，由你自己决定并自行承担相应责任。跨境网络访问请遵守所在地法律法规。

### 许可

MIT，见 [LICENSE](LICENSE)。
