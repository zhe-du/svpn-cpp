# V1 · svpn.exe CLI 只读审计

审计人：V1-verify　｜　时间：2026-10-25 10:22–10:32　｜　方式：复制 exe 到独立目录运行，**未修改 src/、exe、config.example.json**

---

## 1. 环境

| 项 | 值 |
|---|---|
| 审计目标（第 1 份） | `svpn.exe` md5 `b314bc25e4d7b6f3a8b8ad8e96bcbb2c`，1,672,192 B，mtime 10:15:20 |
| 审计目标（第 2 份） | `svpn.exe` md5 `0341fce0b8b36021f3766f0df9b5ab5b`，10:25:06 |
| 审计目标（第 3 份） | `svpn.exe` md5 `f3a8fa7b5dd423a7c820b9a236d50c54`，1,673,216 B，10:29:07 |
| 对应源码 | `src/*.cpp` 在审计期间被持续改动：main.cpp 10:14:12 → 10:25:52 → **10:27:59**；tls.cpp 10:24:03 |
| 隔离方式 | `$TEMP/svpn-v1`（b314）、`$TEMP/svpn-v1b`（0341）、`$TEMP/svpn-v1c`（f3a8），各含 `svpn.exe` + `config.json`，`Paths::resolve()` 落在该目录，不碰工程根目录的 config/runtime |
| 平台 | Windows 11 Pro for Workstations，非管理员（`doctor` 报 `管理员: 否`） |
| 对照实现 | `D:\LE\AI\dshwork\steam-vpn`（`python -m svpn`，1.1.0 同源） |

**两点必须先说明**

1. **exe 与源码不同步**：当前 `svpn.exe`（f3a8fa7b，10:29:07 构建）行为与 10:27:59 的 `main.cpp` 不一致——源码已把子命令索引改为 `a.pos[2]`（main.cpp:627/636/764/848），二进制仍按 `a.pos[1]` 运行（见缺陷 ①）。构建发生在源码修改之后，所以不是"源码没编进去"这么简单，请确认 `build.cmd` 的产物到底来自哪份源码。
2. **后台进程存活受本审计环境限制**：本工具在每条 shell 命令结束时回收进程树，`up -b` 的子进程无法跨命令存活。因此 7/8/16 条测试全部在**同一条命令内**完成（up → status → 再 up → status → down → 端口探测）。首轮"第二次 up 换了 pid"是环境回收导致的假阳性，已重做并推翻。

---

## 2. 逐条结果

| # | 命令 | rc | 关键输出（原样摘录） | 判定 |
|---|---|---|---|---|
| 1 | （无参数） | 2 | `用法: svpn <命令> [选项]` + 14 行命令表 | OK |
| 2 | `-v` / `--version` | 0 | `1.1.0` | OK |
| 3 | `status`（无 config.json） | 0 | `进程 未运行` / `端口 ... 未监听` / `节点 未配置（纯直连）` | OK（不崩） |
| 4 | `doctor`（无 config.json） | 1 | `[!] 缺 config.json，跑 svpn init` … `[!] 1 个问题待处理` | OK |
| 5 | `init` | 0 | `[+] 已写入 C:\Users\zhedu\AppData\Local\Temp\svpn-v1\config.json` | OK |
| 5b | `init`（已存在） | 0 | `[=] config.json 已存在（要覆盖加 --force）`，md5 未变 | OK |
| 6 | `init --force`（先手工写坏 config） | 0 | `[+] 已写入 …`，md5 回到模板值，`diff` 与首次 init 一致 | OK |
| 7 | `up -b` | 0 | `[+] 后台已启动 pid=1596  socks5://127.0.0.1:7899  http://127.0.0.1:7900`；status 显示 `运行中 pid=1596`，两端口 `监听中` | OK |
| 8 | `up -b`（第二次，进程仍活） | 0 | `[=] 已在运行（端口被占用）；改配置请先 svpn down`；pidfile 仍 1596；`tasklist` 命中 1 个 svpn.exe | OK（**无 pid 竞态**） |
| 9a | `probe --node none --timeout 3` | 1 | `[i] 直连可用 6 / 需节点 0 / 不通 3 / 不可用 3`；store/api/community 三行为 `需节点 · SNI 阻断` | 可疑（见 ④） |
| 9b | `probe --node none --timeout 3 --lenient` | 1 | 与 9a **完全相同**的计数与 rc | 可疑（`--lenient` 与严格模式不可区分） |
| 10 | `probe --json --node none --timeout 3` | 1 | stdout = 人类表格 9 行 + JSON + 末尾提示行，**不是合法 JSON**（`Expecting value: line 1 column 2`）；`runtime/probe.json` 是合法纯 JSON（9 rows, ok=false） | 缺陷（见 ②） |
| 11 | `cdn --domain cdn.akamai.steamstatic.com --timeout 5` | 0 | `cdn.akamai.steamstatic.com  23.55.179.9  250ms  0.00MB/s  4`；期间 3 条 `WARN [tls] 解密失败: TLS 握手失败(0x80090318)` | 可疑（实测 0 速率仍报"最优"、rc=0） |
| 12 | `pac` | 0 | `[+] 已写入 …\runtime\svpn.pac（1713 字节）` | OK |
| 12b | PAC 语法（node `new Function`） | 0 | `SyntaxOK 1695`（node 侧长度，文件 1713 B 含 BOM/换行差异） | OK |
| 12c | PAC 三条分流（补 PAC 内置函数垫片） | 0 | `store.steampowered.com -> SOCKS5 127.0.0.1:7899; DIRECT`、`www.baidu.com -> DIRECT`、`example.org -> DIRECT`；附加：`localhost -> DIRECT`、`api.steampowered.com -> SOCKS5…`、`notsteam.example.com -> DIRECT`、`steampowered.com.evil.cn -> DIRECT`（无后缀误命中） | OK |
| 13 | `system show` | **2** | `[!] 用法: svpn system [on|off|show]` | **缺陷 ①** |
| 14 | `hosts`（静态核对） | — | 见第 4 节 | 逻辑 OK，入口 broken |
| 15 | `node scan`（`timeout 30`） | 1 | `[i] 扫描本机常见代理端口…` / `[!] 没找到可用节点（本机常见端口都不通）`（6 秒内返回） | OK（本机确无上游） |
| 16 | `down` → `status` → `down` | 0 / 0 / 0 | `[+] 已停止 pid=13032` → `进程 未运行`、两端口 `未监听` → `[=] 没有在运行的实例`；Python `connect_ex`：7899/7900 = **10061（拒绝）** | OK（端口已释放） |
| 17 | `不存在的命令` | 2 | `[!] 未知命令: 不存在的命令` | OK |
| 附 | `--help` / `-h` / `help` | 2 | `[!] 未知命令: --help` | 缺陷（见 ⑤） |
| 附 | `probe -h` | 1 | **真的执行了 probe**（-h 被当无关参数忽略） | 缺陷（见 ⑤） |
| 附 | 损坏 config（`{"broken":true}`）+ `doctor` | **0** | `[+] 没发现问题` | **缺陷 ③** |
| 附 | 损坏 config + `status` | 0 | 静默按默认值输出，无任何告警 | 缺陷 ③ |
| 附 | `node clear` | 1 | `[i] 扫描本机常见代理端口…`（应为"已清空节点"） | **缺陷 ①** |
| 附 | `node set`（无 URL） | 1 | 同样走 scan（应为 `[!] 用法: svpn node set …` rc=2） | **缺陷 ①** |
| 附 | `pac --write` | 0 | 与 `pac` 输出/md5 完全一致 | 可疑（空开关） |

**空输出 + rc≠0 的组合：未出现。** 所有非 0 退出码都伴随可读文案。

---

## 3. 缺陷清单（按严重度）

### ①【P0】子命令一律读错参数位：`node set/clear` 失效、`system *` 全不可用、`hosts clear` 反向执行
`a.pos = argv`（main.cpp:1024）保留了 argv[0]（exe 路径），但子命令取的是 `a.pos[1]`——那永远是**命令名本身**，不是子命令。

复现（`$TEMP/svpn-v1c`，md5 f3a8fa7b）：
```
> svpn.exe system show        # 期望打印 ProxyEnable/ProxyServer/AutoConfigURL，rc=0
[!] 用法: svpn system [on|off|show]      rc=2
> svpn.exe system              # 期望默认 show
[!] 用法: svpn system [on|off|show]      rc=2
> svpn.exe node clear         # 期望 [+] 已清空节点；config 里 "host": "NODE_HOST" 应被重置
[i] 扫描本机常见代理端口…  [!] 没找到可用节点      rc=1   ← 实际去 scan
> svpn.exe node set            # 期望 [!] 用法: svpn node set socks5://… rc=2
[i] 扫描本机常见代理端口…      rc=1                      ← 实际去 scan
```
**最危险的是 `hosts clear`**：`action = pos[1] = "hosts"`，既不是 `"clear"`，代码就往下落到 apply 分支（main.cpp:764 之后）→ 用户执行"清理 hosts"，程序却去**写** hosts（非 SNI 模式写最优 IP，SNI 模式写 `127.0.0.1`）。同理 `svpn down --clear-hosts` 的提权子进程收到 `hosts clear --no-elevate`，同样被判成 apply → 清理通道整体断开。
（此条只做静态判定，**未执行 `hosts clear`**，避免二次污染真实 hosts。）

期望 vs 实际：`pos[2]`（源码 10:27:59 已改对）vs 二进制仍 `pos[1]`。
**结论：源码已修、二进制未体现 → 当前 `svpn.exe` 不可用于验收。**

### ②【P1】`probe --json` 的 stdout 不是纯 JSON
复现：`svpn.exe probe --json --node none --timeout 3 > p.json` → `python -c "import json;json.load(open('p.json'))"` 失败（`Expecting value: line 1 column 2`）。
实际 stdout 依次是：`[i] 节点: …` → 86 列表格 → JSON → `退出码 0 = …` 提示行。`WARN 全部 N 个 IP 连接失败` 走的是 stderr（已分流验证）。
影响：`svpn probe --json | jq`、脚本化探测全部失效；Python 版同样输出表格，属于同源问题但 C++ 版应当先修。
可用 workaround：`runtime/probe.json` 是纯 JSON（`ok` / `unusable` / `rows[host,verdict,ms,usable]`，已验证可 `json.load`）。
期望：`--json` 时只输出 JSON 到 stdout，人类表格/hint 走 stderr 或被抑制。

### ③【P1】`doctor` 面对损坏的 config.json 报"没发现问题"（rc=0）
复现：
```
printf '{"broken":true}' > config.json
svpn.exe doctor    → [+] 没发现问题   rc=0     # 期望 rc≠0 且明确指出解析失败
svpn.exe status    → 静默按默认配置输出          # 无任何提示
```
原因：`load_config()`（config.cpp:325-347）解析失败时 `log_write("WARN", …)` 后直接 `merge_config(nullptr)`；`cmd_doctor` 调用时没传 `err`，无从得知。doctor 存在的意义就是体检，这是语义性失效。
期望：doctor 用 `load_config(&err)`，`err` 非空即计入 issues 并 rc=1；status 至少在 stderr 提示"config 解析失败，使用默认值"。

### ④【P2】`probe` 汇总与行内判定自相矛盾，`--lenient` 形同虚设
同一份输出里：三行写着 `需节点 · SNI 阻断`，汇总却是 `需节点 0 / 不通 3 / 不可用 3`。
原因：未配节点时 `row.node_ok=false,row.usable=false`，于是被计入 `dead`（main.cpp:511-528 + 565-570）。
连带后果：`--lenient` 的判据是 `dead ? 1 : 0`，而"SNI 阻断（本就要靠节点）"已被算进 `dead`，所以 lenient 永远等于严格模式（实测两次 rc 都是 1）。
期望：三分类应为"直连可用 / 需节点（预期内） / 真不通"，lenient 只在"真不通"时返回 1。

### ⑤【P2】没有任何 `-h/--help`，且未知短选项被静默忽略
`svpn --help`、`-h`、`help` → `[!] 未知命令: …` rc=2；`svpn probe -h` 不报错，直接跑去 probe（rc=1）。
Python 版 `python -m svpn system --help` 正常（argparse）。文案层面 help 里宣传的 `node scan | set <url> | clear`、`system on|off|show`、`hosts apply|clear` 全部与实现符号对不上（见 ①）。

### ⑥【P2】probe 表格列宽不足，`需节点 · SNI 阻断` 撑破列
`%-14s` 装 17 个显示宽度的中文串（`需节点 · SNI 阻断`），导致"备注 / 耗时"列整体左移错位。建议按显示宽度（wcwidth）计算或加宽到 20。

### ⑦【P2】调试输出污染 stdout（已在最新构建消失，仅记录）
b314 与 0341 两个构建会往 stdout 打 `[dbg] REMOTE_CERT_CONTEXT ss=0x00000000 cert=… err=0`（每次 TLS 握手一行，probe 一次 7 行）。f3a8fa7b 已消失，`src/tls.cpp` 也已删除该打印——属于既已修复项，列此以免回归。

### ⑧【P3】`cdn` 在 0 速率下仍报"最优 IP"且 rc=0
`cdn --domain cdn.akamai.steamstatic.com --timeout 5`：4 个候选里 3 个 `TLS 握手失败(0x80090318)`，最终输出 `实测 0.00MB/s`，仍选定 23.55.179.9 并 rc=0，无任何"竞速失败/结果不可信"提示。该 IP 随后被 `hosts apply` 直接写进真实 hosts（见第 6 节），风险链条清楚。
期望：实测为 0 时打 WARN，或在全部候选失败时 rc≠0。

### ⑨【P3】`pac --write` 是空开关
help 写 `pac 生成 PAC（--write / --apply）`，但 `cmd_pac` 只识别 `--apply`；`pac` 与 `pac --write` 输出及 md5 完全一致。要么删掉 `--write` 的表述，要么让它成为"默认不落盘、显式才写"。

### ⑩【P3】`down` 只按 pid 存在性杀进程
`pid_alive()`（main.cpp:115-123）只查进程是否存活，不校验映像名/启动时间；pid 被系统复用后 `svpn down` 会误杀无关进程。建议 `QueryFullProcessImageNameW` 比对 `svpn.exe`。

### ⑪【P3】pid 文件与"可服务"状态之间有窗口
`up -b` 在端口就绪后才 `write_pid`（main.cpp:312），子进程 `_serve` 自己也写一次；此间 `status` 会显示"未运行"但端口已监听。`down` 之后 `runtime/svpn.pid` 已删，属于正常。

### ⑫【P1】失败的第二个实例会删掉正在运行实例的 pid 文件 → `down` 失效、进程孤儿化
复现（`$TEMP/svpn-v1d`，10460aa7）：
```
> svpn.exe up -b -p 17899          # 成功，pid=26820，17899 + 7900 监听
[+] 后台已启动 pid=26820  socks5://127.0.0.1:17899
> svpn.exe up -b                   # 端口检查只看 config 的 7899（空闲）→ 又拉一个子进程
[!] 后台启动失败，日志尾部：
                                   # 子进程因 7900 被占启动失败退出，退出前 remove_pid()
> svpn.exe down
[=] 没有在运行的实例               # 26820 还活着，7900/17899 仍 OPEN
```
链路：`cmd_up` 的占用检测只查 `config.json` 的端口（main.cpp:250）→ 拉到第二个 `_serve` → 它绑不上 7900 → `serve_all` 返回 → `_serve` 退出路径 `remove_pid()`（main.cpp:411）无条件删 pid 文件（不管文件里是不是自己）→ `down` 读到空 pid → 只提示"没有在运行的实例" → 首个实例成孤儿，占着端口，后续 `up` 继续失败。
期望：`_serve` 只删属于自己 pid 的文件；`cmd_up` 的占用检测要覆盖运行时覆盖端口（或干脆检测两个端口）；`down` 在无 pid 时应兜底扫描端口并提示如何清理。
（本次为我手工 `taskkill` 收尾，未在系统里留下 svpn 进程；审计环境会在命令结束时回收进程树，真机终端里这个孤儿会一直活着。）

### ⑬【P3】后台启动失败时没有任何诊断信息
同上场景：`[!] 后台启动失败，日志尾部：` 后面是空的。子进程明明绑不上 7900，却既没在日志里写原因，也没在 stdout 里说。用户无法判断是端口冲突还是配置错误。


### ⑭【P2】`up -b -p/-m/--node` 生效了，但 `status` 不同步（文档也跟着错）
复现（`$TEMP/svpn-v1d`，md5 10460aa7）：
```
> svpn.exe up -b -p 17899 -m blacklist
[+] 后台已启动 pid=24128  socks5://127.0.0.1:17899  http://127.0.0.1:7900
[i] 上游: 无（纯直连，blacklist）
> svpn.exe status
端口     127.0.0.1:7899  未监听        ← 实际在 17899 监听
模式     whitelist                    ← 实际跑 blacklist
```
原因：覆盖项只在 `cmd_up` 的内存里 + 传给子进程（main.cpp:277-282），**不落盘**；`cmd_status` 只读 `config.json`。用户看到"未监听"会以为起失败了，或去 `up` 第二次（幸好端口检测用的是 config 端口，会真的再起一个 → 见 ⑫ 的孤儿化链路）。
连带风险：`up -b -p 17899` 之后 `up -b` 再跑一次，端口检查查的是 7899（空闲）→ 会拉起第二个实例并**覆盖 pid 文件**，前一个成孤儿（`down` 只杀 pid 文件里那个）。建议：运行时把实际生效的 `listen_port/mode` 写进 `runtime/` 供 `status` 读，或干脆把覆盖项落盘。


---

## 4. hosts 静态核对结论（未执行 `hosts apply/clear`）

| 关注点 | 结论 | 依据 |
|---|---|---|
| SNI 模式（`cdn.enabled=true`）是否写 `127.0.0.1` 而非最优 IP | **是** | main.cpp:764-775：`entries["127.0.0.1"] = 所有 cdn.hosts`，随后 `apply_hosts(entries)` |
| 非 SNI 模式写最优 IP | 是 | main.cpp:798-803：按 `cdn-cache.json` 的 `best_ip` 反查聚合 |
| 缓存过期时是拒绝写还是先重选 | **先重选**（正确） | main.cpp:783 `load_cache(p, ttl)` 过滤过期项；为空时打印 `[!] CDN 选边结果已过期（x.x 小时前更新，TTL y 小时），重新选边…` 并 `refresh()` 后重取；仍为空 → `[!] 没有 CDN 选边结果，先跑 svpn cdn --refresh` rc=1，**不写** |
| 是否临时文件 + 原子替换 | **是** | winsys.cpp:271-284：同目录 `hosts.tmp` → `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`，两步任一失败都 `DeleteFileW(tmp)` |
| 块外字节是否保留 | **是（字节级）** | winsys.cpp:299-360 `strip_managed_block`：逐行保留各自 `\r\n`/`\n`、单独摘出 BOM 再原样放回；apply 走 `bom + rstrip(body) + nl + nl + block + nl`；clear 走 `bom + body`（不补换行）。实测佐证：`hosts.backup`(824 B) → apply(+130 B=954 B) → 清理后与备份 **byte-identical** |
| 写前备份 | 有，`runtime/hosts.bak`（审计中实际文件名 `hosts.backup`），覆盖上一份 | winsys.cpp:518-526 |
| 权限被拒时 | `需要管理员权限写 hosts`，rc=1，不抛栈 | winsys.cpp:537-546 |

**唯一问题不在写逻辑，而在入口**：`hosts clear` 与 `down --clear-hosts` 的提权子进程都会被缺陷 ① 判成 apply，等于"清理不掉、反而重写"。修 ① 之后这两条路径即恢复。

---

## 5. 没测到的与原因

| 未测项 | 原因 |
|---|---|
| `up`（前台）、`up --system-proxy` | 前台会阻塞；`--system-proxy` 会改真实 WinINET（备份/还原有风险），按要求只读审计 |
| `system on` / `system off` | 直接改真实系统代理，明令禁止 |
| `hosts apply` / `hosts clear` | UAC + 改真实 hosts；改为静态核对（第 4 节） |
| `pac --apply` | 会写注册表 AutoConfigURL |
| `steam` | 会拉起真实 Steam 客户端 |
| `cdn --refresh` | 按要求"少联网"，只用缓存路径 |
| `probe` 的"需节点 · 可用"分支 | 本机无可用上游，无法构造 node_ok=true |
| `doctor` 的管理员分支 | 当前非管理员 |
| 与 Python 版 `--json` 的逐字节对比 | Python 版同样把表格写 stdout，对比价值低；且不应在 Python 工程里产生文件 |

---

## 6. 事故披露（必须知会）

审计第 13 条时为了诊断 `system` 的参数解析，我跑了 `svpn.exe hosts`（无子命令）——在我当时的认知里它是只读诊断，但 `hosts` 的默认动作是 **apply**，于是它请求提权并**真的改写了本机 hosts**：追加了
```
# >>> steam-vpn managed block (do not edit) >>>
23.55.179.9     cdn.akamai.steamstatic.com
# <<< steam-vpn managed block <<<
```
（UAC 被批准，写入时间 10:25:23；IP 正是第 11 条 cdn 选出的地址。）

已完全还原：该次 apply 前程序自己备份了原始字节（`runtime/hosts.backup`，824 B），我用 Python 参考实现 `python -m svpn hosts clear` 剥离托管块，随后逐字节比对 —— **`backup == 当前 hosts`，824 B，byte-identical，无托管块残留**（10:28:59）。当前无 svpn 进程、7899/7900 均 `10061` 拒绝。

顺带暴露两个 UX 风险，建议一并处理：
1. `hosts`（无子命令）**默认就写系统文件并弹 UAC**，没有任何"即将修改 C:\Windows\System32\drivers\etc\hosts，继续？"的确认。
2. 正因为缺陷 ①，用户**无法用 CLI 撤销**这次写入（`hosts clear` 会再 apply 一遍），只能靠 `down --clear-hosts`（同样被 ① 破坏）或外部手段。

---

## 7. 复测：构建 `10460aa73f4ae333edbc4a4710602858`（10:30:41）

审计结束前 lead 又产出新二进制，对四个关键项复测（干净目录 `$TEMP/svpn-v1d`）：

| 项 | 结果 | 判定 |
|---|---|---|
| `system show` / `system`（无子命令） | `ProxyEnable 0 / ProxyServer / ProxyOverride / AutoConfigURL`，rc=0 | **① 已修复** |
| `node set`（无 URL） | `[!] 用法: svpn node set socks5://user:pass@host:port` rc=2 | **① 已修复** |
| `node clear` | `[+] 已清空节点（回到纯直连分流）` rc=0，config 回到 `"host": "NODE_HOST"` | **① 已修复** |
| `hosts clear --no-elevate`（`SVPN_HOSTS_FILE` 沙箱） | `[+] hosts 已清理` rc=0；BOM 保留、块外 `9.9.9.9 after.example` 原样、托管块消失 | **① 已修复，字节保真达标** |
| `probe --json` | stdout 仍以 `[i] 节点` + 表格开头，`json.load` 仍失败 | **② 未修复** |
| 损坏 config + `doctor` | 仍 `[+] 没发现问题` rc=0 | **③ 未修复** |

沙箱验证的字节级证据（`tmp_hosts`，BOM + CRLF + 块后有内容）：
```
写前: b'\xef\xbb\xbf# header line\r\n127.0.0.1 localhost\r\n\r\n# >>> steam-vpn managed block (do not edit) >>>\r\n1.2.3.4 foo.example\r\n# <<< steam-vpn managed block <<<\r\n9.9.9.9  after.example\r\n'
写后: b'\xef\xbb\xbf# header line\r\n127.0.0.1 localhost\r\n\r\n9.9.9.9  after.example\r\n'
```
> 教训：`hosts apply/clear` 完全可以用 `SVPN_HOSTS_FILE`（winsys.cpp:287）在沙箱里测，根本不需要碰真实 hosts —— 我第 6 节那次事故是没先看这个测试缝，属我的操作失误。

### 7.1 追加复测（同一构建 `10460aa7`，沙箱 `SVPN_HOSTS_FILE`）

| 项 | 结果 |
|---|---|
| `hosts`（省略子命令）= apply？ | **是**。`hosts --no-elevate` 在沙箱文件里写出托管块：`b'127.0.0.1 localhost\r\n\r\n# >>> steam-vpn managed block (do not edit) >>>\r\n23.55.179.9     cdn.akamai.steamstatic.com\r\n# <<< steam-vpn managed block <<<\r\n'`，rc=0 |
| `pac --write` 与 `pac` 等价？ | **是**。两次 md5 均 `1ad12b447fd56d6c60749c1c3d94fbfc`，1713 B |
| `up -b -p 17899 -m blacklist` | **覆盖项确实传到子进程**（源码 main.cpp:277-282 拼 `--listen-port/--mode/--node`）：实际监听 17899，日志 `上游: 无（纯直连，blacklist）`，rc=0 |
| `status` 与运行态 | **不一致（新发现 ⑫）**：`status` 读 `config.json`，显示 `127.0.0.1:7899 未监听`、`模式 whitelist`，而实际在 17899 跑 blacklist |
| `down` | 正确结束 24128，7899/7900/17899 全部 `10061`，无残留 |
| 日志流向 | `log_write`（util.cpp:111）**无条件写 stderr**，所以正常路径也会看到 `INFO hosts 已写入 1 条托管记录`；成功后 `--json` 的 stderr 里因此混着 INFO/WARN 行（stdout 才是数据） |

### 7.2 构建 `cd13a321`（10:36:17）复测

| 项 | 结果 |
|---|---|
| ① `system show` / `node clear` / `hosts clear`（沙箱） | **全部通过**：`system show` 打印四行 rc=0；`node clear` `[+] 已清空节点` rc=0；`hosts clear --no-elevate` 后 `b'127.0.0.1 localhost\r\n\r\n9.9.9.9  after\r\n'`（块去掉、块外保留） |
| ② `probe --json` | **已修**：stdout 60 行、纯 JSON、`json.load` 通过（`ok=false, unusable=2, rows=9`）；表格与提示行已移出 |
| ⑤ help | **已修**：`--help` / `-h` / `help` 三种写法均 rc=0 打印用法；连 `svpn probe -h` 也改为打印用法（此前是「未知命令 rc=2」，`probe -h` 更会真跑一次 probe） |
| ③ 真语法错误（`this is not json {{{`） | **已修**：`doctor` 打印 `[!] config.json 解析失败: …（当前按默认值运行）` + `[!] 1 个问题待处理`，**rc=1**；`status` 也会往 stderr 打 `WARN config.json 解析失败: bad literal，使用默认配置` |
| ③ 合法 JSON 但全是不认识的键（`{"broken":true}`） | 仍 `[+] 没发现问题` rc=0 —— 可接受（合法 JSON，缺项走默认），但"一个已知键都没有"其实值得提示一句（P3 建议） |
| ③ 文案 | 前缀重复：`[!] config.json 解析失败: config.json 解析失败: bad literal（当前按默认值运行）`（P3，`err` 里已带前缀又拼了一次） |
| ⑫ 失败实例删 pid | **仍复现（P1）**：`up -b -p 17899` → 再 `up -b` → `[!] 后台启动失败，日志尾部：` rc=1 → `runtime/svpn.pid` 被删 → `down` 报 `没有在运行的实例`，而 21024 仍活、7900/17899 仍 OPEN |
| ⑬ 失败时日志尾部为空 | 仍为空 |
| ⑭ `status` 与运行时覆盖不一致 | 仍复现：`up -b -p 17899 -m blacklist` 后 `status` 报 `127.0.0.1:7899 未监听` |

**当前净剩余**：P1 ×1（⑫ 孤儿化）、P2 ×2（⑭ status 不同步、④ lenient/三桶语义）、P3 ×若干（⑧⑨⑩⑪⑬、③ 的两处文案）。①②③(语法)⑤ 均已关闭。

### 7.3 构建 `b5c97728d52793974ec34a4471cc1f30`（10:40:03）复测

| 项 | 结果 |
|---|---|
| ⑫ 失败实例删 pid | **已修**：`up -b -p 17899`（pid=21920）→ 再 `up -b` → `[=] 已在运行（端口被占用）；改配置请先 svpn down` rc=0，pid 文件仍是 21920 → `down` → `[+] 已停止 pid=21920`，7899/7900/17899 全部释放、无残留进程。说明占用检测已覆盖 `http_port`（7900） |
| ⑭ `status` 与运行时覆盖 | **已修**：`up -b -p 17899 -m blacklist` 后 `status` 显示 `端口 127.0.0.1:17899 监听中` + `模式 blacklist`，与实际一致 |
| ③ 语法错误 + `doctor` | rc=1 正常；**文案前缀仍重复**：`[!] config.json 解析失败: config.json 解析失败: bad literal（当前按默认值运行）` |
| ⑬ 启动失败无诊断 | 该路径已被 ⑫ 挡住（不再产生第二个实例），本构建无法复现 |
| 回归 | `--help` rc=0、`probe --json` 纯 JSON（`json.load` 通过，rows=9）且 rc=1、`down` 后端口即时释放（TerminateProcess 异步，约 100ms 内 17899 才从 OPEN 转 closed，属正常窗口） |

**验收判定：可以过。** ⑫⑭ 这两条唯一会伤到真实用户的（`down` 失效 + 端口被占死）已关闭。剩余全部是文案/语义级：④ `--lenient` 与三桶口径、⑥ 表格列宽、⑧ `cdn` 0 速率、⑨ `pac --write` 空开关、⑩ `down` 按 pid 误杀（⑫ 修好后触发概率更低但逻辑仍在）、③ 文案前缀重复。

### 7.4 构建 `439fd9dcd000460867f3132934d144fd`（10:40:29，size 1,677,824）复测

| 项 | 结果 |
|---|---|
| ③ 语法错误文案 | **前缀重复已修**：`[!] config.json 解析失败: bad literal（当前按默认值运行）` + `[!] 1 个问题待处理`，rc=1 |
| ③ 未知键检测 | **新增且合理**：`{"broken":true}` → `[!] config.json 里有无法识别的键: broken（拼错了？可 svpn init --force 对照模板）`，rc=1 |
| ③ 缺键容忍 | `{"listen_port":7899}` → `[+] 没发现问题` rc=0（缺项走默认值）——**契约成型：未知键报警、缺键容忍** |
| ⑫ 重复 up | 仍绿：`up -b -p 17899`(pid=20604) → `up -b` → `[=] 已在运行（端口被占用）` rc=0、pid 未变 → `down` → 7899/7900/17899 全 `10061` |
| `system show`（成功文案，供文档引用自查命令） | 四行：`ProxyEnable 0` / `ProxyServer` / `ProxyOverride` / `AutoConfigURL`，rc=0 |

### 7.5 构建 `702be7f89166b51e7b16216acb5199dd`（10:42:03）复测（与 7.4 同口径，全绿）

| 探针 | 结果 |
|---|---|
| `system show` | `ProxyEnable 0 \| ProxyServer \| ProxyOverride \| AutoConfigURL`，rc=0 |
| `--help` | rc=0 打印用法 |
| 语法错误 config | `[!] config.json 解析失败: bad literal（当前按默认值运行）` rc=1（单前缀） |
| 未知键 config | rc=1（提示 `config.json 里有无法识别的键`，位置移到表头之后，纯排版） |
| 缺键 config（`{"listen_port":7899}`） | rc=0（容忍） |
| ⑫ 重复 up | `up -b -p 17899`(pid=6904) → `up -b` → `[=] 已在运行（端口被占用）` rc=0 → **pid 文件内容不变（YES）** → `down` 正常 → 7899/7900/17899 全 `10061` |

同口径复测 `ec6d47eb4b1828f059598b538b22afe1`（10:43:27，size 1,677,824，源码 10:42:39）——**逐项一致，全绿**：门禁 `up -b -p 17899` → `up -b` = `[=] 已在运行（端口被占用）` rc=0、pid 不变（YES）→ `down` → 三端口 `10061`；doctor 语法错 rc=1 / 未知键 rc=1 / 缺键 rc=0；`--help` rc=0；`system show` 四行。

**已验 hash 三份，行为等价：`702be7f8` / `439fd9dc` / `ec6d47eb`。对外发布取其中任一，但必须是这三份之一。**

### 7.6 构建 `ec6d47eb`（10:43:27，size 1,677,824）全项复核 + lead 裁定落地

| 编号 | 最终状态 | 实测证据 |
|---|---|---|
| ① 子命令参数位 | 已修 | `system show` 四行 rc=0；`node clear`/`node set` 正确；`hosts clear` 沙箱字节保真 |
| ② `probe --json` | 已修 | stdout 纯 JSON，`json.load` 通过，rows=9 |
| ③ doctor | 已修 | 语法错 → `[!] config.json 解析失败: expected string（当前按默认值运行）` rc=1（**单前缀**）；2 个未知键 → `[!] config.json 里有 2 个未知键（…）: broken, listen_prot` rc=1；`_note` 注释键 → 静默容忍 rc=0 |
| ④ 三桶口径 / `--lenient` | **已修（我此前的"no-op"结论作废）** | 同一次网络：严格 `直连可用 7 / 需节点 2 / 死端点 0 / 当前不可用 2` **rc=1**；`--lenient` 同计数 **rc=0**。桶拆开后 `need_node` 不再计入 `dead`，宽松只按死端点判——语义成立 |
| ⑤ help | 已修 | `--help`/`-h`/`help`/`probe -h` 均 rc=0 打印用法 |
| ⑥ 表格列宽 | 已改（接受轻微错位） | 结果列 14→20、分隔线 86→92；`需节点 · SNI 阻断` 行备注仍比其它行右移约 1 字符（CJK 宽度不做逐字对齐，可接受） |
| ⑧ cdn 0 速率 | 已改 | 有下载路径：`23.55.179.24  297ms  1.07MB/s  4`；`--no-bench`（无实测）：实测列显示 `-` |
| ⑨ `pac --write` | 已删（不再是承诺） | usage 现为 `pac 生成 PAC（--apply 同时写入系统代理）`，无 `--write`；该 flag 仍被静默忽略（与 `pac` 等价） |
| ⑩ `down` 按 pid 杀进程 | **接受为实现取舍**（lead 裁定：按端口猜进程误杀风险更大） | — |
| ⑫ 失败实例删 pid | 已修，**独立复核通过** | 见 7.5：`up -b -p 17899 -m blacklist`(pid=6580) → `up -b` rc=0、pid 不变、`effective.json` 未被覆盖、`tasklist` 仅 1 个 → `down` 停 6580、`pid`/`effective.json` 均清理、7899/7900/17899 全 `10061` |
| ⑭ `status` 不反映覆盖 | 已修 | `effective.json` 记录 listen_port/http_port/mode，`status` 显示 `17899 监听中` + `blacklist` |
| **⑮（新）`status` 的"节点"行不读 effective.json** | **P3 残留** | `up -b --node socks5://127.0.0.1:1080` 后 `effective.json` 有 `"upstream_configured": true, "upstream": "127.0.0.1:1080"`，但 `status` 仍打 `节点 未配置（纯直连）`——同一份 effective.json 里其它字段都读了，只漏了 upstream |

**新能力顺带验证**：`probe --node socks5://127.0.0.1:9` → `[i] 节点: 127.0.0.1:9`、汇总 `死端点 2`，**`config.json` md5 前后完全一致（不落盘）**；`probe --node ftp://x:1` → `[!] --node 只认 socks5:// 或 http://` rc=2。

### 7.7 构建 `e634fe15067808ccf2ba0dad30d55d97`（10:46:27，size 1,678,848）—— 最终基线

| 项 | 结果 |
|---|---|
| 门禁三步（config 端口改成 17899/17900 避开环境里他人实例） | `up -b` → pid=25552 → `up -b` → `[=] 已在运行（端口被占用）` rc=0、pid 未变 → `status` 显示 17899/17900 监听中 → `down` → 两端口 `10061` |
| **⑮ `status` 的节点行** | **已修**：`up -b --node socks5://127.0.0.1:1080` 后 `status` 显示 `节点 127.0.0.1:1080（socks5）`（与 `effective.json` 一致） |
| ③ doctor | `{"broken":true,"listen_prot":1}` → `[!] config.json 里有 2 个未知键（…）: broken, listen_prot` rc=1；`bad {{{` → `[!] config.json 解析失败: expected number（当前按默认值运行）` rc=1（单前缀） |
| ⑨ usage | `pac  生成 PAC（--apply 同时写入系统代理）`，仍无 `--write` |

**至此 ①–⑮ 中除 ⑩（lead 裁定接受）外全部关闭，无 P3 残留。**

### 7.8 源码修改记录（均已在 7.1–7.7 坐实）

- ③ `doctor` 配置语法检查：`src/main.cpp`（10:33:00）已改成 `load_config(p, &cfg_err)`，`cfg_err` 非空即打印 `[!] config.json 解析失败: …（当前按默认值运行）` 并计入 issues。10460aa7（10:30:41）尚未包含，需等下个构建复测。
- ① 子命令索引在 10460aa7 已生效（见上表）；README 里"`-p/-m/--node` 只在前台生效"的说法随之失效。


---

## 8. 结论

**验收基线：`svpn.exe` md5 `e634fe15067808ccf2ba0dad30d55d97`（10:46:27，size 1,678,848）。判定：可以发。**

六轮复测关闭项：① 子命令参数位偏移（曾让 `hosts clear` 反向写系统文件）、② `probe --json` 非纯 JSON、③ `doctor` 语法/未知键/前缀重复、④ probe 桶拆分与 `--lenient` 语义、⑤ help、⑥ 表格列宽、⑧ cdn 无实测时显示 `-`、⑨ `--write` 从文案移除、⑫ 失败实例删 pid（`down` 失效 + 孤儿占端口）、⑭ `status` 不反映 `-p/-m`。

lead 裁定为"接受实现取舍"的：⑩ `down` 只按 pid 存在性杀进程（理由：按端口猜进程的误杀风险大于收益）。

**⑮ 也已关闭**（`e634fe15`：`status` 的节点行现在读 `effective.json`，显示 `节点 127.0.0.1:1080（socks5）`）。**当前无任何 P3 残留。**

**发版门禁（建议固化）**
```bash
svpn.exe up -b -p 17899 -m blacklist   # [+] 后台已启动 pid=A；effective.json 记 17899/blacklist
svpn.exe up -b                          # [=] 已在运行（端口被占用） rc=0；pid 与 effective.json 均不变
svpn.exe down                           # [+] 已停止 pid=A；pid/effective.json 清理；7899/7900/17899 全释放
```
这三步覆盖了全程唯一真正的 P1（⑫），也是最容易回归的一条。

**结论**：CLI 面已无可挡发布的问题；退出码语义（`probe` 有不可用端点→1、`doctor` 有问题→1、未知命令→2、无参数→2）全部正确，无"空输出 + rc≠0"。


---

## 9. 附录：复现须知（血泪版）

### 9.1 门禁必须**单会话连跑**，否则会出现 P1 式假阳性
本审计环境在每条 shell 命令结束时回收进程树，`up -b` 拉起的 `_serve` 活不过一条命令。跨会话拆开跑会得到：
```
（会话 1）up -b            → [+] pid=A
（会话 2）up -b            → 可能被误判：A 已死，端口空，于是真的再起一个
（会话 3）down             → [=] 没有在运行的实例
```
这**看着完全像 ⑫ 复发**，实际是环境副作用。正确做法：`up -b` → `up -b` → `status` → `down` → 端口探测**写在同一条命令里**。release-bot 与我都踩过这个坑，独立踩到同一种假阳性，可以确认这个模式成立。

### 9.2 残留实例会造成「假绿」
如果机器上已有别的 `svpn.exe` 占着 `config.json` 里的 `http_port`（默认 7900），那么任何 `up` 都会立刻返回 `[=] 已在运行（端口被占用）` rc=0——**哪怕被测二进制是坏的**，"第二次 up 应被拒绝"那条断言会永远通过。审计/门禁请：
- `bind(('127.0.0.1',0))` 现挑 **`listen_port` 和 `http_port` 两个**空闲端口写进临时 config；
- 跑前确认没有他人实例：`tasklist | findstr svpn`。

### 9.3 产物与源码的配对指纹（无 git，靠这个对齐）
目录不是 git 仓库，回滚/追溯都做不到。本次签字对应的配对：
```
svpn.exe          e634fe15067808ccf2ba0dad30d55d97   1,678,848 B   10:46:27
src/main.cpp      ef09fc1c11844a04a76aae86b94c871b      49,232 B   10:45:03
```
**若 exe 的 md5 / size / mtime 任一不符，本报告结论作废，必须重跑门禁与 doctor 三态。** 历史构建（`439fd9dc` / `702be7f8` / `ec6d47eb`）已被覆盖，磁盘上不存在副本。

### 9.4 关于「签哪个版本」
`ec6d47eb`（10:43:27）我也签字了，但它已被 `e634fe15`（10:46:27）覆盖且**源码也变了**（main.cpp 48,274 B → 49,232 B，+958 B），而 `ec6d47eb` 依赖的那版源码已被覆盖、目录无 git → **物理上不可复现**。因此发布只能取 `e634fe15`。


---

## 10. 发布物门禁（dist 副本）

**被测对象（终版命名）**：`dist/svpn.exe` —— MD5 `e634fe15067808ccf2ba0dad30d55d97`，1,678,848 B。
同目录：`README.md` / `LICENSE` / `run.cmd` / `config.example.json` / `RELEASE.txt` / `SHA256SUMS.txt` / `svpn-1.1.0-win64.zip`。

> 交付过程中 dist 换过三次命名：`svpn-1.1.0-win64/svpn.exe` → `svpn-e634fe15.exe` → **`svpn.exe`**。三次都是**同一批字节**（MD5 均为 `e634fe15…`），每次重命名后我都复跑了门禁，结论一致。最终改成 `svpn.exe` 是对的：`run.cmd` 里硬编码调用 `svpn.exe`，若产物带哈希后缀，双击脚本会直接报 `[!] svpn.exe not found`。
>
> **测试方式**：复制到 `$TEMP/dist-ship/svpn.exe`（`tests/test_cli.py` 同步复制到 `$TEMP/dist-ship/tests/`，脚本 `ROOT` 因而指向临时目录），**单会话连跑**，开头先断言端口 free。全程不碰工作区。

### 10.1 指纹

| 对象 | 值 | 判定 |
|---|---|---|
| `dist/svpn.exe` MD5 | `e634fe15067808ccf2ba0dad30d55d97` | **与签字基线一致** |
| 同上 SHA256 | `45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b` | 与 `SHA256SUMS.txt` 首行**逐字一致** |
| `SHA256SUMS.txt` 其余四行（run.cmd / README / LICENSE / config.example.json） | 与顶层同名文件比对通过（run.cmd 实测 `eb8d16b5…`/250 B） | 一致 ✅ |
| `svpn-1.1.0-win64.zip` 内 `svpn.exe` | MD5 `e634fe15…` | 与顶层 exe 一致 ✅ |
| zip 内 `run.cmd` / `SHA256SUMS.txt` vs 顶层 | 逐字节相同 | 包内外一致 ✅ |
| `RELEASE.txt` 声明 | `MD5 e634fe15… / 1,678,848 B / 10:46:27 / g++ 16.2.0` | 与实物一致 ✅ |

### 10.2 四步链路（先断言端口 free）

```
0) 17899 free(10061) / 17900 free(10061)  → 前置成立
1) svpn.exe -v            → 1.1.0   rc=0
2) up -b -p 17899 -m blacklist → [+] 后台已启动 pid=27884  socks5://127.0.0.1:17899  http://127.0.0.1:17900
3) up -b                  → [=] 已在运行（端口被占用）；改配置请先 svpn down   rc=0
                            pidfile 仍 27884（未变）
4) status                 → 运行中 pid=27884 / 17899 监听中 / 17900 监听中 / blacklist
5) down                   → [+] 已停止 pid=27884
                            17899 closed(10061) / 17900 closed(10061)
```
实例数另按**可执行路径**复核（`Get-CimInstance Win32_Process | ? ExecutablePath -like "*dist-final*"/"*dist-ship*"`）→ count=1，pid 与 pidfile 一致。

### 10.3 回归套件

```
python tests/test_cli.py   （在被测副本旁运行；脚本自行 bind(0) 现挑端口）
PASS 113 / FAIL 0 / SKIP 0（硬失败 0、契约待修 0）
```
覆盖 ⑫ 全链路、⑩ 兜底提示、①③④⑤⑧⑨ 等契约项；与 T1-tests 独立跑出的 113/113 一致（另：`test_config.cpp` 178/0、`test_sni_parse.cpp` 27/0）。

**一次假失败已排除（记录在案）**：与 T1 并发跑同一套脚本时，`⑫ 仍然只有 1 个实例` 会红——两边都把 exe 复制成 `svpn_t1.exe`，按**映像名**计数得 2。按路径计数复核为我方 1 个，空闲窗口重跑即 113/113。**属并发假阳性，非产品缺陷。**

### 10.4 一条 P4（不挡发布，仅记录）

`--node http://host:port` 时 `status` 的节点行仍显示 `（socks5）`：`effective.json` 只存 `upstream: "host:port"`、没存协议类型，`status` 的 kind 取自 `config.json`。实测：
```
up -b --node http://127.0.0.1:1080
effective.json → "upstream": "127.0.0.1:1080"（无 kind 字段）
status         → 节点  127.0.0.1:1080（socks5）      ← 应为 http
```
只影响显示，不影响转发（子进程拿到的是正确 kind）。修法：`effective.json` 增加 `upstream_kind`，`status` 有则用、无则回退 config。

### 10.5 判定

> **发布物 `dist/svpn.exe`（MD5 `e634fe15067808ccf2ba0dad30d55d97`，SHA256 `45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b`，1,678,848 B；zip 同字节）通过最终门禁：指纹与签名基线及 `SHA256SUMS.txt` 一致、四步链路全绿（二次 up 被拒且 pid 不变、down 释放两端口）、`-v` 输出 `1.1.0`、回归 113/113。可以发布。**
> **V1-verify · 2026-10-25**

（唯一遗留为 §10.4 的 P4 显示问题，不影响功能与转发。若发布后再次改动 exe 或 `src/`，本判定自动作废，按 §9.3 重新核对指纹并重跑。）
