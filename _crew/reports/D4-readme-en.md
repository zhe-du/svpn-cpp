# D4 英文 README

## 1. 摘要

新建 `README.en.md`（272 行，UTF-8 无 BOM + LF），结构与中文版逐节对齐，声明六节（用途 / 数据与隐私 / 安全 /
兼容性 / 免责 / 许可）完整翻译，一条未漏。所有数字已回源码逐项核对：版本 1.1.1、14 个子命令、9 个 probe 端点、
12 个顶层配置键 + 2 个可选键、10 个运行期文件、16 个扫描端口。发现中文版 3 处事实/结构问题与 SPEC 1 处笔误，
未改中文版与源码，列在 §4 交 Lead 裁决。

## 2. 做了什么

### 2.1 交付文件

| 路径 | 行数 | 说明 |
|---|---|---|
| `README.en.md`(新) | 272 | 英文版 README，本轮唯一产出物 |
| `_crew/reports/D4-readme-en.md`(新) | — | 本报告 |

### 2.2 结构（与 `README.md` 对齐，方便左右对照）

| 中文版 | 英文版 | 行号 |
|---|---|---|
| 标题 + 一句话 + 产物描述 | `# svpn (C++) — local split routing for Steam` | 1–12 |
| 它不做什么 | `## What it does not do` | 13–22 |
| 快速开始（4 条命令） | `## Quick start (4 commands)` | 23–46 |
| 子命令一览（14 个） | `## Subcommands (14)` | 47–82 |
| probe 的 9 个端点与退出码 | ``### `probe`: the 9 endpoints and its exit codes`` | 83–112 |
| 配置（config.json） | ``## Configuration (`config.json`)`` | 113–139 |
| 两个可选高级键 | `### Two optional advanced keys …` | 140–158 |
| CDN 加速的两种接法 | ``### Two ways to accelerate CDN (`cdn.enabled`)`` | 159–172 |
| 运行期文件 | `## Runtime files …` | 173–191 |
| 与 Python 版的关系 | `## Relationship to the Python version` | 192–200 |
| 已知差异与限制 | `## Known differences and limitations (the honest list)` | 201–215 |
| 编译 | `## Building` | 216–225 |
| 声明（6 小节） | `## Statements` | 226–270 |

首行 `> 中文版见 [README.md](README.md)`（第 3 行）；末行版本戳
`Version 1.1.1 · Windows x64 · single static exe, no DLL dependencies`（第 272 行）。

### 2.3 写法处理（不是逐字直译的地方）

1. **Quick start 压成 3 行可复制的 `bat` 块**，把"指代理"这句从伪命令行改成块内注释，避免读者复制一行不存在的命令。
2. **修好了中文版的表格断行**：`svpn-gui.exe` 那句说明在英文版移到 14 行表格**之后**（`README.en.md:68`），
   表格保持完整；中文版把它插在 `gui` 与 `pac` 两行之间，见 §4.1。
3. **不硬编码 md5**：中文版第 9 行写死了 `svpn.exe` 与 `svpn-gui.exe` 的 md5，但当前工作区二进制的 md5 已经对不上
   （见 §4.2），且 Lead 本轮还要重编，重编后必然再变。英文版改为"用 `certutil -hashfile svpn.exe SHA256` 对
   `SHA256SUMS.txt`"，同时保留**已核实的字节数** 1,761,792。
4. **术语固定**：SOCKS5 / HTTP CONNECT / SNI / DoH / CDN edge / hosts file / UAC / upstream node，全文一致。
5. **声明章节逐条落地**：用途 2 段；数据与隐私 5 条（联网清单含 4 个 DoH 服务器、不上传、写哪些本地文件、两个备份
   文件的敏感内容、config.json 明文口令）；安全 7 条（UAC、托管块标记原文、块外字节原样保留含 BOM、`hosts.tmp`
   原子替换、`HKCU` 无需管理员、无服务/自启/注入/驱动、回环默认、SNI 非开放代理、hosts 条目校验、探测不校验证书）；
   兼容性 5 条；免责 2 段；许可 MIT。

## 3. 验证

### 3.1 核对过的数字清单

| 项 | 英文版写的值 | 源码位置 | 核对命令 / 结论 |
|---|---|---|---|
| 版本号 | `1.1.1` | `src/main.cpp:22` `constexpr const char* kVersion = "1.1.1";` | `grep -rn "kVersion" src/` → 命中 main.cpp |
| 子命令数 | **14** | `src/main.cpp:1189`（`gui`）+ `src/main.cpp:1138–1162`（13 个 dispatch 分支） | `grep -n 'cmd == "' src/main.cpp` → `-v` 之外 13 个：init/up/_serve/down/status/probe/node/cdn/hosts/system/pac/steam/doctor；`gui`/`--gui` 在 `wmain` 里先于 dispatch 分流（`src/main.cpp:1189-1191`），故 13+1=14 |
| probe 端点数 | **9**（全部 443） | `src/main.cpp:30–40` `kEndpoints[]` | 逐条比对：store/login/api/steamcommunity/community.akamai.steamstatic/avatars.steamstatic/steamcdn-a.akamaihd/cdn.akamai.steamstatic/help，与英文版 `README.en.md:85` 的列表完全一致 |
| 顶层配置键 | **12** + 可选 **2** | `src/config.cpp:23–27` `kTopKeys[]` / `kExtraKeys[]` | `sed -n '22,28p' src/config.cpp` → 12 个顶层键、`proxy_suffixes`/`direct_suffixes` 2 个额外键 |
| `cdn.hosts` 默认域名数 | **8** | `src/rules_data.cpp:44–45` `kCdnDomains` | 8 条，与 `config.example.json:28–37` 一致 |
| 运行期文件 | **10** 条（`config.json` + `runtime/` 下 9 个） | `src/util.cpp:136–144` `Paths::resolve()` | `grep -rn "effective.json\|svpn.pid\|…" src/*.cpp src/*.hpp` → svpn.log / svpn.pid / effective.json / stats.json / probe.json / cdn-cache.json / svpn.pac / hosts.backup / wininet-backup.json，加 config.json = 10 |
| `node scan` 端口数 | **16** | `src/main.cpp:42–45` `kCommonProxyPorts[]` | 7890/7891/7897/1080/1081/10808/10809/10810/8889/8080/8118/9910/20171/20172/2080/2081 |
| `probe --timeout` 默认 | **6** 秒 | `src/main.cpp:636` `a.num("--timeout", 6)` | ✓ |
| `cdn --timeout` / `hosts --timeout` 默认 | **8** 秒 | `src/main.cpp:810`、`src/main.cpp:899` | ✓ |
| `connect_timeout` 默认/上限 | **12** 秒 / **>300** 回落 | `src/config.cpp:69–73,172` `as_timeout_secs()` | ✓ |
| 中继空闲上限 | **1 小时** | `src/proxy.cpp:34` `kRelayIdleMs = 3600000` | ✓ |
| SNI 读头上限 | **16KB / 10 秒** | `src/sni.cpp:37,39` `kMaxHead = 16*1024`、`kHeadReadMs = 10000` | ✓ |
| 端口 | **7899 / 7900** | `src/config.cpp:169–170`、`config.example.json:3–4` | ✓ |
| 源码文件数 | **14 `.cpp` + 1 `.hpp`** | `ls src/*.cpp \| wc -l` → 14；`ls src/*.hpp \| wc -l` → 1 | ✓ |
| 二进制大小 | **1,761,792** 字节 | `stat -c '%s' svpn.exe` → 1761792 | ✓ |

### 3.2 格式与完整性

```
$ wc -l README.en.md
272 README.en.md                      # 目标 200–280，达标

$ head -c 3 README.en.md | od -An -tx1
 23 20 73                            # "# s"，无 BOM（EF BB BF）

$ grep -c $'\r' README.en.md
0                                     # 纯 LF

$ awk '/^\|/{c=gsub(/\|/,"|"); printf "%s:%s ", NR, c}' README.en.md
51:5 … 66:5   89:3 … 94:3   117:4 … 130:4   142:4 … 145:4   161:4 … 167:4   175:3 … 186:3
# 6 个表格块内部管道数各自一致；仅第 54 行（`up` 行）为 8，因 `whitelist\|all\|blacklist`
# 与 `socks5://…\|http://…` 含 3 个 GFM 转义管道符，属预期

$ grep -c '^| `' README.en.md
42                                    # 子命令 14 + 配置键 12 + 高级键 2 + 运行期文件 10 + probe 判定 4

$ grep -n "进行中\|TODO" README.en.md
（无输出）                             # 占位文本已全部覆盖
```

标题层级核对：`grep -n '^#\{1,3\} ' README.en.md` → 1 个 `#`、11 个 `##`、8 个 `###`，与中文版逐节对应，
无跳级。

### 3.3 工作区隔离

`git status --short` 中本人只涉及两项新增：`?? README.en.md`、`?? _crew/reports/D4-readme-en.md`。
其余 `M`/`??` 均为他人文件（`src/main.cpp`、`src/svpn.hpp`、`build.cmd`、`.gitignore`、`docs/`、`tools/` 等），
本人未改动一个字节。

## 4. 契约变更

**对他人无影响**：只新增一个文档文件，不改源码、不改接口、不动 `README.md`。

以下是核对中发现的**他人文件里的问题，均未擅自修改**，请 Lead 裁决：

### 4.1 `README.md:83–85` — 子命令表格被截断（建议修）

中文版把 `svpn-gui.exe` 的说明段落插在 `gui` 行与 `pac` 行**之间**，导致 14 行表格断成两截：

```
81: | `gui`  | … | `0` |
82:
83: `svpn-gui.exe` 是同一个程序的窗口子系统构建…   ← 插在表格中间
84: | `pac`  | `--apply` | … |              ← 这行与 steam/doctor 失去表头
85: | `steam`| …
```

渲染后 `pac`/`steam`/`doctor` 三行会变成一个无表头的独立表格。英文版已把该说明移到表格之后（`README.en.md:68`）。

### 4.2 `README.md:9` — 二进制 md5 与当前工作区不符（建议改法）

| | README 写的 | `md5sum` 实测（当前工作区） | 字节数 |
|---|---|---|---|
| `svpn.exe` | `21f075119109057e75d3908abbb393b6` | `6f068265d4e0217c85cb06e28d381aca` | 1,761,792（**两边一致**） |
| `svpn-gui.exe` | `f27d313fb49fd6b9761c158830332bb9` | `401103a4b94bd6527a870385310058bb` | 1,759,744（**两边一致**） |

字节数对得上、md5 对不上，说明文档写的是某个历史构建的指纹。而且 Lead 本轮还要重编，重编后 md5 必然再变。
**建议**：删掉硬编码 md5，只保留字节数 + "以发布包 `SHA256SUMS.txt` 为准"。英文版已按这个思路写。

### 4.3 `README.md:5` — "约 1.6MB" 与 `docs/BUILD.md:3` 的 "各约 1.7MB" 不一致（低优先）

实测 1,761,792 字节 = 1.68 MiB = 1.76 MB（十进制）。按十进制说法"1.6MB"偏小，"1.7MB"更接近。
英文版写的是精确字节数 `1,761,792 bytes`，回避了这条。两处是否统一由 Lead 定。

### 4.4 `_crew/SPEC-1.1.1-release.md:22` — 版本号来源写错位置（**重要，影响 P4**）

SPEC §1 写"版本号来源 | `src/svpn.hpp` 的 `kVersion`"，但实际定义在
**`src/main.cpp:22`**：`constexpr const char* kVersion = "1.1.1";`，`src/svpn.hpp` 里根本没有 `kVersion`
（`grep -rn "kVersion" src/` 只命中 main.cpp）。若有人按 SPEC 去 hpp 找会白找；若 P4 把版本常量往 hpp 挪，
会与 main.cpp:22 重复定义。建议只修 SPEC 措辞，不动代码。

### 4.5 `src/pac.cpp:93` — 生成物注释里提到不存在的 flag（低优先，属他人文件）

```cpp
out += "// svpn 自动生成，重跑 `svpn pac --write` 覆盖\n";
```

`--write` 不是合法 flag，且程序"不认识 flag 一律静默忽略"（`README.md:97`）。这行注释会被写进用户生成的
`runtime/svpn.pac`，误导用户。建议改为 `svpn pac`。不影响功能，英文版未复述该注释。

## 5. 没做完/风险

1. **未实测任何命令**。本任务只写文档，未跑 `svpn.exe`；所有行为断言均取自 `README.md` + 源码行号交叉核对，
   未做运行验证。受影响的点：`[+]`/`[i]` 前缀行、`status` 各字段的措辞、probe 四种结论的英文译名（`direct OK` /
   `needs node · OK` / `needs node · SNI blocked` / `unreachable · check`）——**这些英文串是我按语义定的，
   程序实际输出仍是中文**。若将来要出英文 CLI，需另行统一，本轮不做。
2. **`probe --json` 的 JSON 字段名未写入英文版**。中文版同样没列字段名，英文版保持一致；需要 schema 的话建议
   另起 `docs/` 或在 `--help` 里给。
3. **声明章节的法律措辞是按中文语义重写，非法律意见**。免责段落的"跨境网络访问请遵守所在地法律法规"保留原意，
   未做属地法律适配。
4. **`README.en.md` 未加入发布包**。`dist/` 打包脚本属 Lead，是否把英文版一并放进 zip（甚至作为 `README.md`
   的 GitHub 默认展示）需 Lead 决定；我没动 `dist/`。
5. **目录交叉引用未做**：英文版只链了 `docs/BUILD.md` 与 `LICENSE`，未链本轮新出的 `docs/ARCHITECTURE.md`
   （W3）与 `docs/WINDOWS.md`（W4）——它们的英文标题与是否完工我无法确认，等 Lead 定稿后可以补两行。
