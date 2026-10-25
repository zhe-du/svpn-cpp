# R4 流程文档补齐（打包流程 + 日志分析）

## 1. 摘要

1. 补上总日志 §6.4.3 记的两笔欠账：`_crew/reports/packaging.md`（打包流程固化）与
   `_crew/reports/log-analysis.md`（日志与运行期数据分析），均为可照着做的操作手册。
2. 打包文档按 **1.1.1 双产物**（`svpn.exe` + `svpn-gui.exe`）重写，没有沿用 1.1.0 单产物口径：
   包内 7 文件 → 9 文件，`SHA256SUMS` 覆盖 6 项 → 8 项，`RELEASE.txt` 模板改为双 exe + 15 个源文件。
3. 日志文档把 8 个运行期文件逐字段拆开，全部用本机 `runtime/` 的真实样本举例；
   排障表 13 条，含任务点名的 8 个真实案例。
4. 过程中**纠正了 5 处与源码/磁盘不符的既有说法**（见 §3.3），全部有命令输出支撑。
5. 未实跑的有三类：`build.cmd`、`test_cli.py`、发布门禁——原因与替代依据见 §5。

## 2. 做了什么

### 2.1 `_crew/reports/packaging.md`（776 行）

12 节，按「能照着敲」组织，命令用代码块、期望输出紧跟其后：

| 节 | 内容 | 关键点 |
|---|---|---|
| 1 | 前置检查 | `netstat -ano \| grep -E ":(7899\|7900) "` + 443；**为什么必须先查**（`up` 的「已在运行」是端口探测，`main.cpp:255-261`，残留实例会让门禁恒真） |
| 2 | 构建 | `build.cmd` 双产物；`-mconsole` vs `-mwindows -DSVPN_GUI_SUBSYSTEM`；`-static/-s/-municode` 各自少了会怎样；零警告硬门槛；`objdump` 零 DLL 验证（23 条全系统 DLL） |
| 3 | 全量测试 | 三套命令 + 期望值（178 / 27 / 113+）；环境纪律 `unset NODE_OPTIONS http_proxy …`；SKIP>0 视为异常 |
| 4 | 发布门禁 | 三条写在**同一个 shell 块**里；`up -b -p 17899 -m blacklist` → 再 `up -b` 须 rc=0 且 pid/`effective.json` md5 不变 → `down` 后三端口全释放 |
| 5 | 打包清单 | 9 个文件逐项（双 exe + 中英 README + LICENSE + run.cmd + config.example + RELEASE + SHA256SUMS）；泄漏扫描零命中 |
| 6 | 只读位 | 两个 exe 都 `attrib +R`；顺序「先复制→再设只读→最后打包」；1.1.0 漏设的后果（R3 §8.1） |
| 7 | `RELEASE.txt` | 完整模板：版本 / 两个 exe 的 MD5 与字节 / 构建时间 / 编译器全串 / 平台 / 脚本 / **15 个源文件指纹** / 行为基准 commit / 验证步骤 / 包内清单 / 已知问题；指纹生成命令实测可用 |
| 8 | `SHA256SUMS.txt` | `sha256sum --text`（本机默认输出 `hash *file`，与 1.1.0 的两空格格式不同）；覆盖 8 项；`-c` 自校验 |
| 9 | zip | 命名 `svpn-1.1.1-win64.zip`、单层目录；**实测 `tar -a` 不产出 zip**，必须 `--format zip` 或 7-Zip |
| 10 | 提交与打标 | 三个 commit（源码/文档/发布物）→ `tag v1.1.1`（只钉不变项）→ `git push` → `gh release create` |
| 11 | checklist | 15 条坑，逐条给「怎么验 + 破了会怎样」 |
| 12 | 一页速查 | 从查端口到 `gh release create` 的完整序列 |

### 2.2 `_crew/reports/log-analysis.md`（622 行）

| 节 | 内容 |
|---|---|
| 1 | 排障顺序 `status` → `probe` → `svpn.log` → `stats.json`；每步「看什么 / 什么算异常 / 异常怎么办」，含 `status` 10 行逐行判据表和 `probe` 退出码表 |
| 2 | 8 个运行期文件逐字段：`svpn.log`（两种行形态的来源、`--log-to-file` 重开陷阱、CRLF/无 BOM/无轮转）、`stats.json`（8 键 + 累加点 + 5 秒周期 + 为什么不能退出时写）、`probe.json`（4 verdict × `ms` 三态读法）、`cdn-cache.json`（逐字段 + 过期判据 + `max_age=10**9` 事故）、`effective.json`（含 1.1.1 新增 `upstream_kind`）、`wininet-backup.json`、`svpn.pid`、`hosts.backup` |
| 3 | 症状 → 判据 → 处置 **13 条**，覆盖任务点名的 8 个案例与 5 个补充案例 |
| 4 | 22 个数字的出处表 + 一次性复核命令块 |
| 5 | 8 条读数据时必须知道的局限（`via_*` 不含 SNI、`probe.json` 不落 `direct_ok`、`rows` 顺序不稳定等） |

### 2.3 任务点名的 8 个案例对应位置（log-analysis.md §3）

| 点名案例 | 表中的 # |
|---|---|
| `stats.json` 乱码 / `status` 缺「上次统计」 | 1 |
| 设了代理但浏览器没用上（PAC 非法） | 2 |
| `up -b` 报端口被占但 `down` 说没在运行 | 3 |
| `node scan` 把 svpn 自己当上游 | 4 |
| 社区/头像打不开但商店正常 | 5 |
| `probe` rc=0 但社区不通（`--lenient`） | 6 |
| 下载慢（`cdn-cache` 过期 / hosts 有记录） | 7 |
| 半开连接让连接数迟迟不降 | 8 |

补充：9 日志不更新、10 `status` 端口与 `config.json` 不一致、11 GUI 按钮无响应、
12 `down` 后仍走代理、13 443 被占导致 SNI 静默降级。

## 3. 验证

### 3.1 两份文档里的数字，全部有命令或行号

| 数字 | 值 | 来源 |
|---|---|---|
| 源文件数 | 15 | 实测 `ls src/*.cpp src/*.hpp \| wc -l` → `15` |
| probe 端点数 | 9 | `grep -c '", 443, "' src/main.cpp` → `9`；`src/main.cpp:30-40` |
| `test_config` | 178 | 实测 `./tests/test_config.exe` → `[+] PASS 178 / FAIL 0（共 178 项）`，rc=0 |
| `test_sni_parse` | 27 | 实测 `./tests/test_sni_parse.exe` → `[+] PASS 27 / FAIL 0（共 27 项）`，rc=0 |
| `test_cli` | 113+ | SPEC §5（Lead 本轮复跑 113/0/0）；未实跑，见 §5 |
| `status` 行数 | 10 | 实测 `./svpn.exe status \| wc -l` → `10` |
| `svpn.log` 样本 | 2,304 B / 45 行 | 实测 `wc -c` / `wc -l` |
| `stats.json` 样本 | 223 B | 实测 `wc -c` |
| `probe.json` 样本 | 1,268 B，`unusable=3` | 实测 `wc -c` + 原文粘贴 |
| `cdn-cache.json` 样本 | 3,178 B / 8 域名 / `updated=1792895140` | 实测 `wc -c`、`python -c "len(d['domains'])"` |
| 缓存年龄 | 2.45 h = 147 min | `date -d @1792895140` → `2026-10-25 10:25:40`；`status` 输出「147 分钟前更新」互相印证 |
| 落盘周期 | 5 s | `src/main.cpp:421-429`（50 × `Sleep(100)`） |
| `top_hosts` 上限 | 64 / 落盘 8 | `src/util.cpp:159` / `:190` |
| `kMaxTop` | 5 | `src/cdn.cpp:53` |
| CDN TTL | 6 h | `src/svpn.hpp` `CdnCfg::refresh_hours{6}` |
| 中继 idle（TCP） | 3600000 ms | `src/proxy.cpp:34` |
| SNI idle | 0（无限） | `src/sni.cpp:192` |
| `kRelayBuf` | 32 KiB | `src/net.cpp:38` |
| exe 依赖 DLL | 23 条、0 条非系统 | 实测 `objdump -p svpn.exe \| grep -ci "DLL Name"` → `23`；`grep -Eic "libstdc\|libgcc\|winpthread"` → `0` |
| 1.1.1 产物快照 | `svpn.exe` 1,761,792 B / `d4c00335…`；`svpn-gui.exe` 1,759,744 B / `401103a4…` | 实测 `md5sum` + `stat`（12:46 那一刻；之后重编即失效，文档已标注为易变值） |
| `dist/svpn.exe` 只读位 | `A    R` | 实测 `cmd //c "attrib dist\svpn.exe"` |
| 1.1.0 发布包 | 699,901 B / `0704cb08…` / 7 文件 | 实测 `ls -l` + `md5sum` + `unzip -l` |
| 1.1.0 `SHA256SUMS` | 6 行 | 实测 `cat` + `sha256sum -c` 六条 OK |
| `run.cmd` 两份 | 根 823 B / `dist` 250 B | 实测 `ls -l` + `diff` |
| `build.cmd` 行尾 | `i/lf w/crlf attr/text eol=crlf` | 实测 `git ls-files --eol build.cmd` |
| 编译器 | g++ 16.2.0 x86_64-posix-seh-rev1 | 实测 `g++ --version` |
| 端口状态 | 7899/7900/443 全空闲 | 实测 `netstat -ano \| grep -E ":(7899\|7900) "` → 无输出，rc=1 |

### 3.2 工具链实测（写进打包文档前先验证过）

```
$ command -v zip unzip sha256sum md5sum
zip: MISSING            ← 本机没有 zip 命令，文档改用 7-Zip / bsdtar
/usr/bin/unzip
/usr/bin/sha256sum
/usr/bin/md5sum

$ tar -a -c -f s1.zip svpn-1.1.1-win64 && unzip -l s1.zip
End-of-central-directory signature not found.   ← tar -a 没产出 zip（实为 10240 B pax tar）
$ /c/Windows/System32/tar.exe --format zip -c -f s3.zip svpn-1.1.1-win64 && unzip -l s3.zip
  svpn-1.1.1-win64/          ← 单层目录，符合 1.1.0 的打包结构
  svpn-1.1.1-win64/a.txt
$ "/c/Program Files/7-Zip/7z.exe" a -tzip s2.zip svpn-1.1.1-win64 && unzip -l s2.zip
  svpn-1.1.1-win64/          ← 同上
```

（在 `D:\LE\AI\wbwrk\wbwrk_tmp\ziptest\` 里用假文件验证，验证完已删除，没在仓库留痕。）

另外两条必须先试才知道的：

- `svpn-gui.exe -v` 会**直接开窗并挂住**（实测被 SIGTERM 结束），
  所以版本只从 `svpn.exe -v` 取；
- `node --check runtime/svpn.pac` 报 `ERR_UNKNOWN_FILE_EXTENSION`（Node 22），
  必须先 `cp` 成 `.js`（实测 `node --check t.js` 通过）。

### 3.3 纠正的既有说法（原文 → 实测）

| # | 既有说法（出处） | 实测 | 判定 |
|---|---|---|---|
| 1 | R2 / V1：`effective.json` 是 4 键 `{listen_port, http_port, mode, upstream_configured}` | 1.1.1 落地 7 键，含 `upstream` 与 `upstream_kind`（`main.cpp:389-401`，W1 已合入） | 已按新契约写 |
| 2 | 总日志 §6.2.1 / 任务书：半开连接「彻底解决要加 idle timeout」 | TCP 中继**已有** 1h 兜底（`kRelayIdleMs=3600000`，`proxy.cpp:34`）；**只有 SNI 通道**传 `0` 无限等（`sni.cpp:192`） | 文档已改为精确表述 |
| 3 | R3 §1：1.1.0 包 699,771 B / `2146fb16…` | 磁盘上 `dist/svpn-1.1.0-win64.zip` = **699,901 B / `0704cb08…`**（11:02 重打过） | 文档用实测值，并注明 R3 是旧快照 |
| 4 | R3 §1：`SHA256SUMS.txt` 覆盖 5 个文件 | 实际 **6 行**（含 `RELEASE.txt`），`sha256sum -c` 六条 OK | 文档按 6 写，1.1.1 按 8 写 |
| 5 | `src/gui.cpp:214` 注释：「status 实际输出 9 行」 | `./svpn.exe status \| wc -l` = **10**（标题行 + 9 个字段行） | 文档按 10 写并注明注释漏算标题行 |
| 6 | `probe.json` 行内字段 `direct_ok`/`node_ok`（任务书表述） | 这两个字段**不落盘**，JSON 只有 `host/verdict/ms/usable`（`main.cpp:680-687`） | 文档单列一段澄清 |

### 3.4 越权与副作用自查

- 只新建 3 个文件：`packaging.md`、`log-analysis.md`、`R4-proc-docs.md`。
  `git status --short` 中属于 W6 的只有这三条 `??`。
- 未跑 `build.cmd`、未跑 `test_cli.py`、未起任何实例（除 `status` 这种只读命令与
  `tests/*.exe` 两套离线用例），未占 7899/7900，未 `system on`、未 `hosts apply`。
- 唯一写盘的副作用是 zip 工具验证用的 `wbwrk_tmp\ziptest\`，已清理；
  `runtime/` 全程只读（`probe.json` 在 12:46 被**别人**覆盖，不是本报告所为）。

## 4. 契约变更

无。

- 没有改任何 `src/`、`tests/`、`build.cmd`、`dist/`、README 或既有报告。
- 文档中描述的 `effective.json` 第 7 个键 `upstream_kind` 是 **W1（P4）** 的改动
  （`src/main.cpp:400` 与 `:527`、`tests/test_cli.py:710-734`），本文只是如实记录其契约，
  不构成对契约的修改。

### 顺带发现但**没有改**的跨文件问题

1. `docs/BUILD.md` 已过期：§5 说「`build.cmd` 编出根目录的 `svpn.exe`」、
   `[x] build failed` 文案（实际是 `[x] console build failed` / `[x] gui build failed`）、
   §3 说「不要加 `-mwindows`（会把标准输出吞掉）」——1.1.1 的双产物恰恰是 `-mwindows` + 独立 exe。
   建议 Lead 或 W3/W4 顺手对齐。
2. `src/gui.cpp:214` 注释「status 实际输出 9 行」与实际 10 行不符（仅注释，不影响行为）。
3. R3 / 总日志里 1.1.0 包的指纹与文件数已过期（上表 #3/#4），旧报告按规矩不改。
4. `runtime/probe.json` 在多路并行下会被任意一方覆盖；本报告引用的 12:46 那份内容可能再次变化。

## 5. 没做完 / 风险

### 5.1 本轮**未能实测**的环节（诚实列出）

| # | 项 | 为什么没测 | 替代依据 | 风险 |
|---|---|---|---|---|
| 1 | `build.cmd` 双产物重建 + 零警告 | `build.cmd` 与产物归 Lead；重编会覆盖正在用于打包的 exe，并打断其他并行任务 | 总日志 §5.4「源码干净重建，零警告，两个产物都产出」；`objdump` 与 `-v` 在现有产物上实测通过 | 若最终那次构建有警告，本报告的门槛描述仍成立，但「已零警告」这句需 Lead 复核 |
| 2 | `python tests/test_cli.py` 拿 113/0/0 | `tests/test_cli.py` 属 W1 且正在改（新增 `upstream_kind` 用例）；SPEC 要求同一时刻只许一份在跑，与 Lead 的发版复跑会撞车 | SPEC §5（Lead 本轮复跑 113/0/0） | 总日志 §3.3 记过 `111 PASS + 1 SKIP`，§6.4.2 仍列为待复核；**若发版时 SKIP≠0，本文的「SKIP 即异常」判据会立刻抓到** |
| 3 | 三条发布门禁现场实跑 | 需起实例占 17899/7900；并行窗口内起实例会与别人的测试互相干扰（V1 §9.2 明确要求「跑前确认没有他人实例」） | R2 §「在 e634fe15 上复现 V1 全套检查：全绿」与 V1 §7.5 的完整输出 | 门禁代码路径（`port_open` 双端口检查、`remove_pid_owned`）本轮未变动；但**1.1.1 的二进制**没跑过门禁，这是发版前必须由 Lead 补的最后一步 |
| 4 | 真实打一次 `svpn-1.1.1-win64.zip` | 打包归 Lead；我打出来的包没有权威性，且会污染 `dist/` | 在 `wbwrk_tmp` 用假文件验证了两种 zip 工具的结构与单层目录 | 只证明了「工具与结构可用」，没证明「9 个真实文件打出来 `unzip -t` 通过」 |
| 5 | `gh release create` | 对外可见、不可撤销，必须由 Lead 决定 | SPEC §1 的 gh 登录状态 | 命令形态（`--notes-file dist/RELEASE.txt`）未实跑 |
| 6 | `attrib +R dist\svpn-gui.exe` | `dist/` 里当前还没有 `svpn-gui.exe` | `dist\svpn.exe` 上实测已是 `A    R` | 双 exe 的 `+R` 只验证了一个 |
| 7 | GUI 两条入口（`svpn-gui.exe` / `svpn.exe gui`） | 实测 `svpn-gui.exe -v` 会开窗挂住，不适合在无人值守会话里点 | 总日志 §5.4「两个入口都能开窗，标题 `svpn 1.1.1 · Steam 分流/加速`，面板读真实状态」 | 打包文档不依赖 GUI 行为；但「双击不闪黑框」这条只有总日志的记录，没有本轮实测 |
| 8 | `.gitattributes` 对全新 clone 的效果 | 只能看到本工作区 `i/lf w/crlf`，clone 需要另建目录（会污染工作区） | `git ls-files --eol build.cmd` 实测 | `core.autocrlf=true` 的新机器上源码 MD5 是否真的稳定，未做端到端验证 |

### 5.2 双产物打包流程里**判断有风险**但本轮无法实测的点

1. **两个 exe 的字节一致性**：`svpn.exe` 与 `svpn-gui.exe` 是同一份源码编两次，
   但 GUI 版走 `-mwindows`，若构建时间跨秒，两个产物的构建时间戳会不同。
   `RELEASE.txt` 里「构建时间」只有一格——**应写哪一个**没有先例可依。
   本报告的建议是写两个 exe 各自的 mtime，但没有实测样例支撑，Lead 需现场决定。
2. **`SHA256SUMS.txt` 的项数**：本文按「包内 9 文件 − 自身 = 8 项」写。
   若 Lead 决定再放一个文件（例如 `docs/` 摘要或 `CHANGELOG`），项数要跟着变。
   `RELEASE.txt` 正文里那句「上述 8 个文件」是**硬编码在文字里**的，最容易漏改——
   1.1.0 就在这里错过一次（`a84dd7a` 的提交信息：「RELEASE.txt 文件数改对（5→6）」）。
3. **`README.en.md` 的内容是否与中文版同口径**：1.1.1 是首次带英文 README 发版，
   若英文版漏写「已知问题」或版本号，`RELEASE.txt` 与包内文档会自相矛盾。
   本轮 `README.en.md` 由 W5 维护，成稿时间晚于本文，无法交叉核对。
4. **zip 内的时间戳与可复现性**：7-Zip 与 bsdtar 都会写入文件 mtime，
   同一批文件在不同时刻打包会得到不同 zip 的 md5。tag 里因此**不写 zip 指纹**（沿用 1.1.0 的设计），
   但 `gh release` 附件的可复现性事实上不存在——这一点**只能靠 tag + Release 页面留证**，
   没有更硬的方案（`dist/` 不入库是既定决定）。
5. **零 DLL 依赖的边界**：23 条导入里 12 条是 `api-ms-win-crt-*`，属 Windows 10/11 的 UCRT。
   在 **Windows 7 / 未装 UCRT 的 Server 2012** 上能否启动，本轮没有环境验证。
   `RELEASE.txt` 的「目标平台」只写 `Windows x64`，可能让人误以为覆盖 Win7。
   建议在已知问题里补一句「需要 Windows 10 及以上（UCRT）」，但这是**推断**，需 Lead 拍板。
6. **`-municode` + `-mwindows` 的组合**：GUI 产物同样带 `-municode`，入口是 `wWinMain` 还是
   `wmain`＋子系统切换，只在 `gui.cpp` 内部 `#ifdef SVPN_GUI_SUBSYSTEM` 分支里成立。
   本轮没读透这条分支的全部分支路径（不属 W6 文件），**若哪天有人去掉 `-municode`，
   GUI 会以链接错误而非运行错误暴露**——这一点在打包文档里只写了「不加直接链接失败」，
   没有区分两个产物。
