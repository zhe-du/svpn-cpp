# D1-docs 交付报告

角色：给 svpn（C++ 重写版）写交付文档。
约束遵守情况：**没有碰 `src/`、`tests/`、`svpn.exe`**；只新增 5 个文件（`README.md`、`LICENSE`、`run.cmd`、`docs/BUILD.md`、`.gitignore`）与本报告。

## 1. 写了哪些文件

| 路径 | 大小 | 说明 |
|---|---|---|
| `svpn-cpp/README.md` | 21KB | 定位/不做什么、快速开始、13 子命令表、配置、运行期文件、与 Python 版关系、已知差异 11 条、编译入口、六段声明 |
| `svpn-cpp/LICENSE` | 新增 | MIT，`Copyright (c) 2026 zhe-du`，与 Python 版逐字相同 |
| `svpn-cpp/run.cmd` | 新增 | 双击启动器；**33 个 CRLF、0 个裸 LF、0 个非 ASCII 字节**（已用 Python 校验） |
| `svpn-cpp/docs/BUILD.md` | 新增 | 工具链、完整编译命令、`-static`/`-municode`/字符集开关的原因、`build.cmd` 的作用、14 条常见错误对照、最小自检 |
| `svpn-cpp/.gitignore` | 新增 | 仅 6 条：`*.o`、`build/`、`runtime/`、`config.json`、`tests/_*.exe`、`tests/_*.cpp` |

动笔前读过的基准文件：`D:\LE\AI\dshwork\steam-vpn\README.md`（含「声明」六段、§2 CDN 两种接法、§3 命令、§5 配置）、`svpn-cpp/src/main.cpp`（全文 1040 行）、`src/config.cpp`、`src/svpn.hpp`、`src/rules.cpp`（decide）、`src/proxy.cpp`（头部 + SOCKS5 命令/鉴权）、`src/cdn.cpp`（测速与常量）、`src/sni.cpp`（白名单/读头限制）、`src/winsys.cpp`（托管块标记、原子写）、`src/util.cpp`（`Paths::resolve`）、`build.cmd`、`config.example.json`。

## 2. 逐节内容

### README.md

1. **标题 + 一句话**：单 exe 约 1.6MB、静态链接、零 DLL 依赖；点明是 Python 版的原生重写，配置双向兼容。
2. **它不做什么**：5 条——不是 VPN/无 TUN、不内置节点、不代理 UDP（`cmd != 0x01` 直接拒）、不做 TLS 中间人、不装服务/不自启/不注入/不加载驱动。
3. **快速开始**：`init` → `up -b` → 代理指向 `127.0.0.1:7899` / `:7900` → `down`；另给 `system on/show/off`。提到 `run.cmd`。
4. **子命令表**：13 行（`init up _serve down status probe node cdn hosts system pac steam doctor`），逐行写参数与退出码；表后 7 条「细节」把参数的真实生效范围写清（见第 4 节）。
5. **probe 的 9 个端点** + 四种结论 + `--json` 不是纯 JSON + `--lenient` 语义。
6. **配置**：12 键表（默认值/说明）、5 条规范化规则、2 个高级键（`proxy_suffixes` / `direct_suffixes`，替换非追加、空表回落、`init --force` 会丢）、优先级链、CDN 两种接法对照表 + SNI 边界。
7. **运行期文件**：9 条路径表，含 `hosts.backup` / `wininet-backup.json` 的隐私提示。
8. **与 Python 版的关系**：行为基准、配置与 cdn-cache 互换、不需要 Python、并存会抢端口与 hosts 托管块。
9. **已知差异与限制**：11 条，都是对着源码核过的（详见第 4 节）。
10. **编译**：指到 `docs/BUILD.md`，一句话给全套 flag 与 13 个源文件清单。
11. **声明**：用途 / 数据与隐私 / 安全 / 兼容性 / 免责 / 许可 六段，事实全部对齐 C++ 实现（日志 `runtime/svpn.log`、托管块标记原文、只写 `HKCU`、hosts 按字节处理、DoH 四家服务商、`hosts.tmp` 原子替换）。

### LICENSE

与 Python 版 `LICENSE` 完全一致（MIT，2026 zhe-du）。

### run.cmd

`@echo off` → ASCII 注释两行 → `chcp 65001 >nul` → `cd /d "%~dp0"` → 无 `config.json` 时 `svpn.exe init`（失败跳 `:failed`）→ `svpn.exe up -b` → `timeout /t 2 >nul` → `svpn.exe status` → 提示行 → `pause`。失败分支另打一行指向 `runtime\svpn.log` 再 `pause`。
**全文件 ASCII**（连提示语都是英文），因此不存在「CP936 起始控制台行错位」的触发条件；CRLF 已校验。

### docs/BUILD.md

7 节：工具链要求表 → 一条可复制的完整编译命令（cmd `^` 续行 + Git Bash 提示）→ 必须 `-static`（含 `-s`/`-mconsole` 的取舍、退出码 127 的原因）→ 必须 `-municode`（`wmain` → UTF-8 的原因、忘加会 `undefined reference to WinMain`）→ `build.cmd` 做的四件事（并注明 `build\` 目录当前不被使用）→ 14 行常见错误对照（缺 `-lws2_32` / `-lsecur32` / `-lcrypt32` / `-lwinhttp` / `-ladvapi32` / `-lshell32` / `-liphlpapi` / `-lole32 -luuid`、忘 `-municode`、字符集、忘 `-static`、误加 `-mwindows`、`g++` 不在 PATH）→ 最小自检三步。

## 3. 核对结果

每个写进 README 的参数都能在源码里找到出处（`main.cpp` 行号）：

| 命令/参数 | 源码位置（`src/main.cpp`，10:37 版行号；二进制为 10:35 构建） |
|---|---|---|
| `init --force` | :216 |
| `up -b/--background` `-p/--port` `-m/--mode` `--node` `--system-proxy`（含后台透传 `--listen-port/--mode/--node`） | :234、:270-285 |
| `_serve --log-to-file` `--system-proxy` `--listen-port` `--mode` `--node` | :344-348 |
| `down --keep-system` `--clear-hosts` | :442 |
| `probe --node none` `--timeout`(6) `--json` `--lenient` | :603 |
| `node scan/set/clear`（子命令在 `pos[2]`，URL 在 `pos[3]`） | :679-681 |
| `cdn --refresh` `--no-bench` `--domain` `--timeout`(8) | :776 |
| `hosts apply/clear`（默认 apply）`--no-elevate` `--timeout`(8) | :817-819 |
| `system on/off/show`（子命令在 `pos[2]`，默认 show） | :901 |
| `pac --apply`（`--write` 无判定） | :934 |
| `steam --system` `--exe` | :973 |
| `doctor`（含 config 解析错误） | :1001 |
| `-h/--help/help` 任意位置短路 → 用法 rc=0 | :1043-1046 |

配置 12 键 + 2 个高级键与 `config.cpp:23-27` 的 `kTopKeys` / `kExtraKeys` 完全一致，默认值与 `default_config()`（config.cpp:209-236）一致。文档里所有「默认值」都取源码，不取 Python 版（例如 `probe --timeout` 默认 **6** 秒、`cdn --timeout` 默认 **8** 秒，与 Python 版的 4 / 3 不同，文档按 C++ 写）。

其它核对：9 个 probe 端点 = `kEndpoints`（main.cpp:30-40）；16 个 node scan 端口 = `kCommonProxyPorts`（main.cpp:42-45）；DoH 四家 = cdn.cpp:28-31；1MB / 前 3 名 / 最多 6 候选 / top 5 = cdn.cpp:49-53；SNI 读头 16KB / 10s = sni.cpp:37-39；中继空闲 1h = proxy.cpp:35；hosts 托管块标记原文 = winsys.cpp:29-30；`runtime/` 9 个文件名 = util.cpp `Paths::resolve`。

`run.cmd` 未实跑（见第 5 节），只做了静态校验：CRLF 33 个、裸 LF 0 个、非 ASCII 字节 0 个、`if errorlevel` 分支无误。

## 4. 与源码不一致之处（**需要 Lead 决定要不要改代码，我没有动 src/**）

按严重程度排：

1. **`up -b` 会丢掉 `-p/--port`、`-m/--mode`、`--node`**。`cmd_up` 改的是父进程内存里的 `cfg`，随后 `CreateProcess` 只传 `"exe" _serve --log-to-file`，子进程 `cmd_serve` 重新 `load_config(p)` 读磁盘（main.cpp:269-282 vs 322）。后果：`svpn.exe up -b -p 17899` 必然报「后台启动失败」——父进程在等 17899，子进程在 7899。前台模式（不加 `-b`）这三个参数是正常的。我在 README「子命令表」和「已知差异 1」都如实写了「后台改配置请改文件」。**这是与 Python 版最大的行为差**（Python 的 `--node` 会写回 config.json）。
2. **`up --system-proxy` 不接管系统代理**。main.cpp:273 把它拼进子进程命令行，但 `cmd_serve`（main.cpp:321-384）只看 `--log-to-file`，全项目 `apply_wininet` 的调用点只有 `system on`（:838）、`pac --apply`（:861 备份）、`steam --system`（:897）。README 已写明「用 `system on`」。
3. **`auto_node` 读了不用**。`config.cpp:173/216/245` 三处只做读写，`main.cpp` 无任何自动发现逻辑；`up` 的用法行也没提 `--no-auto`。README 已写「当前版本不参与决策，要自动找跑 `node scan`」。
4. **`pac` 的用法文案写了 `--write`，代码里没有**（用法文本仍列 `--write`，`cmd_pac` 只判 `--apply`）。所以 `pac --write` 与 `pac` 完全等价（都只落盘 `runtime/svpn.pac`），`--write` 是**空开关**。README 现在写的是「`pac` 本身就会落盘；`--apply` 才改 AutoConfigURL；`--write` 是空开关」。
5. **`probe --node <url>` 只认 `none`**。main.cpp:536-537 拿到 `node_arg` 后唯一动作是 `if (node_arg == "none") clear()`，传 URL 会被静默忽略（Python 会解析 URL）。README 已写明。
6. **`cert_cn` 取到了但没人显示**。`tls.cpp:164-174` 用 `SECPKG_ATTR_REMOTE_CERT_CONTEXT` 填充 `st->cert_cn`，`tls_probe` 也回填到 `TlsProbe`（:595），但唯一调用方是 `cdn.cpp:541`，`cmd_probe` 用的 `tls_connect` 不打印 CN。所以「probe 能取到 cert_cn」目前只在库层成立。README「已知差异 5」写成「SChannel 层能拿到，但 `probe` 表格当前不输出」——比直接说「能取到」诚实。**如果 Lead 打算在 probe 表格里加一列，改完告我一句，我把那句删掉。**
7. **链接库清单与简报不一致**。简报说包含 `dnsapi` 和 `wininet`，`build.cmd:22` 实际没有，源码里也没有 `DnsQuery*` / `WinHttpGet*Proxy*` 调用（`grep` 过，只有 `ShellExecuteExW`）。`docs/BUILD.md` 用的是 `build.cmd` 那一套（已验证能产出 1.6MB 的 exe），**没有**多写两个库。
8. `build.cmd` 会 `mkdir build`，但编译是一步到 exe、不落 `.o`；`build\` 目录当前没被使用（根目录那堆 `build_*.o` 是别的脚本产物）。BUILD.md 如实注明「可删」。

### 4.2 与 V1 CLI 审计 8 条的逐条对照（**中间态快照：10:30 前后，已被 4.3 取代**）

> 本节记录的是「V1 首轮审计 → 我第一版改稿」之间的事实。Lead 之后把 8 条一次性全修了（新构建 `cd13a3219c323469ed144d3881e8ae30`），**最终口径见 4.3**，本节只作为过程留痕。

| V1 条目 | 复核结论 | 文档处置 |
|---|---|---|
| ① 子命令一律取 `pos[1]`（`node set/clear` 失效、`system *` 全挂、`hosts clear` 反向 apply） | **已修复**。源码 10:29 版改成 `pos[2]`（:627/:636/:764/:848，带注释）；V1 后续在干净目录复测构建 **`10460aa7`（10:30:41）**：`system show` rc=0 打印四行、`node clear` rc=0、`hosts clear` 沙箱下字节保真。10:30 那份 exe 我也实测过 `system show` rc=0。V1 首轮审的 `f3a8fa7b`（10:29:07）是旧二进制。 | 按**修复后**写，并在子命令表前加「版本基线」引用块：写明 `10460aa7` 及之后、旧构建的三类症状、以及一行自查（`svpn.exe system show` 是否打印四行）。`hosts` 默认 apply 仍按事实警告（那是设计，不是 bug）。 |
| ② `probe --json` 不是合法 JSON | 成立 | 原文已写「先表格后 JSON」，本次补上「`\| jq` 会失败，脚本读 `runtime/probe.json`」。 |
| ③ `--help`/`-h`/`help` 全不支持，`probe -h` 会真跑 | 成立（实测 `--help` rc=2） | 新增细节条目：没有 `--help`；`-h` 会被静默忽略。 |
| ④ `pac --write` 是空开关 | 成立 | 改成「`pac` 本身就落盘，`--apply` 才改 AutoConfigURL，`--write` 是空开关」。 |
| ⑤ `--lenient` 与严格模式 rc 恒等 | 成立（:592-593 两个计数器同增同减，:619-620 判据换了个名字但值相同） | 删掉原「只放宽…」的承诺，改成「没有任何实际效果」，并写进已知差异第 8 条。 |
| ⑥ 三桶口径自相矛盾 | 成立。无节点时行内是「需节点 · SNI 阻断」，但汇总的「需节点」只统计 `node_ok`（有节点且真通），所以显示 0；「不通」与「不可用」是同一批 | 把 probe 小节重写成「行内四种结论表 + 汇总口径说明」，明确点出两个数字恒等、不要对号入座。 |
| ⑦ `hosts` 省略子命令 = apply，无二次确认 | 成立且危险（会弹 UAC 改真实 hosts） | 子命令表标「省略即 `apply`」，细节第一条加 ⚠ 加粗警告，已知差异第 13 条再提一次。`hosts clear` 已可用，不再写「无法撤销」。 |
| ⑧ `doctor` 对损坏 config 判 OK | 成立（`load_config` 解析失败静默回落默认值，只往日志写 WARN；`doctor` 只看文件存在性） | doctor 行改成「只查是否存在、不校验语法」，已知差异第 10 条写清，BUILD.md 末尾也加了提醒。**建议 Lead 让 `load_config` 把解析错误透给 `doctor`，这条就能从文档里消失。** |

结论：V1 的 8 条里 **1 条已被代码修掉（P0）**，其余 7 条都是「帮助文本 / 文档承诺与实现不符」，已全部改成描述真实行为，没有留「未来会支持」。

### 4.3 最终态：Lead 全量修复后，哪些是真 bug、哪些是测试假象

**唯一基准（冻结配对，两者必须同时成立）**：

| 对象 | md5 | 大小 | mtime |
|---|---|---|---|
| `svpn.exe` | `e634fe15067808ccf2ba0dad30d55d97` | 1,678,848 B | 2026-10-25 10:46:27 |
| `src/main.cpp` | `ef09fc1c11844a04a76aae86b94c871b` | 49,232 B | 2026-10-25 10:45:03 |

> 只写 exe md5 挡不住「exe 没变、源码又改了」的错配；这一对同时对上，才能说「文档描述的就是这个二进制 + 这份源码」。
> **任一 md5 或 mtime 变了，本文档的「已修/已验」结论即作废**，需重新核对（V1 的审计报告第 9.3 节用的是同一对，两边可互相校验）。
> 基线一路在动：`cd13a321`(10:35) → `b5c97728`(10:40) → `439fd9dc`(10:40:29，V1 的验收基线) → `702be7f8`(10:42) → `ec6d47eb`(10:43) → **`e634fe15`(10:46，size 变了 = 有实际改动)**。我在 702be7f8 / ec6d47eb / e634fe15 上都跑了同一组探针，结论一致；文档锚在最新的 e634fe15。

| # | V1 的判定 | 最终状态 | 我的验证方式 |
|---|---|---|---|
| ① 子命令取 `pos[1]` | 真 bug（P0） | **已修**：改取 `pos[2]`（`node`/`hosts`/`system` 三处），并给 `down --clear-hosts` 的提权链恢复 | 隔离目录实测：`system show` rc=0 打四行、`node set`（无参）rc=2 提示用法、`node clear` rc=0 |
| ② `--json` 污染 stdout | 真 bug | **已修**：`human` 流在 `--json` 时指向 `std::cerr`，stdout 只留 JSON | 隔离目录 `probe --json >o.json 2>e.txt`：`json.loads(o.json)` 通过（9 rows）、e.txt 16 行含表格与汇总 |
| ③ `--help`/`-h` 不可用 | 真 bug | **已修**：argv 任意位置出现三者都直接打用法、rc=0 | 实测 `--help`、`probe -h` 都只出用法，无探测输出 |
| ④ `pac --write` 空开关 | 文案问题（不是功能 bug） | **保留空开关**，帮助文本仍列着；`--apply` 才是唯一有效参数 | 源码核对（`cmd_pac` 只判 `--apply`） |
| ⑤ `--lenient` 与严格模式恒等 | 真 bug | **已修**：桶拆开后 `--lenient` 只按 `dead`，没配节点时返回 0 | 源码 + 实测汇总行 |
| ⑥ 三桶口径自相矛盾 | 真 bug | **已修**：`直连可用 / 需节点 / 死端点` 三桶互斥，`当前不可用 = 需节点 + 死端点`（第四个数是换算），标签也从「不通/不可用」改成「死端点/当前不可用」 | 实测 stderr 末行 `[i] 直连可用 7 / 需节点 2 / 死端点 0 / 当前不可用 2`（无节点时「需节点」终于非 0） |
| ⑦ `hosts` 默认 apply | **设计如此，不是 bug** | 保留，但文档加 ⚠ 警告（会直接改真实 hosts、无二次确认） | 源码核对（`pos[2]` 缺省 = `"apply"`）；**没执行** |
| ⑧ `doctor` 对坏 config 判 OK | 真 bug | **已修**：`load_config(p, &cfg_err)` 的错误透到 doctor，计入 issues | 隔离目录写 `{"broken":true,}` → `[!] config.json 解析失败: …（当前按默认值运行）`，rc=**1** |

Lead 另外修了三条 V1 没列进缺陷清单的：`probe` 表格「结果」列宽 14→20（分隔线 92；注意 `%-20s` 按**字节**补齐，CJK 与英文混排时显示宽度仍会有细微差异，V1 说的「列宽问题」应该指这个——文档不承诺像素级对齐）、`cdn` 无下载路径显示 `-` 而非 `0.00MB/s`、`build.cmd` 补 `-ldnsapi -lwininet`（`DnsFlushResolverCache` / `InternetSetOptionW` 未定义，之前用它独立构建会链接失败）。

**`b5c97728` → `702be7f8` 新增/变化（这几条是 V1 消息之后才出现的，V1 的对应结论已过期）**：

| 变化 | 证据 | 文档处置 |
|---|---|---|
| `status`/`down` 改读 `runtime/effective.json`，显示**实际生效**的端口/模式 | `main.cpp:388-400` 写、`:501-510` 读、`:464` down 时删；`up -b -p 17899 -m blacklist` 后 status 报 17899/blacklist | 子命令表 status 行标注「取实际生效值」，运行期文件表新增 `effective.json`，细节里写清「覆盖项不写回 config.json，下次不带参数 up 会回退」 |
| `doctor` 新增**未知键**检查 | `{"broken":true}` → `[!] config.json 里有 1 个未知键（拼错了？…）: broken`，rc=1 | doctor 行补「有没有未知键」 |
| `doctor` 报错前缀不再重复 | `[!] config.json 解析失败: bad literal（当前按默认值运行）`（此前是 `…解析失败: config.json 解析失败: …`） | 文档一律用省略号写，不逐字抄 |
| `probe --node <url>` 现在能解析 URL（此前只认 `none`） | `main.cpp:610-616` 调 `apply_node_url`，失败返回 2 | 子命令表改成 `--node <url>` / `--node none`，删掉旧「已知差异 5」，并注明「只影响本次、不落盘」 |
| `--lenient` **不再等价**于严格模式（V1 说法过期） | 实测同一次探测：严格 rc=1、`--lenient` rc=0，计数 `直连可用 6 / 需节点 3 / 死端点 0 / 当前不可用 3` | 文档保留「只按死端点判」的写法 |

**`e634fe15`（10:46，size 1,678,848）相对上一版的实际改动**：

- `status` 现在**也读 `effective.json` 的 `upstream` / `upstream_configured`**（main.cpp:503-515），所以 `up -b --node socks5://…` 之后 status 的「节点」行显示实际值，不再是 `config.json` 里的旧值。V1 记的 ⑮（P3）在文档定稿前已被 Lead 修掉，所以**没有**写进「已知差异」。
- `pac` 的帮助文本去掉了 `--write`（现为 `pac 生成 PAC（--apply 同时写入系统代理）`）。实测 `pac --write` 与 `pac` 产物 md5 同为 `1ad12b44…`、rc=0：**未知 flag 一律静默忽略**，文档按这个口径写，不再说「帮助里列着一个空开关」。

**V1 的测试假象 / 不适用结论**（不是它们错，是环境或方法导致）：

1. `up -b` 第二次换 pid —— V1 自己已推翻，原因是每条 shell 命令结束时进程树被回收。
2. 管道取退出码：`cmd | head` 拿到的是 `head` 的 rc，不是 `svpn` 的。要 rc 必须 `${PIPESTATUS[0]}` 或重定向到文件（我这次全程 `>o.json 2>e.txt`，没踩这个坑）。
3. `hosts clear` 的「反向执行 apply」是**纯静态推导**（V1 明确说没执行），结论对，但证据是代码不是运行。
4. `f3a8fa7b`（10:29:07）不能代表当时源码：`main.cpp` 10:27 就改成 `pos[2]` 了，属于「源码先改、exe 后编」的时间窗错配。这也是我第一版文档误判「P0 仍存在」的直接原因。

**我实际跑过的验证**（全部在 `/tmp/d1probe` 的 exe+config 副本里，独立 `runtime/`，没碰工程根的 config/runtime，也没碰真实 hosts）：`--help` / `probe -h` / `system show` / `doctor`（坏 JSON）/ `node set` / `node clear` / `probe --json` 的 stdout-stderr 分离与桶计数。
**没跑**：`up -b`（后台进程会被本工具回收 → 假阴性）、`--system-proxy`（改 `HKCU`，不实测）、`hosts`（改真实 hosts）。这三条只做源码核对，并在文档里按代码路径描述。

## 5. 没做完

- **`run.cmd` 没有实跑**。实跑会 `up -b` 起一个常驻实例并抢 7899/7900，与 Lead 正在做的 TLS/测速验证冲突；只做了字节级校验（CRLF / 无裸 LF / 全 ASCII）与语法走查。建议 Lead 在本机空跑一次确认 `pause` 与 `errorlevel` 分支。
- **没有英文版 README**（Python 版有 `README.en.md`）。本轮需求只点名 `README.md`，未扩展。
- **没有写 `docs/ARCHITECTURE.md` / `docs/WINDOWS.md`**（Python 版有）。README 末尾只链到 `docs/BUILD.md`，没有留死链。
- **`svpn.exe doctor` / `status` 的实际输出没有逐字贴进 README**（怕版本迭代后过期），只描述字段含义。
- 4.1 里那三条（`up -b` 参数、`--system-proxy`、`cert_cn`）**已经全部落地**：Lead 在 10:35 的构建里修了前两条，文档已按修复后写；`cert_cn` 仍只在库层，CLI 表格没这一列。
- **V1 审计后的改稿已完成并同步到 e634fe15**（见 4.2 → 4.3）：8 条按修复后的行为重写，`已知差异` 从 14 条精简到 **10 条**（auto_node / 半开连接 / CDN 实测范围 / 443 占用 / cert_cn / 偶发抖动 / node scan / 上游协议 / hosts 权限 / Windows-only）。`probe --node` 已支持 URL，从差异里删掉。README 顶部指纹更新为 e634fe15，运行期文件表补了 `effective.json`，status/doctor 两行按新行为改写。
- `svpn.exe` 在这次复核中一直是可用的（md5 `cd13a3219…` 没再变），所以除了 `up -b` / `--system-proxy` / `hosts` 三条刻意不跑，其余都做了真机验证。
