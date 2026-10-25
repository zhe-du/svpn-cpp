# svpn / Steam 分流器 · 全项目总日志

- 汇总时间：2026-10-25 12:30（Asia/Shanghai）
- **修订：2026-10-25 13:20** —— 补入 1.1.1 发版轮（新增 §2.6、§4.5；重写 §3、§5.4、§6.4、§8.3）；
  修订 §1「五条硬事实」中已被本轮实测推翻的两条。发版前的快照不再适用，以此版为准。
- **本文已归档进仓库**：`svpn-cpp/_crew/PROJECT-LOG.md`（随 tag `v1.1.1` 之后提交）。
  GitHub 上可直接看；本地这份与仓库版内容一致，仓库版为准。
- 覆盖范围：2026-10-23 至 2026-10-25 全部 VPN / Steam 网络相关工作，共 3 条实现线 + 1 段前史
- 素材来源：`steam-vpn/WORKLOG.md`、`steam-vpn/_crew/WORKLOG-上一版-0341.md`、`工作日志/svpn-Steam分流器收尾-工作日志.md`、`svpn-cpp/_crew/`（SPEC + D1/R2/R3/T1/V1 五份旧报告 + 本轮 R5/G1/P4/D2/D3/D4/R4/V2 八份新报告）、两仓 git 历史、本轮（1.1.1 发版）实测记录
- 本文是**总账**：把散在四个仓库、十七份报告里的结论合成一份可独立阅读的档案。细节以各子报告为准，本文给结论与证据。

---

## 0. 一页速览

| 实现线 | 位置 | 版本 | 代码量 | 测试 | 状态 |
|---|---|---|---|---|---|
| Python 原版 | `D:\LE\AI\dshwork\steam-vpn` | 1.1.0 | 14 文件 / 3727 行 | 364 用例全绿 + 冒烟 15 项 | 完成，已推 GitHub（`zhe-du/svpn`） |
| C++ 原生重写 | `D:\LE\AI\wbwrk\svpn-cpp` | **1.1.1** | 15 文件 / 7784 行 | 178 + 27 + **126** 全绿 | **已发布**（tag `v1.1.1` + GitHub Release，13 commit） |
| 前史：DNS 工具 | `D:\LE\AI\wbwrk\steam-dns` | — | 5 文件，批处理 | 无 | 一次性小工具，已定型 |

一句话：**给 Steam 做本地分流和下载加速的工具，从 Python 写到 C++ 单 exe，再到带图形界面，三轮走完；1.1.1 已发版，剩下的瓶颈在外部通路和几条没有自动化用例的分支。**

---

## 1. 这东西到底是什么

在本机起一个**只服务 Steam 的代理**（SOCKS5 `127.0.0.1:7899` + HTTP `:7900`），按域名把 Steam 流量交给用户自己的上游节点；再带一个 CDN 选边器（DoH 解析 + TLS 握手竞速 + 1MB 实测），把下载/更新钉到当前最快的 Akamai 边缘。

**五条硬事实**（本机实测，是整个项目的前提）：

> ⚠️ **2026-10-25 13:00 复测修订**：下表第 2、3 条已经过时。当时实测 `probe --timeout 4` 得
> **直连可用 7 / 需节点 2**：被 SNI 阻断的是 `steamcommunity.com` 与 **`api.steampowered.com`**
> （原表漏记），而 **`avatars.steamstatic.com` 直连可用（406ms）**（原表记为阻断）。
> 网络环境会变，这类事实必须带测量日期，不能当常量用。

| 事实 | 结论 |
|---|---|
| `store` / `login` / `api` / CDN 直连正常 | 60–160ms，商店、登录、下载都不需要节点 |
| `steamcommunity.com` SNI 阻断 | TCP 能连，ClientHello 带该域名立刻被 RST，**换 IP 无效** |
| `avatars.steamstatic.com` 同样阻断 | 头像/部分图片（**已过时，现直连可用 406ms**） |
| 社区、创意工坊 | **必须走节点** |
| 没有节点时的加速 | CDN 选边 + hosts 仍能把下载指到最快边缘（需一次管理员授权） |

**它不做什么**（写进 README 的边界，不是免责声明）：不是 VPN、没有 TUN 虚拟网卡、不内置任何节点、不代理 UDP、不做 TLS 中间人、不装服务、不加开机自启、不注入进程、不加载驱动。上游只认 `socks5://` 与 `http://`。

---

## 2. 时间线

### 2.1 前史（10-23 前后）

`steam-dns`：5 个文件的批处理套件（`steam-dns-fix.cmd` / `restore` / `flush-dns.cmd` / `dns-backup.txt` / README）。做的是改 DNS + 刷缓存这一件事，属"先试试改 DNS 有没有用"的阶段。结论推动了后面的判断：DNS 改了也没用，因为卡点是 SNI 而不是解析。

### 2.2 第一代 Python · 1.0.0（10-24 03:00–03:41）

- **接手状态**：半成品，启动即 `Errno 10048`（端口被占）。
- **规模**：1 Lead + 8 teammate + 13 subagent，共 22 路。
- **做了什么**：03:02–03:05 测网络基线；03:05–03:16 建 `netutil` / `discover` / `probe` / `cdn`，离线首版 107 用例；03:17–03:40 修 P0/P1 并并行写文档、做审计；03:41 收到停止指令，出"最终版"。
- **四个 P0**：
  1. `Upstream.enabled` 把 `127.0.0.1` 当占位符 → 节点链路永远不生效；
  2. PAC 生成成 `var PROXY={...}` 非法 JS → 浏览器静默回退直连，表现为"设了代理没用"；
  3. `up -b` 就绪判定写反 → 孤儿进程占着端口；
  4. `hosts` 写回用 UTF-8 毁掉 GBK 注释，且条目可注入 → 改字节无损往返 + `sanitize_entries`。

### 2.3 收尾轮 Python · 1.1.0（10-24 18:25–19:20，55 分钟）

- **目标**：把上一版 §7 列的代码级缺陷与交付面不自洽全部闭环。
- **规模**：11 个子 AI 两轮 —— 第一轮 8 路并行改代码（P1–P8，**文件所有权互斥**），第二轮 3 路做交付面（D1 文档 / D2 审计 / D3 e2e）。
- **产出**：2 份 SPEC + 11 份五段式报告（改了哪些文件 / 逐条修复 / 验证结果 / 契约变更 / 没做完）。
- **结果**：27 处修复闭环，364 用例全绿，静态审计 P0 归零。

### 2.4 C++ 原生重写 · 1.1.0-cpp（10-24 19:45 → 10-25 11:02）

- **动机**：Python 版要解释器、要 `.cmd` 垫片、`.ps1` 还要 BOM 才不乱码；目标机只想拷一个文件就能跑。
- **硬约束**（无一条被破）：不引入第三方库（只用 Win32 / Winsock2 / SChannel / WinHTTP / CRT / STL）；`src/svpn.hpp` 是共享契约不许改；C++20 且 `-Wall -Wextra` 零警告；源码与运行期字符串一律 UTF-8；线程 + 阻塞 socket + `select`，禁 IOCP/epoll；跨线程不抛异常；禁把中文 printf 到窄流。
- **模块划分**：A1 config/rules、A2 net、A3 tls、A4 proxy、A5 cdn、A6 winsys/pac、A7 sni，Lead 拿 main/json/util/build.cmd。
- **结果**：单 exe 1,678,848 B、md5 `e634fe15…`、`-static -municode`、零 DLL 依赖。
- **四道门**：
  - **R2**：Python 基准仓发布（`037a78f`，fix(netutil) DoH 必须绕过系统代理，3 文件 +92/−6，103 用例全过），并附 C++ 二进制门禁核对；
  - **V1**：`svpn.exe` 只读 CLI 审计，挖出 14 条缺陷（含 1 条 P0、4 条 P1），并对发布物签字；
  - **T1**：单测矩阵（config 6 组 + SNI 解析含守卫页 516 次截断、500 段 fuzz）；
  - **R3**：打包入库（tag `v1.1.0` → `9ff8fdb`，产物 + 哈希 + 校验指引）。
- **三条发布门禁**（必须单会话连跑，先确认端口空闲）：`up -b -p 17899 -m blacklist` 后再 `up -b` 须 rc=0 且提示"已在运行"、pid 与 `effective.json` 不变；`down` 后三个端口全返 10061。

### 2.5 图形界面轮 · 1.1.1（10-25 11:15 → 12:22，本轮）

- **做了什么**：给 C++ 版加 Win32 原生 GUI（`src/gui.cpp` 451 行，只用 user32/gdi32/comctl32），`build.cmd` 改成一趟编两个产物（`-mconsole` / `-mwindows`）。
- **关键设计**：所有按钮都调 `dispatch_cli()` —— 与命令行**同一条代码路径**，绝不给 GUI 另写一套逻辑，避免两个入口行为漂移。长任务放工作线程，输出用自定义 `streambuf` 逐行 `PostMessage` 回 UI 线程。
- **本轮修 3 个真缺陷**（详见 §4.4）：`stats.json` 编码、状态框高度、停止按钮静默死键。
- **版本**：1.1.0 → **1.1.1**。

### 2.6 发版轮 · 1.1.1 定稿并发布（10-25 12:35 → 13:20）

- **起点**：1.1.1 已经编好、测试全绿，但**没发布**——`dist/` 仍是 1.1.0 的单产物包，10 项改动未提交，tag 未打。
- **做了什么**：先盘清剩余工作（发布阻断 5 项 + 交付面欠账 7 项 + 代码级 3 项），
  按**文件所有权互斥**派 7 路子 AI 并行（P4 代码 / git 卫生 / 架构文档 / Windows 文档 / 英文 README / 流程文档 / 分支补测），
  Lead 负责重建、门禁、打包、提交、发版，并**自己复测裁决各路报上来的事实冲突**。
- **被中断一次**：第一轮派发 8 分钟后全部子 AI 被杀（我用了单条长命令静默轮询 9 分钟，界面看着像卡死）。
  重派时加了两条改进：**子 AI 先写占位报告再补全**（中断也能看到进度）、**轮询拆成 100 秒短段**。
- **发版前又挖出 5 条缺陷**（详见 §4.5），全部修掉后重建。
- **产出**：4 个 commit、tag `v1.1.1`、GitHub Release（附 zip 1,456,696 B）、3 份新文档（ARCHITECTURE 398 行 / WINDOWS 294 行 / README.en 272 行）、2 份流程手册（packaging 776 行 / log-analysis 622 行）、8 份五段式报告。
- **`gh` 可用**：已登录 `zhe-du` 且带 `repo` scope —— 此前"必须人工建仓库/Release"的结论**作废**，全流程可自动。

---

## 3. 代码与产物现状

### 3.1 两仓

| 项 | Python | C++ |
|---|---|---|
| 路径 | `D:\LE\AI\dshwork\steam-vpn` | `D:\LE\AI\wbwrk\svpn-cpp` |
| 远端 | `git@github.com:zhe-du/svpn.git` | `git@github.com:zhe-du/svpn-cpp.git` |
| 分支 / HEAD | `main` @ `0d21850`（3 commit） | `main` @ `dfc9d0f`（13 commit） |
| tag | 无 | `v1.1.0`、**`v1.1.1`**（均已推） |
| 工作区 | 干净，与 origin 同步 | **干净**，与 origin 同步 |
| GitHub Release | 无 | **`v1.1.1` 已发布**，附件 `svpn-1.1.1-win64.zip`（1,456,696 B） |
| 二进制是否入库 | 否 | 否（`dist/` 在 `.gitignore` 里，走 tag + Release 附件） |

### 3.2 产物指纹

| 版本 | 文件 | 字节 | md5 |
|---|---|---|---|
| 1.1.0-cpp（旧发布） | `dist/svpn.exe` | 1,678,848 | `e634fe15067808ccf2ba0dad30d55d97` |
| **1.1.1（当前发布）** | `svpn.exe` | 1,764,864 | `a0352b4fc75d2a4f25a0c6c429955d5c` |
| **1.1.1（当前发布）** | `svpn-gui.exe` | 1,763,328 | `897c489e23048bd843d1981b97b98f0a` |

已发布包：`svpn-1.1.1-win64.zip`，1,456,696 B，**9 个文件**（双产物），`SHA256SUMS.txt` 覆盖 8 项（不含自身）。
zip 内是单层目录 `svpn-1.1.1-win64/`，与 1.1.0 一致。两个 exe 均已设只读位。

### 3.3 测试矩阵

| 套件 | 1.0.0 | 1.1.0 (Py) | 1.1.0-cpp | 1.1.1 |
|---|---|---|---|---|
| Python 离线 | 169 | **364**（+195） | — | — |
| 冒烟 | 15（原记 17，数错） | 15 ALL PASS | — | — |
| `test_config.cpp` | — | — | 178 | 178 |
| `test_sni_parse.cpp` | — | — | 27 | 27 |
| `test_cli.py` | — | — | 113 / 0 / 0 | **126 / 0 / 0**（dev 与发布物各跑一遍） |

---

## 4. 修过的问题总账

按性质分为五类，跨三轮去重。编号沿用各轮日志。

### 4.1 会崩、会伤到用户的（20 处）

| 问题 | 修法 |
|---|---|
| `_bridge` 用裸 `gather`，一侧异常即外穿、另一侧 pending 不回收 → 日志刷 `Task was destroyed` | `return_exceptions=True`，`finally` 取消并回收另一侧，两侧 writer 必关 |
| `pipe` 异常白名单过窄 | 放宽到 `OSError`/`TimeoutError`/`IncompleteReadError`/`RuntimeError`，`CancelledError` 继续上抛 |
| `"listen_port": null` → `int(None)` 崩进程 | `_as_port`/`_as_timeout` 归一，`None`/`""`/`"abc"`/越界一律回落默认 |
| `up --port 7900` 撞 `http_port` → 裸 `OSError 10048` | 双层预检：cli 给人话，proxy 抛 `ValueError` 兜底 |
| `CONNECT host:abc` → `int()` 异常穿出 handler | 回 `400` 并关连接 |
| `probe` 在全新安装时跑完才 `FileNotFoundError`（结果全丢） | 写 `probe.json` 前 `RUNTIME.mkdir` |
| `pid_alive`/`_port_owner` 用 `text=True`，`PYTHONUTF8=1` 下解码失败被吞 → **误判"未运行"** | 收 bytes 按 `mbcs + replace` 解 |
| SNI 转发空白名单时 `return True` → 本机任意进程可当通用 443 出口 | 空表 = 全部拒绝 + 启动警告一次 |
| SNI 成功路径永不关上游 socket → 每连接漏一个 fd | `sw` 提到 `try` 外，`finally` 双关 |
| `node scan` 把 svpn 自己写成上游节点 → 流量绕回自身打满 fd | `discover.is_self()` + 全源过滤 + `node set` 拒绝 |
| `down` 没有 `wininet-backup.json` 仍打印"系统代理已还原" | 按返回值分流，没备份就明说"没动过"并回显当前值 |
| PAC suffix 用 `f'"{s}"'` 拼 → 带引号/换行即非法 JS | 改 `json.dumps` 生成数组元素 |
| `mode=blacklist` 的 PAC 里 `DIRECT=[]` → "国内直连"是空话 | 空表回落内置 `CN_DIRECT_SUFFIXES` |
| `dial_socks5` 用 `isdigit()` 判 IP → **IPv6 被当域名发 ATYP=3** | `ipaddress.ip_address()` 三分支（v4/v6/域名），本地假上游实测 5 种输入 |
| `up -b` 先写 PID 再等就绪 → 并发启动时第二个实例删掉第一个的 pid 记录 | 就绪后才写；失败只删自己那条；`down` 找不到 pid 但端口被占时打印占用者 |
| `_race_connect` 同批多 IP 同时连上时非 winner 不关 → 每次漏 1–3 fd | 记录 winner，其余 `_close_result()`，`finally` 兜底 |
| `ProxyServer.close()` 不清 `_servers` → 同一实例再 `start()` 静默失效 | 交换清空 |
| `hosts apply` 写死 `max_age=10**9` → **16.3 小时前的边缘 IP 被写进 hosts** | 按 `refresh_hours` 校验 + 区分"没缓存/已过期"自动重选，补 8 条 TTL 边界测试 |
| stats 只在 `finally` 落盘 → `taskkill` 必丢 | 改 5 秒周期落盘（C++ 侧退出时写） |
| `[C++]` **子命令一律取错参数位**（V1 ①，P0）：`pos[1]` 当子命令 → `node set/clear` 失效、`system *` 全不可用、**`hosts clear` 反向执行（把条目写进 hosts）** | 改取 `pos[2]` |
| `[C++]` `probe --json` 的 stdout 不是纯 JSON（表格混在里面，V1 ②） | 人类可读输出改走 `cerr` |
| `[C++]` `doctor` 面对损坏的 `config.json` 报"没发现问题"、rc=0（V1 ③） | `load_config(&err)` 计入 issues |
| `[C++]` 失败的第二个实例抢删正在运行实例的 pid（V1 ⑫，唯一真 P1）→ `down` 失效、进程孤儿占端口 | 改为检测两个端口 + 只删自身 pid（`b5c97728`） |
| `[C++]` `up -b -p/-m/--node` 生效了但 `status` 不同步（V1 ⑭/⑮） | 写 `effective.json`，`status` 读它 |

### 4.2 "假成功"类（7 处）

写了代码、写了注释、写了文档，但**没有任何东西在跑它** —— 不报错，只是安静地什么都不做。这类比崩溃更难发现。

| 问题 | 修法 |
|---|---|
| `cdn.enabled` 是假开关（`forward.py` 无人 import） | `_serve_async` 按开关真起 `SniForwarder`；`hosts apply` 在 SNI 模式写 `127.0.0.1` 并提示需常驻 |
| `probe` 退出码恒 0（社区不可用也 exit 0），CI 无法当判据 | 新增 `unusable`，`ok = not unusable`，退出码 0/1，`--lenient` 保留旧语义 |
| `EXTRA_KEYS` 归一分支不可达 → `proxy_suffixes`/`direct_suffixes` 不可配置 | 白名单放行 + 归一 |
| `svpn.cmd` 的 `python -c "import sys"` 缺 `call` → 静默无输出 + exit 0 | 探测与真实调用全加 `call`（用 `python.bat` 垫片实测复现） |
| `install.ps1` 引用两个不存在的脚本 | 真造 `install`/`uninstall`/`start` 三件套，开工清单 3 → 5 项 |
| `--bench` 从来不是参数，文档却写它 | 文档按实测改（`--no-bench`），版本升 1.1.0 |
| `[C++]` `pac --write` 是空开关（V1 ⑨） | 文档改成描述真实行为，不留"未来会支持" |

### 4.3 交付面（脚本 / 配置 / 文档 / 打包）

| 问题 | 修法 |
|---|---|
| `install.ps1` 缺 UTF-8 BOM → PS 5.1 按 ANSI 读源码，中文乱码甚至 `ParserError` | 重存 `utf-8-sig` + CRLF，实测漏 BOM 的副本直接解析失败 |
| `.cmd` 里中文 `rem` 注释在 CP936 控制台触发行偏移 → `'?' is not recognized` | 注释全 ASCII，含中文的 `echo` 后强制留空行（规则写进 `docs/WINDOWS.md`） |
| `config.example.json` 与 `DEFAULT_CONFIG` 漂移，`config.json` 缺 3 个键，`_note` 被并进运行配置 | 12 键契约 + `merge_config()` 归一 + 15 条契约测试焊死 |
| 文档项数、版本号、参数与事实不符 | 全部按实测改：15 项、1.1.0、`--no-bench` |
| `[C++]` 目录改名后 README 仍写旧名；`RELEASE.txt` 两处失实（文件数 5→6） | 去掉过期提示；文件数改对，补校验指引 |
| `[C++]` `BUILD.md` 说"单个 svpn.exe"、README 说"子命令 13 个" | 本轮改成双产物、14 个命令（含 `gui`） |

### 4.4 本轮（1.1.1）新发现并修掉的 3 处

| # | 问题 | 定位 | 修法 |
|---|---|---|---|
| 1 | **`runtime/stats.json` 被写坏**：`_wfopen_s(..., L"w, ccs=UTF-8")` 配 `fwrite` 写窄字节串，每两个字节被当 UTF-16 码元重编码。结果落盘是乱码（实测 339 B），Python `json.loads` 报 `Unexpected UTF-8 BOM`，C++ 自己读不回来，`status` 的「上次统计」行**永久不出现** | `src/util.cpp:202` | 改 `L"wb"`。最小复现：同写法写 23 B JSON，落盘只剩 3 B（一个 BOM）；改二进制后 23 B 完整 |
| 2 | **状态框放不下**：`status` 输出 9 行，框高 `S(96)` 只有 6 行，`CDN缓存` 与 `hosts` 被挤到滚动区外 —— 这两行正是判断"下载加速有没有生效"的唯一依据 | `src/gui.cpp` layout | 高度 96 → 168（按 10 行留） |
| 3 | **「停止」在任务执行期是静默死键**：`run_async` 用 `g_busy` CAS，忙时直接 `return`，而停止按钮走的也是它 → 点下去日志打一行"正在停止…"，然后什么都不发生 | `src/gui.cpp` WM_COMMAND | 忙碌时先拦下并明确提示，不再假装提交 |

### 4.5 发版轮（1.1.1 定稿）新修 5 处

前一轮把 1.1.1 编出来了但没发。这一轮在发版前又挖出并修掉 5 条，其中 4 条属「假成功」。

| # | 问题 | 定位 | 修法 |
|---|---|---|---|
| 1 | **P4**：`up -b --node http://…` 时 `effective.json` 只写 `upstream` 不写 `upstream_kind`，`status` 把 HTTP 上游**谎报成 `socks5`** | `src/main.cpp` 写侧 / 读侧 | 写 `upstream_kind` 并读回；旧格式文件（无该键）保持原样不报错。新增用例组 ⑯ |
| 2 | **前台模式是另一套行为**：`up` 不带 `-b` 只调 `serve_all` 就返回 —— 没 SNI 转发线程、没统计周期落盘、不做 `apply_ip_map`、不写 pid/effective.json，`--system-proxy` 完全没生效。最严重后果：配了 `cdn.enabled` + `hosts apply` 后，客户端连 `127.0.0.1:443`，而前台模式里**没有东西在听那个端口，下载静默全断** | `src/main.cpp` 原前台分支 | 前台改为复用 `cmd_serve(p, a, &cfg)` 的同一条启动路径（新增 `preset` 参数）。前台与后台只应差「日志去哪、进程是否脱离控制台」 |
| 3 | **端口被别的程序占用时谎报「已在运行」并 `return 0`** —— 调用方以为代理起来了，连上去全失败 | `src/main.cpp` 端口预检 | 先看 pidfile：有活 pid 才报「已在运行」，否则明说被其他程序占用并返回 1（**rc 语义变更**，用例 `[N]` 焊死） |
| 4 | **`--timeout 0.5` 变永不超时**：`std::stoi("0.5")=0`，`0×1000` 传进 `SetSocketTimeout` 等于 `INFINITE`。实测一次 `probe` 在不可达端点跑了 **64 秒** | `src/main.cpp` `Args::num` | 新增 `Args::secs()`：按 double 解析、下限 1 秒、上限 3600 |
| 5 | 生成的 PAC 文件头提示「重跑 `svpn pac --write`」，而 `--write` **不是参数**（会被静默忽略） | `src/pac.cpp` | 改为 `svpn pac` |

顺带：`print_usage()` 漏掉 `gui`；`tests/test_sni_parse.cpp` 注释行尾反斜杠触发 `-Wcomment`；两套测试产物改按 `-Wall -Wextra` 编译（此前没加，正是总日志 §6.4.8 记的欠账）。

**文档层面的三处失实也一并改了**：
- README 子命令表在第 83 行被一行插叙**截断**，`pac` 与 `steam` 两行掉出表外，表格渲染成半张
- README 顶部写死 1.1.1 的 md5 与字节数（任一次重编都会变，注定过期）→ 改为指向 `SHA256SUMS.txt`
- 「单个 `svpn.exe` / **零 DLL 依赖**」不准确：1.1.1 是双产物，且产物仍导入 **11 个系统 DLL + 12 个 UCRT `api-ms-win-crt-*`（实测 23 项）**。改为「不依赖任何第三方运行库 DLL」
- `docs/BUILD.md` §5 还停在 1.1.0 的单产物流程；`RELEASE.txt` 源码指纹改按 **git 索引 blob（LF）** 计算，不受 `core.autocrlf` 影响

---

## 5. 验证证据

### 5.1 Python 1.1.0

- 离线 **364 用例全绿**：基线 50/57/62 + proxy_hardening 25、forward 16、netutil_extra 42、cli_contract 50、config_contract 15、integration_local 8、pac 12、hosts_cache 8、fuzz 19（1 skip）
- 冒烟 15 项 ALL PASS；`compileall` exit 0；13 个子命令 `--help` 全部 rc=0 且 stderr 0 字节
- 3 个 `.ps1` 头三字节 `ef bb bf`、`Parser::ParseFile` 0 错；2 个 `.cmd` 无 BOM、纯 CRLF
- 静态审计：P0 2 → **0**；未定义名 0、签名不匹配 0、argparse 13/13、编码 39/39、`-X dev` 17 次 stderr 全 0、裸 `except` 0
- PAC 三模式用 node 真解析：`SyntaxOK` ×4 + 分流语义正确（含大写、尾点、lookalike 反例）

### 5.2 端到端（会联网，结论：未达标但已归因）

| 轮次 | SOCKS5 | HTTP | P95 |
|---|---|---|---|
| 第一轮 | 0/10 | 1/10 | 7579ms |
| 第二轮 | 3/10 | 6/10 | 935 / 960ms |
| **直连对照组** | **5/10** | — | 529–1041ms |

按事先定的标准判**未达标**。但对照组同样 5/10、失败签名完全一致（`ConnectionResetError` 56–59ms、`SSL: UNEXPECTED_EOF` 61–81ms，都是握手刚发出就被掐断）→ 瓶颈在本机到 Steam 的通路，不在 svpn 代码。隧道本体正常：经 SOCKS5 取 CDN 两次 200 OK（444 / 238ms）。

`probe --node none` → rc=1，`unusable=[steamcommunity.com, avatars.steamstatic.com]`、`dead=[]`、store 直连可用 138ms —— 与"五条硬事实"吻合。

**归因修订**：上一版把 6/10 归因于"13 个 agent 抢网"，本轮加对照组后证伪 —— 安静环境同样 6/10。

### 5.3 C++ 1.1.0-cpp

`test_config` 178/178、`test_sni_parse` 27/27、`test_cli` 113/113，发布物门禁：`dist/svpn.exe`（同 md5）→ 113/113。

### 5.4 C++ 1.1.1（发版轮实测，2026-10-25 13:00 前后）

- 源码干净重建，`-Wall -Wextra` **零警告**（两个产物 + 两个测试产物），双产物都产出：
  `svpn.exe` 1,764,864 B、`svpn-gui.exe` 1,763,328 B
- `test_config` **178/0**、`test_sni_parse` **27/0**、`test_cli` **126 PASS / 0 FAIL / 0 SKIP**
- **对发布物再跑一遍门禁**：`test_cli.py --exe dist/svpn.exe` → **126/0/0**
- 三条发布门禁单会话连跑：`up -b -p 17899 -m blacklist` → 再 `up -b -p 17899` 报「已在运行（pid=22016）」、
  pid 与 `effective.json` 都没变 → `down` 后 17899/7900/443 全部拒连
- **前台模式修复实测**：`up -p 19101`（不带 `-b`）现在会写 `runtime/svpn.pid`、写 `effective.json`、
  `stats.json` 存在、`status` 能看见它（`进程 运行中 pid=33988`）、`down` 能优雅停掉、pidfile 清理干净。
  修复前这五项里只有「监听」成立
- **代理管道实测**（纯 HTTP 目标，避开 Steam 通路的干扰）：
  `http://www.baidu.com/` → 200（0.59s，HTTP 代理）/ 200（1.36s，SOCKS5）；`http://www.qq.com/` → 302
- `stats.json` 是合法 JSON，「上次统计 3 连接 / 下行 1.41MB / 错误 0」正常显示 →
  1.1.1 修的 `_wfopen_s(ccs=UTF-8)` + `fwrite` 编码 bug 未复发
- 发布包：`svpn-1.1.1-win64.zip` 1,456,696 B / 9 文件，`sha256sum -c` **8 项全 OK**，
  双 exe 均设只读位，zip 内单层目录 `svpn-1.1.1-win64/`，`testzip()` 无损坏

**端到端结论修订（重要）**：上一版此节写「`curl` 经 HTTP 代理与 SOCKS5 访问 `store.steampowered.com`
均 200」。本轮复测**不成立**：经代理 20s / 15s 超时（0 字节），而**直连同一域名同样 12s 超时**
（TLS 握手成功、HTTP 层无响应）。用纯 HTTP 目标验证代理管道是通的（见上）。
**结论：瓶颈在本机到 Steam 的通路，不在 svpn**，与 §5.2 的对照实验一致。

---

## 6. 仍然存在的问题

### 6.1 要人工做一次的

1. `hosts apply` 需要一次 UAC（或在管理员 PowerShell 里跑 `--no-elevate`）
2. 社区 / 创意工坊 / 头像需要上游节点（本机 14 个常见客户端端口全空）；有节点后 `node set socks5://host:port` 或 `node scan`

### 6.2 已知行为取舍（不是漏修）

1. 半开连接会等到系统 TCP 超时：中继语义是"两个方向都 EOF 才收工"，彻底解决要加 idle timeout
2. `probe` 的「直连可用」只代表"竞速里至少一个 IP 成功"，**不能当成功率判据**，CI 里只能当"有没有路"的冒烟
3. `cdn.sni_port` 实际必须是 443 —— hosts 只能表达 IP，客户端连的就是 `127.0.0.1:443`
4. C++ 版 `auto_node` 键只解析不回写；`node scan` 只扫固定 16 个本机端口；上游只认 `socks5://` 与 `http://`
5. C++ 版只有两个 CDN 域名做真实吞吐实测，其余只做 HEAD（显示 `-` 是设计如此）

### 6.3 技术债

1. **唯一 P1**：`netutil._DNS_CACHE` 无上限、无负缓存、无总 deadline —— DoH 全被墙时线程池会被占满，代理假死数十秒。属设计改动，C++ 版照搬了同一语义
2. P1 部分闭合 2 项：`system.py` 的 hosts 写入**仍非原子**（C++ 版用 `MoveFileEx` 做了原子替换）；SOCKS5 握手 15 处读只有 2 处带超时、且全包无并发闸门
3. P2 共 13 项：`ips[:4]` 取样、DoH 查询串未编码、`100.64/10` 仍送上游、日志无轮转、`save_upstream` 解析失败清空配置等（带复现命令，见 `runtime/audit-after.md` §7）
4. `probe` 的 `cert_cn` 恒空（仅影响诊断；C++ 版已取到，CLI 未输出）
5. C++ 版 `--node http://…` 在 `status` 仍显示 socks5（P4，未修）；V1 ⑩ 的"`down` 只按 pid 存在性杀进程"被 Lead 裁定接受

### 6.4 流程与发布面的欠账（**1.1.1 发版轮已清 6 条，见 §4.5**）

1. ~~**C++ 1.1.1 还没发布**~~ → **已发布**：tag `v1.1.1` + GitHub Release，`dist/` 重建为双产物
2. ~~`test_cli.py` 从 113 掉到 111 + 1 skip~~ → **误报**。那是环境噪声（7899/7900 被占 + 读真实 hosts 无权限）。
   干净环境下 113/0/0；本轮加用例后 **126/0/0**，dev 二进制与发布物各跑一遍都是 126
3. ~~未做的文档：`runtime/packaging.md`、`runtime/log-analysis.md`~~ → 已补，落在 `_crew/reports/packaging.md`（776 行）、`log-analysis.md`（622 行）
4. ~~C++ 仓缺 `.gitattributes`~~ → 已加（源码恒 LF、`*.cmd` 恒 CRLF、二进制不转换）。`dist/` 与 `.gitignore` 注释矛盾已改。
   **仍欠**：1.6MB 的 dist blob 还在 `0c1c602`/`9e6bfb4` 历史里（本轮明确不改写历史）
5. ~~`run.cmd` 从未实跑；C++ 版没有英文 README，也没有 ARCHITECTURE/WINDOWS 文档~~
   → `docs/ARCHITECTURE.md`（398 行）、`docs/WINDOWS.md`（294 行）、`README.en.md`（272 行）已补。
   `run.cmd` 的发布版（250 B）与开发版（823 B）是两个不同文件，已分别收进 `tools/run-release.cmd` 与仓库根
6. ~~`upstream_kind` 没进 `effective.json`~~ → 已修（P4）
7. **机器上残留过 PID 9008 占着 7899/7900** —— 门禁前必须先确认端口空闲，这条仍然有效
8. **V1 审计未覆盖**：`up` 前台模式、`--system-proxy`、`system on/off`、`hosts apply`、`pac --apply`、
   `steam`、`cdn --refresh`、`doctor` 管理员分支；T1 未与 Python 做自动差分
   → V2 轮补测了非侵入的 14 组（见 `_crew/reports/V2-branch-coverage.md`），
   **改全局状态的那几条仍只有代码审计、没有可执行用例**

### 6.5 本轮（1.1.1 发版）新发现、且**未修**的

全部记录在 `_crew/reports/R5-release-1.1.1.md` §5 与 `V2-branch-coverage.md`：

1. `doctor` 不提示 hosts 属主可写（只在真正 apply 时才发现）
2. `status` 在「端口被占但进程已死」时两行自相矛盾（「进程 未运行」+「端口 监听中」）
3. `pac --apply` 与 `doctor` 都会改 WinINet，违反「只读命令无副作用」
4. `node set` 的值本身非法时，用「Steam 不可达」当拒绝理由，会误导排障
5. `svpn-gui.exe -v` 被当成「无参数启动」直接开窗（打包脚本必须绕开）
6. `test_cli.py` 里两组用例共用同一临时工作区，前一组残留会让后一组「碰巧通过」
7. `runtime/svpn.log` 被常驻进程独占，外部 `tail` 报 `Device or resource busy`

---

## 7. 方法论沉淀（六条，都是踩出来的）

1. **最贵的 bug 不是崩溃，是"假成功"**。代码写了、注释写了、文档也写了，但没有任何东西在跑它 —— `cdn.enabled`、`EXTRA_KEYS`、`--bench`、引用不存在的脚本、`stats.json` 写坏了却没人读。零报错，只安静地什么都不做。
2. **交叉读胜过自审**。Python 轮 4 个真 bug（PAC 转义、`close()` 不清 `_servers`、`node scan` 自环、自己改出的 `KeyError`）全部由另一个 AI 读同一份代码时发现，无一来自自查。
3. **归因必须带对照组**。"6/10 是并发抢网"听起来合理，加了直连对照才发现安静环境同样 6/10。没有对照组的归因基本是猜。
4. **每条断言都要有"能红"的路径**。契约测试焊死 12 个键、`-X dev` 扫告警、`Parser::ParseFile`、字节级 BOM 比对、故意回退求红 —— 否则测试只是"看起来对"。
5. **审计要覆盖运行期数据的新鲜度**。`max_age=10**9` 类型合法、无告警、覆盖率满分，静态审计/模糊/集成全抓不到；把代码行为与磁盘数据（cache `updated` 16.3h 前 vs TTL 6h）对齐才看得出来。
6. **并行安全靠文件所有权互斥 + 跨文件问题只写报告**。8 路同改不互踩，Lead 统一裁决；过程留 2 份 SPEC + 11 份五段式报告，可追溯。

**本轮新增教训（7）**：改 UTF-8 源码**不要**用 PowerShell 的 `Get-Content | Set-Content` 批量替换 —— PS 5.1 按 ANSI 读、按 UTF-8 写，中文会被双重编码毁掉，且非 git 跟踪文件无法恢复。要用 `edit` 类工具或 Python 显式指定编码。本轮为此重写了 `gui.cpp`。

**发版轮新增教训（8–12）**：

8. **文档里的"事实"必须带测量日期**。§1 那五条硬事实写的时候是真的，一天后两条就不成立了（`avatars` 从阻断变直连可用、`api` 变成阻断）。把易变的观测写成常量，等于给后来的人埋雷。
9. **别用长命令静默轮询**。派完子 AI 后用一条 9 分钟不输出的命令等结果，界面看着就是卡死，用户会（正确地）中断，然后所有子 AI 一起死。改成**短段轮询 + 每段都出字**，并且让子 AI **先写占位报告再补全** —— 中断至少留下进度。
10. **"零依赖"这类绝对化表述最容易翻车**。README 写"零 DLL 依赖"，实测 `objdump -p` 出来 23 项导入（11 系统 DLL + 12 UCRT）。准确说法是"不依赖第三方运行库 DLL"。凡是写"零/无/永不"，先去实测一次。
11. **打包脚本本身要防"会挂住"的命令**。`svpn-gui.exe -v` 是 `-mwindows` 子系统，没有控制台，传 `-v` 会被当成无参数启动**直接开窗并阻塞** —— 打包脚本里放这么一句就永远发不出去。
12. **同一份代码里"另写一段精简版"几乎一定丢东西**。前台模式为了少写几行，绕过 `_serve` 自己拉起 `serve_all`，结果丢了 SNI 转发、统计落盘、hosts 映射、pid、系统代理五样。**入口可以有两个，代码路径只能有一条** —— 这条和 GUI 的"所有按钮都调 `dispatch_cli()`"是同一个道理。

---

## 8. 接手步骤

### 8.1 Python 版

```powershell
cd D:\LE\AI\dshwork\steam-vpn
python -m svpn doctor
python -m svpn cdn --refresh
python -m svpn hosts apply          # 弹一次 UAC
python -m svpn node scan            # 有 Clash/v2rayN/sing-box 在跑的话
python -m svpn up -b --system-proxy
# 收工
python -m svpn down --clear-hosts
```

### 8.2 C++ 版

```powershell
cd D:\LE\AI\wbwrk\svpn-cpp
build.cmd                            # 一趟出 svpn.exe + svpn-gui.exe
.\svpn.exe init
.\svpn.exe up -b
.\svpn.exe status
.\svpn-gui.exe                       # 或 .\svpn.exe gui
.\svpn.exe down --clear-hosts
```

### 8.3 把 1.1.1 发出去（**已完成，2026-10-25 13:20**）

实际执行的顺序（与计划的差异已注明）：

1. ✅ `.\svpn.exe -v` 复核 `1.1.1`；`test_cli.py` 基线**不是 113 掉了，而是环境噪声** ——
   干净环境下 113/0/0，本轮加用例后 126
2. ✅ 修掉发版前挖出的 5 条缺陷（见 §4.5），重建，`-Wall -Wextra` 零警告
3. ✅ 重打 `dist/`：双 exe + README.md + README.en.md + LICENSE + run.cmd + config.example.json
   + RELEASE.txt + SHA256SUMS（8 项）+ zip（9 文件、单层目录、双 exe 只读）
4. ✅ 提交 **4 个 commit**（计划是 3 个，多出一个把发版报告与 pack.py 收尾）：
   `f8b53c4` 行尾契约 / `a154b93` 源码 / `fc908b4` 文档 / `dfc9d0f` 发版报告
5. ✅ `tag v1.1.1` → `git push origin main` + `git push origin v1.1.1`
6. ✅ `gh release create v1.1.1` → https://github.com/zhe-du/svpn-cpp/releases/tag/v1.1.1
   （`gh` 已登录 `zhe-du`，带 `repo` scope，**全程无需人工介入**）
7. ⬜ **仍未做**：Python 仓那条 DNS 缓存 P1（`_DNS_CACHE` 上限 + 负缓存 + 总 deadline），属设计改动，需独立一轮

发版的完整证据链与遗留清单：`svpn-cpp/_crew/reports/R5-release-1.1.1.md`。
打包流程已固化成可照做的操作手册：`svpn-cpp/_crew/reports/packaging.md`。

### 8.4 排障顺序

`status` → `probe`（看退出码 + `runtime/probe.json`）→ `runtime/svpn.log` → `runtime/stats.json`（连接数、上下行、按域名 top）

---

## 9. 档案索引

| 想知道什么 | 看哪里 |
|---|---|
| Python 收尾轮全过程 | `steam-vpn/WORKLOG.md` |
| 上一版（1.0.0）全过程 | `steam-vpn/_crew/WORKLOG-上一版-0341.md` |
| Python 架构与 Windows 细节 | `steam-vpn/docs/ARCHITECTURE.md`、`docs/WINDOWS.md` |
| Python 静态审计全文 | `steam-vpn/runtime/audit-after.md` |
| Python 端到端实测 | `steam-vpn/runtime/e2e-after.md` |
| C++ 模块规范 | `svpn-cpp/_crew/SPEC.md` |
| C++ CLI 审计 14 条 | `svpn-cpp/_crew/reports/V1-cli-audit.md` |
| C++ 单测矩阵 | `svpn-cpp/_crew/reports/T1-tests.md` |
| C++ 发版 / 打包门禁 | `svpn-cpp/_crew/reports/R2-release.md`（Python 仓发布 + 二进制门禁核对）、`R3-package.md`（C++ 1.1.0 打包入库） |
| C++ 文档对齐 | `svpn-cpp/_crew/reports/D1-docs.md` |
| **C++ 1.1.1 发版全过程与遗留清单** | **`svpn-cpp/_crew/reports/R5-release-1.1.1.md`** |
| **打包怎么打（可照做）** | **`svpn-cpp/_crew/reports/packaging.md`**（776 行）、脚本 `svpn-cpp/tools/pack.py` |
| **日志/运行期数据怎么读** | **`svpn-cpp/_crew/reports/log-analysis.md`**（622 行） |
| **C++ 架构** | **`svpn-cpp/docs/ARCHITECTURE.md`**（398 行，211 处行内引用） |
| **C++ Windows 踩坑** | **`svpn-cpp/docs/WINDOWS.md`**（32 张四步卡片 + 8 条「不能照搬 Python 版」） |
| **P4 修复细节** | **`svpn-cpp/_crew/reports/P4-upstream-kind.md`** |
| **行尾契约 / gitignore 为什么这么写** | **`svpn-cpp/_crew/reports/G1-git-hygiene.md`** |
| **V1 未覆盖分支的补测（含 3 条 P1）** | **`svpn-cpp/_crew/reports/V2-branch-coverage.md`** |
| 本轮并行分工与文件所有权表 | `svpn-cpp/_crew/SPEC-1.1.1-release.md` |
| 面向人阅读的合并版（Python 轮） | `工作日志/svpn-Steam分流器收尾-工作日志.md` |

---

*本文由三轮日志 + 十三份 C++ 报告 + 两仓 git 历史 + 本轮实测合并而成。数字凡标"实测"均为本机当场跑出来的结果；凡标"报告值"以对应子报告为准。*

*发版物：<https://github.com/zhe-du/svpn-cpp/releases/tag/v1.1.1> · 源码 tag `v1.1.1` @ `dfc9d0f`*
