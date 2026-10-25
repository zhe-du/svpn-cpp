# V2 分支覆盖补测

## 1. 摘要

1. 锁定副本 `svpn.exe` md5 `6f068265d4e0217c85cb06e28d381aca`（构建于 12:34:39，`-v` = `1.1.1`），全程只跑该副本，仓库根 `svpn.exe` 未被执行过。
2. V1 遗留的 8 个未覆盖分支里，**能安全实测的全部真跑了**：`up` 前台、`pac`、`steam`（失败分支）、`cdn --refresh`、`node` 全系列、`probe` 全开关、`hosts apply/clear`（借 `SVPN_HOSTS_FILE` 缝隙，不碰真实 hosts）。
3. `system on/off`、`--system-proxy`、不带 `--no-elevate` 的 `hosts apply/clear`、`elevate` 路径**只做代码审计**，未执行，原因见 §3.5。
4. 共发现 **16 条新缺陷（3 条 P1 + 13 条 P2）**，最严重的是「前台 `up` 无法优雅停止，且 `down`/`status` 完全看不见前台实例」（D2）。
5. 与 Python 版 CLI 的交叉比对已完成（§3.6）：13 个子命令名一一对应，但 **11 处参数语义不一致**，`pac` 默认行为截然相反，这是自动差分的头号障碍。

> 前提声明：本报告行号取 2026-10-25 12:56 的工作区源码。`src/main.cpp`（12:45:03）、`src/pac.cpp`（12:51:47）在测试期间被其他工人改动，仓库根 `svpn.exe` 已于 12:46:41 重建。**我测的副本早于这些改动**，所以个别条目出现「源码已修、二进制未修」，逐条都已标注，发版前必须以最终产物重跑一遍 §3.1 的第 5、9 条。

## 2. 做了什么

### 2.1 二进制锁定（防并行重建污染）

```
$ mkdir -p /d/LE/AI/wbwrk/wbwrk_tmp/v2 && cp svpn.exe svpn-gui.exe → wbwrk_tmp/v2/
$ md5sum wbwrk_tmp/v2/*.exe
401103a4b94bd6527a870385310058bb  svpn-gui.exe
6f068265d4e0217c85cb06e28d381aca  svpn.exe
$ ./svpn.exe -v
1.1.1
rc=0
```

`Paths::resolve()`（`src/util.cpp:130-146`）跟 exe 目录走，副本在 `wbwrk_tmp/v2/` 下自建了 `config.json` 与 `runtime/`，仓库树零污染。

### 2.2 端口纪律

- 每条命令前 `unset NODE_OPTIONS NODE_EXTRA_CA_CERTS http_proxy https_proxy HTTP_PROXY HTTPS_PROXY`。
- 实例端口全用 19101（SOCKS5）/19102（HTTP）/19103/19104/19105；开跑前 `netstat -ano | grep -E ":(7899|7900) "` 两次确认均无匹配（输出「空闲（无匹配）」）。
- 每条命令记 `rc`；单条命令上限 120 秒，`probe`/`cdn`/`node` 一律带 `--timeout 3` 或 `--no-bench`。

### 2.3 读了哪些代码

| 文件 | 用途 |
|---|---|
| `src/main.cpp`（1195+ 行，全读） | 子命令分发、`cmd_up`/`cmd_serve`/`cmd_down`/`cmd_probe`/`cmd_node`/`cmd_cdn`/`cmd_hosts`/`cmd_system`/`cmd_pac`/`cmd_steam`/`cmd_doctor` |
| `src/winsys.cpp`（812 行，全读） | hosts 托管块、WinINET 接管、`elevate` |
| `src/config.cpp`（400 行，全读） | `load_config` 缺文件/坏 JSON 的回落路径、`save_config` 原子写 |
| `src/net.cpp`（`connect_ip_wait`、`resolve_system`） | 超时语义（D3 的根因） |
| `src/rules.cpp`（`Router::decide`） | 非法 `mode` 的实际后果（D4） |
| `src/pac.cpp` | PAC 生成文案 |
| Python 仓 `svpn/cli.py`、`tests/test_cli_contract.py`（只读） | §3.6 差分评估 |

### 2.4 为覆盖「危险分支」用的安全缝隙

`hosts apply/clear` 本来在禁测名单里（会写真实 hosts / 弹 UAC）。`src/winsys.cpp:286-297` 留了一个测试缝：`SVPN_HOSTS_FILE` 环境变量可以改写目标 hosts 路径。我用它把 `apply_hosts` → `backup` → 原子替换 → `flush_dns` 全链路跑了一遍，目标是临时文件，真实 `C:\Windows\System32\drivers\etc\hosts` 一个字节没动。这是本轮新增覆盖里最有价值的一块（V1 完全没测过）。

## 3. 验证

### 3.1 大表：分支号 → 命令 → rc → 关键输出 → 判定

| # | 分支 | 命令（副本，全带 `--help`/`--timeout 3` 等控时参数） | rc | 关键输出摘要 | 判定 |
|---|---|---|---|---|---|
| 1 | `--help` 矩阵 | 13 个子命令逐个 `X --help` | 13×`0` | stdout 全部 `666` 字节、stderr 全部 `0` 字节；无子命令级 help（所有子命令打同一份全局用法） | 通过（有瑕疵：无子命令 help，见 D13） |
| 1b | `gui --help` | `svpn-gui.exe --help` | — | 会开窗（`wWinMain` → `gui_main()`，`src/main.cpp:1191`） | **未测**（会开 GUI 窗口） |
| 2 | `-v` / `--version` | `-v`、`--version` | `0`/`0` | 均输出 `1.1.1` | 通过 |
| 2b | 无参数 | `svpn.exe` | `2` | 打印完整用法 | 通过 |
| 2c | 未知子命令 | `frobnicate` / `UP` / `""` | `2`/`2`/`2` | `[!] 未知命令: frobnicate`（stdout） | 通过（不静默 rc=0） |
| 3 | `init` 幂等 | `init` ×2（干净目录） | `0`/`0` | 首次写 1049 字节；二次 `[=] config.json 已存在（要覆盖加 --force）`，mtime `12:45:57` 与 md5 `bfa856c8…` 均未变 | 通过 |
| 4 | `doctor` 正常 | `doctor` | `0` | `[+] 没发现问题` | 通过 |
| 4b | `doctor` 坏配置 | `printf '{bad' > config.json; doctor` | `1` | `[!] config.json 解析失败: expected string（当前按默认值运行）` + `[!] 1 个问题待处理` | 通过（V1 ③ 的修复已生效） |
| 4c | `doctor` 缺配置 | `rm config.json; doctor` | `1` | `[!] 缺 config.json，跑 svpn init` | 通过 |
| 5 | **`up` 前台模式** | `up -p 19103`（Python 拉起，等 3 秒后探活） | — | 3 秒后 `poll()=None`（**真阻塞** ✓）；端口可连 ✓；**`runtime/svpn.pid` 不存在、`effective.json` 不存在**；发 `CTRL_BREAK` 后退出码 `3221225786`（=`0xC000013A` STATUS_CONTROL_C_EXIT），日志**无「已停止」** | **缺陷 D2（P1）** |
| 5b | 前台态被 `down`/`status` 感知 | 另一进程 `status` / `down` | `0`/`0` | `进程 未运行` + `端口 127.0.0.1:19102 监听中`（自相矛盾）；`down` → `[=] 没有在运行的实例`，前台进程 `poll()` 仍 `None`（停不掉） | **缺陷 D2（P1）** |
| 6 | `pac` | `pac`、`pac --write` | `0`/`0` | 每次都写 `runtime/svpn.pac`（1714 字节）并只打一行 `[+] 已写入 …`；**stdout 无 PAC 正文**（`grep -c FindProxyForURL` = 0）；两次 md5 相同（`a399ae07…`） | 通过（有瑕疵：D9） |
| 6b | `pac --apply` | — | — | 会写 `HKCU\…\Internet Settings\AutoConfigURL` | **未测**（改全局 WinINET，见 §3.5） |
| 7 | `steam` | `steam --exe 'C:\no\such\steam.exe'` | `1` | `[!] 启动 Steam 失败: 2`（裸 Win32 码，无中文解释） | 通过（有瑕疵：D12）；成功分支未测，见 §3.5 |
| 8 | `cdn --refresh --no-bench` | `cdn --refresh --no-bench --timeout 3` | `0` | 耗时 **4.5 秒**；8 个域名全部选到边缘 IP：`steamcdn-a.akamaihd.net 96.17.178.154 703ms cand=4`、`cdn.akamai.steamstatic.com 23.72.252.82 641ms cand=4`、`client-update… 23.200.230.108 657ms cand=5`、`shared… 23.33.184.231 657ms`、`store.akamai… 95.100.202.104 625ms`、`community… 23.33.184.242 641ms`、`media.steampowered.com 95.100.202.155 687ms`、`avatars.steamstatic.com 151.101.91.52 625ms cand=2` | 通过 |
| 8b | 缓存结构 | 读 `runtime/cdn-cache.json` | — | 顶层键 `['domains','updated']`，`domains` 8 项，`updated=1792904001`；每项含 `best_ip/best_ms/candidates` | 通过 |
| 8c | 读缓存 & 单域名 | `cdn`、`cdn --domain store.akamai.steamstatic.com` | `0`/`0` | 不 refresh 直出缓存表；单域名只打一行 | 通过 |
| 9 | `node show` | `node show` | `1` | **静默执行 `scan`**：`[i] 扫描本机常见代理端口… / [!] 没找到可用节点`，无「未知动作」提示 | **缺陷 D10（P2）** |
| 9b | `node set` 假上游 | `node set socks5://127.0.0.1:19999` | `1` | `[!] 节点不可用: 连接 127.0.0.1:19999 失败`；`cmp` 确认 config.json **未改动** | 通过 |
| 9c | `node set` 非法值 | `ftp://x` / 空 / 无端口 / `:70000` | `2`×4 | `只支持 socks5:// 或 http://` / `用法: …` / `缺少端口` / `端口越界` | 通过 |
| 9d | `node set` 自环 | `node set socks5://127.0.0.1:19101` | `1` | `[!] 这个端口就是 svpn 自己，会形成自环` | 通过 |
| 9e | `node clear` | `node clear` | `0` | `[+] 已清空节点…`；config 里 `host` 写回占位符 `NODE_HOST`（`configured()` 判false，等价清空） | 通过 |
| 9f | `node scan --json` | `node scan --json` | `1` | `--json` 被静默忽略，输出仍是人读文本（Python 版有 `--json`） | **缺陷 D11（P2）** |
| 10 | `probe --json` 纯 JSON | `probe --json --timeout 3` | `1` | stdout **1266 字节纯 JSON**，`json.load()` 通过：`keys=['ok','rows','unusable']`、`rows=9`；人读表格与 WARN 全部走 stderr | 通过（V1 ② 的修复已生效） |
| 10b | `probe --lenient` | `probe --lenient --timeout 3` | `0` | 无节点时 `dead=0` → rc 0，符合契约 | 通过（V1 ② 的修复已生效） |
| 10c | `probe --node none` | `probe --node none --json --timeout 3` | `1` | `unusable=2 ok=False rows=9` | 通过 |
| 10d | 默认打表 9 端点 | `probe --timeout 3` | `0`/`1` | 9 行齐全，分桶 `直连可用 8 / 需节点 1 / 死端点 0 / 当前不可用 1` | 通过（但表格列错位，D6） |
| 10e | `probe` 行序稳定性 | 连跑两次 `--json` | `1`/`1` | run1 前 3 行 `[avatars, community.akamai, cdn.akamai]`；run2 `[avatars, community.akamai, steamcdn-a]` → **顺序不一致** | **缺陷 D5（P2）** |
| 10f | `probe --timeout 0.5` | `probe --timeout 0.5 --json` | `1` | **耗时 64.4 秒**（默认 6 秒的 10 倍），无任何提示 | **缺陷 D3（P1）** |
| 10g | `probe --node auto` | `probe --node auto --timeout 3` | `2` | `[!] --node 只认 socks5:// 或 http://`（Python 版 `auto` 是合法值） | **缺陷 D11（P2）** |
| 11 | `status` 未运行态 | `status` | `0` | 逐行给人话：`进程 未运行` / `端口 … 未监听` / `节点 未配置（纯直连）` | 通过 |
| 11b | `down` 未运行态 | `down` | `0` | `[=] 没有在运行的实例` + `[!] 系统代理没动过：没有备份（可能从没接管过）` | 通过（有瑕疵：D8） |
| 12 | **端口冲突** | Python 先占 `19102`，再 `up -b`（config: 19101/19102） | **`0`** | `[=] 已在运行（端口被占用）；改配置请先 svpn down`；此时**根本没有 svpn 在跑**（`runtime/svpn.pid` 不存在、无日志） | **缺陷 D1（P1）** |
| 13 | 二次 `up -b` | `up -b` ×2 | `0`/`0` | 二次 `[=] 已在运行（端口被占用）`；pid `34272` → `34272`（不变），两端口仍由同一 pid 监听 | 通过 |
| 14 | 配置缺失下 `status` | 删 config 后 `status` | `0` | 直接用默认值出全套状态（7899/7900/whitelist），**既不自动 init 也不提示「配置缺失」** | 通过（有瑕疵，见 §3.4-①） |
| 14b | 配置缺失下 `up -b` | — | — | 会按默认 config 绑 **7899/7900**，SPEC §5 明令禁止占用 | **未测**（审计结论见 §3.5） |
| 15 | `up -b` 真实代理 | `up -b` + curl 经 19101/19102 | `0` | `http://example.com` 经 HTTP 代理 **200**（0.61s）；`https://example.com` CONNECT **200**（2.76s）；经 SOCKS5 取 http **200**（0.60s） | 通过 |
| 15b | 上游不可达时的失败语义 | 同上，打 `https://store.steampowered.com` | — | 一次 `< HTTP/1.1 502 Bad Gateway`（立即）；另两次 15 秒无响应（curl rc=28）。本机系统 DNS 把该域解析到 `184.29.46.118`（SYN 被黑洞），curl 自己解析到 `2.22.122.121` 才 200 | 通过（环境相关，见 §3.4-②） |
| 16 | `hosts apply`（缝隙） | `SVPN_HOSTS_FILE=…\fake-hosts hosts apply --no-elevate` | `0` | `[+] hosts 已写入 8 个域名 -> 8 个最优边缘 IP` 并逐条列出；托管块用 `# >>> steam-vpn managed block` 包裹；原 CRLF 与中文注释**逐字节保留**；备份落 `runtime/hosts.backup`（49 字节） | 通过（V1 从未覆盖） |
| 16b | `hosts clear`（缝隙） | `hosts clear --no-elevate` | `0` | 清理后 md5 与原始文件**完全一致**（`1fba080870f176a5903c2e016333c1da`） | 通过 |
| 16c | `status` 读回 hosts | `status \| grep hosts` | `0` | `hosts    8 条 svpn 记录` | 通过 |
| 16d | `down --clear-hosts`（缝隙） | `down --clear-hosts` | `0` | `[i] hosts 已清理`，临时 hosts 只剩 `127.0.0.1 localhost` | 通过 |
| 16e | 非管理员清真实 hosts | 不设缝隙、`hosts clear --no-elevate` | `1` | `[!] 清理失败: 需要管理员权限写 hosts` + stderr `WARN 写 hosts 被拒绝: 错误码 5 (拒绝访问。)`；真实 hosts 未被修改 | 通过（人话清楚） |
| 17 | `system show` | `system show` | `0` | `ProxyEnable 0 / ProxyServer (空) / ProxyOverride (空) / AutoConfigURL (空)` | 通过（只读，安全） |
| 17b | `system` 未知动作 | `system bogus` | `2` | `[!] 用法: svpn system [on\|off\|show]` | 通过 |
| 18 | 非法 `mode` | `up -b -p 19104 -m garbage` | `0` | **启动成功**，`effective.json` 里 `"mode": "garbage"`，打印 `[i] 上游: 无（纯直连，garbage）` | **缺陷 D4（P2）** |

### 3.2 缺陷清单

严重级别定义：P0 = 数据损坏/安全事故；P1 = 主路径功能错误或无法恢复；P2 = 可用性/一致性问题，有绕行办法。

#### D1（P1）端口被别的程序占用时，`up -b` 谎报「已在运行」并返回 0

- **复现**：`python -c "socket.bind(('127.0.0.1',19102)); listen; sleep"` 占住 HTTP 端口 → `svpn up -b`（config: listen 19101 / http 19102）。
- **期望**：指出是哪个端口被占、占用者不是 svpn，返回非 0。
- **实际**：`[=] 已在运行（端口被占用）；改配置请先 svpn down`，**rc=0**。此时没有 svpn 进程、没有 pid 文件、没有日志。用户按提示跑 `svpn down` 会得到「没有在运行的实例」，然后回到同一条错话——死循环。
- **定位**：`src/main.cpp:255-261`。SOCKS5 与 HTTP 端口被合并成一个判断，且无论占用者是谁都套用「已在运行」的文案并 `return 0`；缺少 `read_pid()` 交叉验证（`cmd_down` 在 `src/main.cpp:453-459` 里做了这件事，`cmd_up` 这里没做）。
- **影响**：真·启动失败被伪装成成功，自动化脚本（`if svpn up -b; then …`）会继续往下走。

#### D2（P1）前台 `up` 无法优雅停止，且 `down`/`status` 完全看不见它

- **复现**：`svpn up -p 19103`（前台，故意不带 `-b`），3 秒后从另一进程发 `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pgid)`。
- **期望**：按用法文本「（Ctrl+C 退出）」走优雅退出，日志出现「已停止」，统计落盘。
- **实际**：退出码 `3221225786`（`0xC000013A` = STATUS_CONTROL_C_EXIT，CRT 默认的 Ctrl+C 硬终止），日志无「已停止」、无统计。另有两个连带症状：
  1. 前台实例**不写 `runtime/svpn.pid`、不写 `runtime/effective.json`** → 同一时刻另开终端 `svpn down` 只会说「没有在运行的实例」，**根本停不掉**，只能去任务管理器；`svpn status` 会输出自相矛盾的「进程 未运行」+「端口 19102 监听中」。
  2. `-p` 只覆盖 `listen_port`，`http_port` 仍取 config，所以前台实例的 HTTP 端口与 SOCKS5 端口不同源，`status` 按 config 的 19101 去判断 SOCKS5、按 19102 判断 HTTP，两个端口一真一假。
- **定位**：前台分支 `src/main.cpp:263-275` 只调了 `log_init` / `serve_all`，**既没有 `write_pid` 也没有 `SetConsoleCtrlHandler`**；这两件事都只在 `cmd_serve` 里做（`src/main.cpp:385`、`src/main.cpp:405`）。另外 `src/main.cpp:267` 传给 `serve_all` 的 `stop` 是局部 `std::atomic<bool>`，全流程无人置位——即使注册了 handler，`serve_all` 也收不到停止信号（对比 `cmd_serve` 传的是全局 `g_stop`，`src/main.cpp:335-342`、`src/main.cpp:431`）。
- **影响**：默认启动路径（不带 `-b`）的用户，Ctrl+C 后统计丢失、日志无收尾记录；更关键的是前台实例处于「管理盲区」，`down` 与 `status` 都给不出真相。这是本轮**最严重**的一条。

#### D3（P1）`--timeout` 传小数被静默截断为 0，导致无限等待

- **复现**：`svpn probe --timeout 0.5 --json`（Python 版 `--timeout` 是 `float`、默认 `4.0`，照着写就踩）。
- **期望**：0.5 秒超时，9 个端点并行，1 秒内出结果；或明确拒绝非法值。
- **实际**：**耗时 64.4 秒**才返回（rc=1），无任何提示。原因是 `Args::num` 用 `std::stoi`（`src/main.cpp:188-196`），`"0.5"` → `0`；而 `0` 在 `src/net.cpp:163` 被解释为**无限等**：
  ```cpp
  const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
  ```
  同一条路径还影响 `cdn --timeout 0.5`、`hosts --timeout 0.5`（`std::stoi` 同一实现）。
- **影响**：用户按 Python 文档写小数超时，命令从 6 秒变成「挂到 DNS/连接自己放弃为止」，看起来就是卡死。

#### D4（P2）非法 `mode` 被静默接受

- `svpn up -b -p 19104 -m garbage` → **rc=0，启动成功**，`effective.json` 记 `"mode": "garbage"`。
- Python 版 `-m` 带 `choices=["whitelist","all","blacklist"]`（`svpn/cli.py:1243`），非法值由 argparse 直接 rc=2 拒掉。
- 后果由 `Router::decide` 兜底：`src/rules.cpp` 第 6 步只认 `all`/`blacklist`，其它一律落到第 7 步 `return "direct"` —— 即**静默退化成 whitelist 行为**，用户以为换了模式其实没换。
- 定位：`src/main.cpp:238-239`（`-m`/`--mode` 直接赋值，无枚举校验）。

#### D5（P2）`probe` 输出行序不稳定

- 连跑两次 `probe --json`，`rows` 的 host 顺序不同（run2 第 3 项从 `cdn.akamai.steamstatic.com` 变成 `steamcdn-a.akamaihd.net`）。
- 定位：`src/main.cpp:649-655`，9 个线程各自 `rows.push_back()`，向量顺序 = 线程完成顺序，而非 `kEndpoints` 声明顺序（`src/main.cpp:30-40`）。
- 后果：① 人读表格每次顺序都在跳；② 落盘的 `runtime/probe.json` 每次不同，`git diff` 噪音；③ **与 Python 版逐字比对不可能**（见 §3.6）。

#### D6（P2）`probe` 表格按字节补空格，CJK 列错位

- 实测一行：`steamcommunity.com                     需节点 · SNI 阻断 社区 / 创意工坊        0ms` —— 「备注」列被挤到「结果」列里，两列贴在一起。
- 定位：`src/main.cpp:661-666` 用 `fmt("%-38s %-20s %-22s %8s")`，`printf` 的宽度按**字节**算，一个汉字 3 字节、显示只占 2 列，于是「需节点 · SNI 阻断」这类 20 字节的串被当成正好填满、实际显示 12 列。`src/main.cpp:659-660` 的注释认为「20 够宽了」，但那解决的是字节预算，不是显示对齐。
- 影响：纯观感，但每个用户都会看到，且 1.1.1 正在补文档截图。

#### D7（P2）`init` 不备份系统代理，导致首次 `down` 必打警告

- `svpn init` 只写 config（`src/main.cpp:216-231`，全程无 `backup_wininet`）。Python 版 `init` 会 `sysx.backup_wininet(RUNTIME)` 并打印「系统代理已备份」（`svpn/cli.py::cmd_init`）。
- 后果：全新安装后第一次 `svpn down` 必然输出 `[!] 系统代理没动过：没有备份（可能从没接管过）`——我实测每次 `down` 都出现。
- 定位：`src/main.cpp:216-231` vs `src/main.cpp:473-480`（`down` 的 no-backup 分支）。

#### D8（P2）`down` 在「本来就没接管过」时用 `[!]` 级别报警

- 定位：`src/main.cpp:473-480`。「没有备份」是完全正常的初始状态，却每次都以 `[!]` 输出两行（含 `当前 ProxyEnable=0 `，行尾还带一个空格）。
- 影响：噪音，且真正的异常（备份文件损坏）与「从未接管」共用一个级别。

#### D9（P2）`pac` 与 Python 版默认行为相反；PAC 文件自述了一个不存在的开关

- Python：`pac` 不带参数**把 PAC 正文打到 stdout**，`pac --write` 才写文件（`svpn/cli.py::cmd_pac`）。
- C++：`cmd_pac` 无条件写文件、stdout 只有一行 `[+] 已写入 …`，**没有任何模式能打印正文**（`src/main.cpp:966-990`）；`--write` 不是开关，传了也被忽略。
- 更糟的是我测的副本生成的 PAC 首行写着 `重跑 \`svpn pac --write\` 覆盖`——一个不存在的开关。**注意**：`src/pac.cpp:93` 已在 12:51:47 改成 `重跑 \`svpn pac\` 覆盖`，但我测的 12:34 构建里还是旧文案，仓库根 12:46:41 的重建**早于**这次源码修改，所以**发版前必须再编一次**，否则 dist 里仍是错的。
- 定位：`src/main.cpp:966-990`、`src/pac.cpp:93`。

#### D10（P2）`node` 的未知动作被静默当成 `scan`

- `svpn node show` → 直接开始扫端口（rc=1，3.4 秒），既不报「未知动作」，也不提示合法值。
- 定位：`src/main.cpp:714`（`action` 只有 `clear`/`set` 两个 if，其余全部落到 scan 的尾部逻辑），用法文本只写了 `scan | set <url> | clear`。
- 后果：用户以为在看当前节点，实际触发了一次 16 端口 × 2 协议的扫描。

#### D11（P2）与 Python 版 CLI 的参数缺口（差分硬冲突）

| 参数 | Python | C++（副本实测） |
|---|---|---|
| `probe --node auto` | 合法（自动发现节点） | **rc=2 拒绝**：`[!] --node 只认 socks5:// 或 http://` |
| `probe --timeout` | `float`，默认 `4.0` | `int`，默认 `6`；小数被截断（见 D3） |
| `up --no-auto` | 有 | 无（传了被忽略） |
| `node --json` | 有 | 无（实测被忽略，输出仍是人读文本） |
| `cdn --json` | 有 | 无 |
| `cdn --timeout` | 默认 `3.0` | 默认 `8` |
| `hosts --timeout` | 默认 `3.0` | 默认 `8` |
| `pac --write` / `pac`（打正文） | 有 / 有 | 无 / 无（见 D9） |
| `steam --restart --no-autostart --args` | 有 | 无 |
| `system`（不带动作） | 必填参数，缺了 rc=2 | 默认为 `show`，rc=0 |
| `-m/--mode` 取值校验 | argparse `choices` → rc=2 | 不校验（见 D4） |

#### D12（P2）`steam` 启动失败只给裸 Win32 错误码

- `steam --exe 'C:\no\such\steam.exe'` → `[!] 启动 Steam 失败: 2`，rc=1。
- 同一份代码里 `winsys.cpp` 已有 `win_err_text()`（`src/winsys.cpp:53-63`，会输出「错误码 2 (系统找不到指定的文件。)」），这里没用。
- 定位：`src/main.cpp:1023`。

#### D13（P2）用法文本漏掉第 14 个子命令 `gui`

- `print_usage()`（`src/main.cpp:1105-1119`）列了 13 项，没有 `gui`；而 `svpn gui` / `svpn --gui` 是 1.1.1 的头号新功能（`src/main.cpp:1200`）。
- 实测：13 次 `--help` 输出全部 666 字节，`grep gui` 无匹配。
- 后果：新功能不可发现；`svpn --help` 与 README 描述的 14 项对不上。

#### D14（P2）`system on` / `--system-proxy` 应用失败时没有回滚

- 路径：`cmd_system("on")` → `backup_wininet` → `apply_wininet`，失败只打印 `[!] 设置失败: …` 并 `return 1`（`src/main.cpp:953-960`）；`--system-proxy` 在 `cmd_serve` 里同样只打印不修（`src/main.cpp:363-374`）。
- 危险点在 `apply_wininet`（`src/winsys.cpp:695-703`）：四个 `RegSetValueEx` 用 `&&` 串起来——**前一个成功、后一个失败时就停在半套状态**。最坏序列是 `AutoConfigURL` 已被清空、`ProxyEnable` 已置 1、`ProxyServer` 写失败：浏览器会带着「启用代理但没有服务器」的状态跑，正是「指向死代理」的那种情形。失败分支里没有任何「用备份回滚」的调用（备份在 `backup_wininet` 里已经存好了，就是没用）。
- 这条只做审计，未实测（见 §3.5）。

#### D15（P2）`hosts apply` 被用户拒绝 UAC 时静默失败

- 非管理员、不带 `--no-elevate`：打印 `[i] 写 hosts 需要管理员，正在弹 UAC…` → `elevate(...)` 返回 false（用户点「否」时 `ERROR_CANCELLED` 被 `src/winsys.cpp:798` 吞掉）→ `return hosts_managed_entries(p) > 0 ? 0 : 1`。
- 实测等价路径（`--no-elevate` + 无权限）rc=1 且只有一句 `需要管理员权限写 hosts`；带提权时**连这句都没有**，只有 rc=1 和静默。
- 定位：`src/main.cpp:872-876`（CDN 直连 IP 分支）、`src/main.cpp:913-917`（SNI 分支）。

#### D16（P2）后台运行时无法读日志

- `up -b` 运行期间，另一个进程 `tail runtime/svpn.log` → `tail: cannot open 'runtime/svpn.log' for reading: Device or resource busy`；`down` 之后立刻可读。
- 定位：`src/main.cpp:355-360`，`_wfreopen_s` 把 stdout/stderr 都绑到日志文件，CRT 默认共享模式不允许外部读。`src/main.cpp:376-380` 的注释说「别再让 log_write 每行重开一次文件（两个流抢同一个文件会互相锁住，外部 tail 直接 resource busy）」——重开的问题确实修了，但**文件被独占**这个现象仍在。
- 影响：用户/支持人员排查「为什么连不上」时，唯一的信息源在进程活着时打不开。

### 3.3 实测数据附录：代理链路端到端

```
$ ./svpn.exe up -b            # config: listen 19101 / http 19102，无上游，whitelist
[+] 后台已启动 pid=23672  socks5://127.0.0.1:19101  http://127.0.0.1:19102
rc=0

A http://example.com   经 HTTP 代理   → 200  t=0.610s
B https://example.com  经 HTTP CONNECT → 200  t=2.760s
C https://store.steampowered.com 经 HTTP 代理 → 15s 无响应（curl rc=28）
D https://store.steampowered.com 经 SOCKS5    → 15s 无响应（curl rc=28）
E http://example.com   经 SOCKS5     → 200  t=0.602s

$ cat runtime/stats.json
{ "uptime_s": 32, "connections": 4, "errors": 1, "via_upstream": 0, "via_direct": 4,
  "up_mb": 0.000628, "down_mb": 0.006429,
  "top_hosts": [["example.com",2],["store.steampowered.com",2]] }
```

C/D 的超时不是 svpn 的确定缺陷：本机系统 DNS 把 `store.steampowered.com` 解析到 `184.29.46.118`（该 IP 的 SYN 被黑洞，python 直连 5 秒超时），而 curl 自带解析拿到 `2.22.122.121` 才 200。svpn 走 `resolve_system`（系统 DNS），必然拿到污染结果。重跑同样命令时又拿到 `< HTTP/1.1 502 Bad Gateway`（立即返回），说明失败语义本身是对的，只是耗时随目标 IP 的丢包情况在 0.6s～>15s 之间波动。**不计入缺陷，记入 §5 风险。**

### 3.4 两条判「通过但有瑕疵」的说明

① **配置缺失下的 `status`**：`load_config` 对不存在的 config 直接回落 `merge_config(nullptr)`（`src/config.cpp:337`），于是 `status` 用 7899/7900 打出一份「像真的」的状态，不提示配置缺失，也不自动 `init`。对比 `doctor` 在同样情况下会明确说「缺 config.json，跑 svpn init」（rc=1）。不崩、不误导到无法排查的程度，但 `status` 与 `doctor` 的口径不一致。

② **代理链路**：见 §3.3。HTTP/SOCKS5 对可达目标完全正常（4 个方向 3 个 200），失败时也能给出 `502 Bad Gateway`。不计缺陷。

### 3.5 未实测的分支：为什么没测 + 代码审计结论 + 行号

| 分支 | 为什么没测 | 代码审计结论 | 行号 |
|---|---|---|---|
| `system on` | 直接改 `HKCU\…\Internet Settings`（`ProxyEnable=1` + `ProxyServer=127.0.0.1:<port>`），一旦写坏且无回滚，用户浏览器立刻全局断网；并行测试期更不能动 | `backup_wininet` → `apply_wininet`，失败分支**无回滚**（见 D14）。成功路径输出 `[+] 系统代理已指向 127.0.0.1:<port>`，失败输出 `[!] 设置失败: <reason>` 且 rc=1 | `src/main.cpp:953-960`；`src/winsys.cpp:655-706` |
| `system off` | 会按备份**改写**注册表；本机从没接管过（无备份），跑了只会走 no-backup 分支，覆盖价值低 | 无备份时：`[!] 没有备份；当前 ProxyEnable=%d` + rc=1（**不猜默认值、不动注册表**，这一点是对的）；有备份时按 `ProxyEnable/ProxyServer/ProxyOverride/AutoConfigURL` 四项原样写回 | `src/main.cpp:943-952`；`src/winsys.cpp:708-762` |
| `up --system-proxy` | 同 `system on`（后台子进程 `_serve` 里执行） | `cmd_up` 把 `--system-proxy` 透传给子进程（`src/main.cpp:281`），`cmd_serve` 里 `backup_wininet` → `apply_wininet` → 成功/失败都只打印一行、**进程继续跑**（失败也不退出、不回滚）。也就是说「系统代理没接管成功」时，用户看到的是 `[!] 系统代理设置失败: …` 但 `up -b` 仍然 rc=0 | `src/main.cpp:281`、`src/main.cpp:363-374`；`src/winsys.cpp:681-706` |
| `hosts apply`（不带 `--no-elevate`） | `elevate()` → `ShellExecuteExW(lpVerb="runas")` → **`WaitForSingleObject(INFINITE)`**，无人值守会永久挂起；且提权后的子进程会真写 `System32\drivers\etc\hosts` | 非管理员分支：打印 `[i] 写 hosts 需要管理员，正在弹 UAC…` → `elevate("hosts apply --no-elevate")` → 用 `hosts_managed_entries(p) > 0 ? 0 : 1` 反推成功与否。用户拒绝 UAC 时 `ERROR_CANCELLED` 被静默吞掉，**没有任何失败说明**（见 D15）。写入本身已用 `SVPN_HOSTS_FILE` 缝隙完整验证（§3.1 第 16 条），字节级往返正确 | `src/main.cpp:872-886`、`src/main.cpp:913-930`；`src/winsys.cpp:778-810` |
| `hosts clear`（不带 `--no-elevate`） | 同上 | `src/main.cpp:851-862`：先 `is_admin()` 判断，非管理员且未给 `--no-elevate` 就提权；提权返回值被丢弃，仍按 `hosts_managed_entries(p) == 0 ? 0 : 1` 给 rc | `src/main.cpp:851-862` |
| `down --clear-hosts` 的提权分支 | 同上 | `src/main.cpp:480-494`：`hosts_managed_entries > 0` 才清；非管理员则 `elevate("hosts clear --no-elevate")`，**返回值同样被丢弃**，函数最后无条件 `return 0` | `src/main.cpp:480-494` |
| `doctor` 的管理员分支 | 本机非管理员，无法切换 | 「管理员」在 `doctor` 里只影响一行输出（`[i] 管理员: 是/否（写 hosts 需要提权）`），**没有管理员专属检查项**——即所谓「doctor 管理员分支」实际上不存在 | `src/main.cpp:1100` |
| 配置缺失下 `up -b` | 会用默认 config 绑 **7899/7900**，SPEC §5 明令禁止占用这两个端口 | `load_config` 缺文件 → `merge_config(nullptr)` → `default_config()`（7899/7900），直接起服务；**不自动 init、不提示、不写 config.json**，退出后目录里仍然没有 config.json | `src/config.cpp:333-337`、`src/config.cpp:209-236` |
| `steam` 成功分支 | `D:\app\Bapp\st\steam.exe` 存在，跑成功分支会真的拉起 Steam 客户端（会抢焦点、可能触发更新），不适合无人值守 | `find_steam()` 按 3 个硬编码路径找（`D:\app\Bapp\st\steam.exe` / `C:\Program Files (x86)\Steam\steam.exe` / `C:\Program Files\Steam\steam.exe`），找不到 → `[i] 没找到 steam.exe（返回 2，属于正常）` rc=2。**注意硬编码了作者本人的 D 盘路径**，作为发布版首位候选路径不合理 | `src/main.cpp:993-1003`、`src/main.cpp:1005-1030` |
| `pac --apply` | 写 `AutoConfigURL`，改全局 WinINET | `backup_wininet` → `set_autoconfig_url("file:///…/runtime/svpn.pac")`，成功后提示「浏览器重启后生效」；失败 rc=1 且**不回滚已备份的状态**（与 D14 同源） | `src/main.cpp:977-989`、`src/main.cpp:74-87` |
| `gui`（窗口模式） | 会开窗，无法在无头环境判定 | 未审计 GUI 逻辑（不属本任务范围）；仅确认 `svpn gui` / `svpn --gui` 在 `wmain` 里先于 dispatch 拦截 | `src/main.cpp:1193-1202`（拦截在 1200） |

### 3.6 附：与 Python 版做自动差分的可行性评估（只评估，未写代码）

**现状**：两侧子命令**名称完全一致**（13 个：`init/up/_serve/down/status/probe/node/cdn/hosts/system/pac/steam/doctor`，C++ 另有 `gui`）。Python 侧的 `tests/test_cli_contract.py:37-50` 里有一张现成的 `SUBCOMMANDS` 表，逐子命令给出了代表性 argv，可以直接拿来当差分用例集的种子。

**一一对应的部分**：子命令名、`init --force`、`up -b/--background`、`down --keep-system/--clear-hosts`、`system on|off|show`、`cdn --refresh/--no-bench/--domain`、`hosts apply|clear`、`hosts --no-elevate`、`probe --json/--lenient`、`status`、`doctor`。

**不一致项（这是本节最有价值的产出，共 11 处）**：见 §3.2 D11 的表。其中三条会直接让差分脚本红：

1. **`pac` 语义相反**：Python 默认打正文、`--write` 才写文件；C++ 永远写文件、从不打正文、`--write` 无效。任何比对 stdout 的用例在这一条上必然失败。
2. **`probe --timeout` 类型不同**：Python `float`（默认 4.0）对 C++ `int`（默认 6，小数截断为 0 → 无限等待，D3）。差分跑 `--timeout 0.5` 会让 C++ 侧单条用例挂 60 秒以上。
3. **`probe --node auto`**：Python 合法值，C++ rc=2 直接退出。

**四个实际障碍**：

| 障碍 | 具体表现 | 建议解法 |
|---|---|---|
| 路径不可注入 | C++ `Paths::resolve()` 死绑 exe 目录（`src/util.cpp:130-146`），CLI 无 `--config/--runtime`；Python `ROOT = Path(__file__).parent.parent`（`svpn/cli.py:32-34`）也是包位置。两侧都只能靠**挪文件**隔离 | 两侧各复制一份到临时沙箱（C++ 复制 `svpn.exe`，Python 复制整个 `svpn/` 包 + `config.json`），我用过这个办法，可行 |
| 端口固定 | 默认 7899/7900，且**两侧都没有 CLI 覆盖 `http_port` 的开关**（`-p` 只改 `listen_port`）；同机并行会互抢 | 沙箱里先改写各自 `config.json` 的 `listen_port`/`http_port` 到不同高位端口，再跑；同一时刻只跑一侧 |
| 输出不可比 | C++ 是中文 `[+]/[i]/[!]` 前缀 + `fmt` 表格；Python 是 `OK/INFO/EQ` 符号 + `print`。文案本身**没有对齐契约** | 只比 `(rc, 归一化后的 JSON)`；文案层不做断言。可比的 JSON 面目前只有 C++ 的 `probe --json`（§3.1 第 10 条已验证纯净），`cdn --json`/`node --json` 在 C++ 侧还缺（D11） |
| 非确定性 | C++ `probe` 行序不稳定（D5）；`probe`/`cdn` 结果依赖当场网络与 DNS 污染；`--timeout` 抖动 | 先修 D5（按 `kEndpoints` 顺序输出）再上差分；联网用例标记为 nightly，CI 只跑离线子集 |

**建议方案（不写代码，只给路线）**：

1. **第一步不是写脚本，是先把「可差分面」定死**：把 D11 里 11 处参数差异逐条决策（补实现 / 明确记为不兼容），并只对 `status`、`doctor`、`probe --node none --json`、`init` 这四个**离线且有结构**的命令做差分。这四条覆盖了配置归一、健康检查、端点分桶、初始落盘四个最容易漂移的面。
2. 第二步再补 `pac`（要么 C++ 加「无参数打 stdout」，要么把 Python 改成默认写文件——**必须先统一，否则差分永远是红的**）。
3. 第三步才上「起实例 + 真实代理」的 e2e 差分，且必须串行、必须用沙箱 config、必须允许 UDP/网络抖动差异（只断言「200 vs 非 200」，不断言耗时）。
4. `_serve`、`gui`、`hosts apply`（提权）、`system on/off` 建议**永久排除**在自动差分之外：它们是内部命令或会改全局状态，两侧都不该在测试里真跑。

## 4. 契约变更

无。本轮只新建 `_crew/reports/V2-branch-coverage.md` 一个文件；未改 `src/`、`tests/`、`docs/`、`README*`、`.gitignore`、`build.cmd`，未改 Python 仓（全程只读）。

对他人有影响的信息（不需要别人改代码，但发版时要看）：
- **D9 的文案存在「源码已修、二进制未修」状态**：`src/pac.cpp:93` 于 12:51:47 已改为 `svpn pac`，而仓库根 `svpn.exe`（12:46:41）与我的副本（12:34:39）里都还是 `svpn pac --write`。**打包前必须重编**，否则 dist 里的 PAC 首行仍指向不存在的开关。
- 我测的副本建于 `src/main.cpp` 与 `src/pac.cpp` 最后一次修改之前，Lead 若在最终产物上复跑，第 5、9、12 条（`up` 前台 / `pac` 文案 / 端口冲突）需要重测一遍。

## 5. 没做完 / 风险

1. **7 类分支未实测**（`system on/off`、`--system-proxy`、`hosts apply/clear` 的提权路径、`down --clear-hosts` 提权、`pac --apply`、`gui`、`steam` 成功分支），原因逐条写在 §3.5。其中 `system on`/`--system-proxy` 的回滚缺失（D14）是**只靠读代码得出的结论，没有实测佐证**，建议发版说明里保留「首次使用建议先 `svpn init` 备份」的提示。
2. **D2 只验证到「退出码是 0xC000013A、日志无已停止、down 停不掉」**，没有在真实交互式控制台里按物理 Ctrl+C 复现（用的是 `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT)`，与 Ctrl+C 在 `ctrl_handler` 里走同一分支，`src/main.cpp:336-342`），置信度高但严格说不是真按键。
3. **`probe`/代理链路的耗时数据受本机 DNS 污染影响大幅波动**：同一命令有时 0.6 秒 200、有时 15 秒超时、有时立即 502。§3.3 的 502 与超时都如实记录了，但**不建议**把耗时写进任何验收标准。
4. **`cdn --refresh` 只跑了 `--no-bench`**。带吞吐实测（1MB 下载）的完整路径没跑——单域名串行 8 个域名、每个都下 1MB，很容易超过 120 秒上限，且会给 CDN 造成无谓流量。`--no-bench` 已经覆盖了 DoH 解析 + 候选枚举 + TLS 竞速三段主干，吞吐分支（`rank_domain(bench=true)`）属于未覆盖。
5. **`node scan` 在「本机真的有可用代理」时的成功分支没覆盖**：本机 16 个常见端口全关，实测走的是 `found.empty()` → rc=1 分支（`src/main.cpp:797-803`）。成功分支（`src/main.cpp:799-805` 的列表输出）只有代码审计，未实测。
6. **D16（后台日志被独占）只做了一次观察**，未确认是 CRT 默认共享模式还是 `_wfreopen_s` 的附加语义导致，也没验证「以只读方式打开能否成功」。根因定位不够精确，但现象可稳定复现（`up -b` 运行中 `tail` 必报 `Device or resource busy`，`down` 后立刻可读）。
7. 报告行号对应 12:56 的源码快照；`src/main.cpp` 本轮仍在被 W1 修改，若有 +/- 几行的漂移，以函数名（`cmd_up` / `cmd_probe` / `cmd_node` 等）为准。

---

**一句话结论：本轮实测 + 审计共发现 16 条新缺陷（3 条 P1 + 13 条 P2，D1–D16），最严重的是 D2「前台 `up` 无法优雅停止，且 `down`/`status` 完全看不见前台实例」。**
