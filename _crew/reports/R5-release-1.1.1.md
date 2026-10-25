# R5 · svpn 1.1.1 发版报告

## 1. 摘要

C++ 版从「1.1.1 已编好但没发」推进到**已发布**：3 个提交 + tag `v1.1.1` + GitHub Release。
发版前修掉 4 个真缺陷（P4 kind 谎报、前台模式缺 SNI/统计/pid、端口被他占却报「已在运行」、
`--timeout` 小数被截成 0 变永不超时），补齐 3 份文档 + 2 份流程手册。
发布物 `svpn-1.1.1-win64.zip`（1,456,678 B，9 个文件，双产物），
`test_cli` 对发布物的门禁结果 **PASS 126 / FAIL 0 / SKIP 0**。

## 2. 做了什么

### 2.1 修掉的缺陷（4 条）

| # | 缺陷 | 位置 | 修法 |
|---|---|---|---|
| 1 | **P4**：`up -b --node http://…` 时 effective.json 只写 `upstream` 不写 `upstream_kind`，`status` 把 http 上游谎报成 socks5 | `src/main.cpp` 写侧 400、读侧 527 | 写 `upstream_kind` 并读回；旧格式文件（无该键）保持原样不报错 |
| 2 | **前台模式是另一套行为**：`up` 不带 `-b` 只调 `serve_all` 就返回，缺 SNI 转发线程、缺统计周期落盘、缺 `apply_ip_map`、不写 pid/effective.json、`--system-proxy` 完全没生效。后果：配 `cdn.enabled` + `hosts apply` 后前台跑等于 443 上没人在听，下载静默全断 | `src/main.cpp` 原 265–276 | 前台改为复用 `cmd_serve(p, a, &cfg)` 的同一条启动路径（新增 `preset` 参数） |
| 3 | **端口被别的程序占用时谎报「已在运行」**：只要端口被占就 `return 0`，调用方以为代理起来了 | `src/main.cpp` 原 275–281 | 先看 pidfile：有活 pid 才报「已在运行」；否则明说「被其他程序占用」并返回 1 |
| 4 | **`--timeout 0.5` 变永不超时**：`std::stoi("0.5")=0`，`0*1000` 传进 `SetSocketTimeout` 等于 `INFINITE`（实测 probe 在不可达端点跑 64 秒） | `src/main.cpp` `Args` | 新增 `Args::secs()`：按 double 解析、下限 1 秒、上限 3600；三处 `--timeout` 全部改用它 |

### 2.2 顺手修正

- `print_usage()` 漏掉 `gui` 子命令 → 补上
- `src/pac.cpp:93` 生成的 PAC 文件头提示「重跑 `svpn pac --write`」，而 `--write` 不是参数 → 改为 `svpn pac`
- `tests/test_sni_parse.cpp` 注释行尾的反斜杠把下一行续成注释（`-Wcomment`）→ 去掉；两套测试产物改按 `-Wall -Wextra` 编译
- `README.md` 子命令表在第 83 行被一行插叙截断，`pac`/`steam` 两行掉出表外 → 插叙移到表后
- `README.md` 写死 1.1.1 的 md5/字节数（重编必变）→ 改为指向 `SHA256SUMS.txt`
- `README.md`「单个 svpn.exe / 零 DLL 依赖」不准确 → 双产物 + 实测 23 项导入（11 系统 DLL + 12 UCRT），改为「不依赖第三方运行库 DLL」
- `docs/BUILD.md` §5 停在 1.1.0 单产物流程 → 改为双产物两趟编译
- `RELEASE.txt` 源码指纹改按 **git 索引 blob（LF）** 计算——工作区受 `autocrlf` 影响，同一 commit 在不同机器会算出不同 MD5

### 2.3 新增文件

| 文件 | 内容 |
|---|---|
| `src/gui.cpp` | Win32 原生 GUI，460 行，所有按钮调 `dispatch_cli()` |
| `docs/ARCHITECTURE.md` | 398 行，8 章，211 处行内引用逐条回读核对 |
| `docs/WINDOWS.md` | 294 行，A–G 七组 32 张四步卡片 |
| `README.en.md` | 272 行，声明六节完整 |
| `_crew/SPEC-1.1.1-release.md` | 本轮下发规范 + 文件所有权互斥表 |
| `_crew/reports/packaging.md` | 776 行打包流程手册 |
| `_crew/reports/log-analysis.md` | 622 行日志与运行期数据分析手册 |
| `tools/pack.py` | 打包脚本（索引指纹 / 双产物 / SHA256SUMS / zip） |
| `tools/run-release.cmd` | 发布版启动器（250 B，与仓库根 823 B 的开发版**不是同一个文件**） |
| `.gitattributes` | 源码恒 LF、`*.cmd` 恒 CRLF、二进制不转换 |

### 2.4 并行分工

7 路子 AI 并行，文件所有权互斥（详见 `SPEC-1.1.1-release.md` §4）：
W1 P4 代码 / W2 git 卫生 / W3 架构文档 / W4 Windows 文档 / W5 英文 README / W6 流程文档 / W7 分支补测。
Lead 负责重建、门禁、打包、提交、发版，并把各路报上来的**事实性冲突**自己复测裁决。

## 3. 验证

### 3.1 构建

```
[i] compiler: D:\LE\AI\tools\mingw64\bin\g++.exe
[1/2] svpn.exe  (console, CLI + "svpn gui")
[2/2] svpn-gui.exe  (windowed, no console flash)
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn.exe  1764864 bytes
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn-gui.exe  1763328 bytes
```

`-Wall -Wextra` 零警告（两个产物 + 两个测试产物）。

### 3.2 测试矩阵

| 套件 | 结果 |
|---|---|
| `tests/test_config.exe` | `PASS 178 / FAIL 0（共 178 项）` |
| `tests/test_sni_parse.exe` | `PASS 27 / FAIL 0（共 27 项）` |
| `tests/test_cli.py`（dev binary） | `PASS 126 / FAIL 0 / SKIP 0` |
| **`tests/test_cli.py --exe dist/svpn.exe`（发布物门禁）** | **`PASS 126 / FAIL 0 / SKIP 0`** |

`test_cli` 从 113 升到 126：P4 用例组 5 项、端口占用 6 项、`--timeout` 边界 2 项。

### 3.3 三条发布门禁（单会话连跑，`wbwrk_tmp/gate.sh`）

```
=== A：up -b -p 17899 -m blacklist ===
[+] 后台已启动 pid=22016  socks5://127.0.0.1:17899  http://127.0.0.1:7900
rc=0
=== B：再 up -b -p 17899 ===
[=] 已在运行（pid=22016）；改配置请先 svpn down
rc=0
  OK  pid 未变
  OK  effective.json 未变
=== C：down ===
[+] 已停止 pid=22016
  17899  拒连    7900  拒连    443  拒连
```

### 3.4 前台模式修复的实测（本轮新增验证）

```
=== up -p 19101 前台 ===
  前台进程 alive=yes
  pidfile=runtime/svpn.pid  内容=33988
  effective.json=有
  stats.json 存在=yes
  监听: 19101 监听中 / 7900 监听中
  status → 进程 运行中 pid=33988
  down  → [+] 已停止 pid=33988
  前台进程 alive=no   pidfile 残留=0   19101 拒连
```

修复前这五项里只有「监听」成立：没有 pidfile、没有 effective.json、`status` 看不见它、`down` 停不掉。

### 3.5 代理隧道实测

```
HTTP代理 http://www.baidu.com/ -> 200  0.591211s
HTTP代理 http://www.qq.com/    -> 302  0.097354s
SOCKS5  http://www.baidu.com/  -> 200  1.359284s
上次统计 3 连接 / 下行 1.41MB / 错误 0
```

`stats.json` 是合法 JSON、`status` 的「上次统计」行正常显示（1.1.1 修的 `_wfopen_s(ccs=UTF-8)` + `fwrite` 编码 bug 未复发）。

### 3.6 发布包

```
== dist/ ==
  LICENSE                 1,063 B  rw-
  README.en.md           24,733 B  rw-
  README.md              23,177 B  rw-
  RELEASE.txt             4,202 B  rw-
  SHA256SUMS.txt            621 B  rw-
  config.example.json     2,084 B  rw-
  run.cmd                   250 B  rw-
  svpn-1.1.1-win64.zip 1,456,678 B  rw-
  svpn-gui.exe        1,763,328 B  r--   ← 只读
  svpn.exe            1,764,864 B  r--   ← 只读

sha256sum -c SHA256SUMS.txt → 8 项全 OK
zip 内部为单层目录 svpn-1.1.1-win64/（与 1.1.0 一致），testzip() 无损坏
```

### 3.7 端到端（联网，结论同前：未达标但已归因）

`curl` 经 HTTP 代理与 SOCKS5 访问 `store.steampowered.com` 均**超时**（20s / 15s，0 字节）。
对照组：`curl` **直连**同一域名同样 12s 超时（TLS 握手成功、HTTP 层无响应）。
用纯 HTTP 目标验证代理管道 → 200/302/200 全通。
**结论：瓶颈在本机到 Steam 的通路，不在 svpn**，与总日志 §5.2 的对照实验一致。

**事实修订**：总日志 §1「五条硬事实」有两条已过时。本轮实测
`probe --timeout 4` 得 **直连可用 7 / 需节点 2**：阻断的是 `steamcommunity.com` 与
**`api.steampowered.com`**（日志未记），而 **`avatars.steamstatic.com` 直连可用（406ms）**（日志记为阻断）。
网络环境会变，这类事实应带测量日期。

## 4. 契约变更

| 变更 | 影响 |
|---|---|
| `cmd_serve` 新增可选第三参数 `const Config* preset = nullptr` | 仅 `main.cpp` 内部；后台自启路径行为不变 |
| `Args::secs()` 新增 | 仅新增，`num()` 保留 |
| `runtime/effective.json` 新增 `upstream_kind` 键 | 旧文件无该键仍正常（已加用例 ⑯） |
| `up` 撞到他人端口：`rc 0` → **`rc 1`** | 这是**有意改语义**。若有脚本依赖旧的 rc=0，需改。用例 `[N]` 焊死 |
| 发布包的 `run.cmd` 是 250 B 精简版，与仓库根 823 B 开发版不同 | 已存为 `tools/run-release.cmd`，打包时改名 |

## 5. 没做完 / 风险

1. **GitHub Release 的产物校验**只能人工点开确认一次（`gh release view v1.1.1` 已核对资产名与大小）。
2. **`system on/off`、`--system-proxy`、`hosts apply`、`pac --apply`、`elevate` 仍未实测**：
   会改本机全局 WinINet / 弹 UAC，无人值守环境无法安全验证。V2 报告给了代码路径审计（带行号），
   但这些分支**没有"能红"的用例**。
3. **V2 报的 P2 未修**（已记录，不影响本版发布）：
   - `doctor` 不提示 hosts 属主可写（只真正 apply 时才 68→21 字节）
   - `status` 在「端口被占但进程已死」时仍报「进程 未运行」，两行自相矛盾
   - `pac --apply` 与 `doctor` 都会改 WinINet，违反「只读命令无副作用」
   - `node set` 的值本身非法时，用「Steam 不可达」做拒绝理由，会误导排障
   - `svpn-gui.exe -v` 被当成「无参数启动」直接开窗（打包脚本必须绕开，已在 `tools/pack.py` 注明）
4. **历史里的 1.6 MB dist blob** 仍在 `0c1c602`/`9e6bfb4`。本轮**明确不改写历史**，
   靠 tag + Release 附件追溯。
5. **`test_cli.py` 里两组用例共用同一临时工作区**，前一组残留会让后一组"碰巧通过"
   （当前不影响结论，但属于测试卫生问题）。
6. **`runtime/svpn.log` 被常驻进程独占**，外部 `tail` 报 `Device or resource busy`。
   已记入 `log-analysis.md`；根治要给 `log_write` 换共享打开方式，属独立改动。
7. **V1 未覆盖分支里 V2 也未测的**：`up` 前台模式在 `cdn.enabled=true` 下的 SNI 转发实际生效性
   （本轮只验证了 pid/统计/down，没验证 443 上真的在转发）。
