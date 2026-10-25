# G1 git 卫生：.gitattributes 落行尾契约 + .gitignore 收口

## 1. 摘要

新建 `.gitattributes`，把「源码恒 LF、`.cmd` 恒 CRLF、二进制不转换」写成仓库级契约，clone 后源码字节可复现。
`.gitignore` 修掉 `dist/` 与 `/svpn.exe` 两处矛盾注释，补 `/svpn-gui.exe`、`tools/*.bmp`。
`svpn-gui.exe` 与 `tools/gui1.bmp` 已从未跟踪列表消失，`tools/shot.py` 仍可入库，六个不该被 ignore 的路径全部不命中。
`git add --renormalize --dry-run` 列出全部 37 个已跟踪文件（git 对 renormalize 的固定行为），但按内容算，纯行尾原因导致的改动文件数 = **0**；索引本来就全是 `i/lf`。
一条硬数据要 Lead 决策：`dist/RELEASE.txt` 里 1.1.0 的源码 MD5 是在 **CRLF 工作区**上算的（见 §5 第 1 条），1.1.1 若改按 LF 算，14 个指纹会全部变号。

## 2. 做了什么

### 2.1 新建 `.gitattributes`（仓库根，655 B，UTF-8 无 BOM，LF）

| 行 | 规则 | 语义 |
|---|---|---|
| 4 | `* text=auto eol=lf` | 默认按文本处理，仓库内与 checkout 到工作区一律 LF。`core.autocrlf=true` 时 checkout 也不再转 CRLF，源码字节指纹才稳定 |
| 7 | `*.cmd text eol=crlf` | `cmd.exe` 要求 CRLF，覆盖上面的默认 |
| 8 | `*.bat text eol=crlf` | 同上 |
| 11 | `*.md text eol=lf` | 显式声明为文本（不被 auto 误判成二进制），跟随默认 LF |
| 14-19 | `*.exe` `*.bmp` `*.zip` `*.o` `*.lib` `*.dll` → `binary` | `binary` 是 `-text -diff` 的宏：不做行尾转换、不参与 diff/合并 |

`* text=auto` 前置的作用：将来新增的未列出二进制（字体、图标等）仍由 git 内容探测兜底；已列出的六类用 `-text` 钉死。

### 2.2 修 `.gitignore`（977 B，UTF-8 无 BOM，LF）

- 文件头（第 1-2 行）新增统一口径：**二进制产物一律不入库（双产物 svpn.exe / svpn-gui.exe），发布物统一放 `dist/` 打包，追溯靠 git tag + `dist/RELEASE.txt` 的源码指纹**。
- 删掉原第 7 行矛盾注释「唯一发布物在 dist/，命名带哈希且设只读」——它与末尾「二进制不进源码树」自相矛盾，现在只有一处口径。
- 原 `/svpn.exe` 段改为「根目录构建输出（源码树里不保留二进制副本）」，并补 `/svpn-gui.exe`（第 11-12 行）。
- 新增第 32-33 行「工具目录里的截图素材（体积大、可随时重拍；shot.py 等脚本要入库）」+ `tools/*.bmp`。
- 末尾 `dist/` 段注释改为「整个目录忽略……靠 tag + RELEASE.txt 源码指纹追溯」，与文件头一致。
- 保留未动：`*.o`、`build/`、`build_*.exe`、`test_*.exe`、`runtime/` 及其四类备份/pid/log、`config.json`、`tests/*.exe`、`_crew/tmp_*/`、`_crew/**/runtime/`。分组与组间空行保持原样，注释全中文、一行一条。

### 2.3 未做的事（按 SPEC 与分工）

没有 `git add`、没有 `git commit`、没有真跑 renormalize、没有碰 `0c1c602`/`9e6bfb4` 历史、没有碰 `src/` `tests/` `docs/` `README*` `build.cmd` `dist/`。

## 3. 验证

### 3.1 属性生效（任务 1）

```
$ git check-attr -a -- src/main.cpp src/gui.cpp build.cmd run.cmd tools/shot.py
src/main.cpp: text: auto
src/main.cpp: eol: lf
src/gui.cpp: text: auto
src/gui.cpp: eol: lf
build.cmd: text: set
build.cmd: eol: crlf
run.cmd: text: set
run.cmd: eol: crlf
tools/shot.py: text: auto
tools/shot.py: eol: lf
```

二进制：

```
$ git check-attr -a -- svpn.exe tools/gui1.bmp dist/svpn-1.1.0-win64.zip build_main.o
svpn.exe: binary: set
svpn.exe: diff: unset
svpn.exe: merge: unset
svpn.exe: text: unset
svpn.exe: eol: lf
tools/gui1.bmp: binary: set
tools/gui1.bmp: diff: unset
tools/gui1.bmp: merge: unset
tools/gui1.bmp: text: unset
tools/gui1.bmp: eol: lf
dist/svpn-1.1.0-win64.zip: binary: set
dist/svpn-1.1.0-win64.zip: diff: unset
dist/svpn-1.1.0-win64.zip: merge: unset
dist/svpn-1.1.0-win64.zip: text: unset
dist/svpn-1.1.0-win64.zip: eol: lf
build_main.o: binary: set
build_main.o: diff: unset
build_main.o: merge: unset
build_main.o: text: unset
build_main.o: eol: lf
```

> 注：二进制文件上也显示 `eol: lf`，来自 `* text=auto eol=lf` 的默认值；`text` 为 `unset` 时 git 忽略 `eol`，实际不做任何转换。

`.md`：

```
$ git check-attr -a -- README.md _crew/reports/G1-git-hygiene.md
README.md: text: set
README.md: eol: lf
_crew/reports/G1-git-hygiene.md: text: set
_crew/reports/G1-git-hygiene.md: eol: lf
```

### 3.2 renormalize 干跑（只 dry-run）

```
$ git add --renormalize --dry-run . | wc -l
37
$ git ls-files | wc -l
37
```

37 = 已跟踪文件总数，即 renormalize 会把每个已跟踪文件都重写一次索引并逐个报告——这是 `--renormalize` 的固定行为，**不代表 37 个文件内容会变**。按内容核对：

```
$ git ls-files --eol | awk '{print $1, $2}' | sort | uniq -c
      7 i/lf w/crlf
     30 i/lf w/lf
```

索引里 37 个文件**全部**是 `i/lf`（已是 LF 归一态）。工作区仍是 CRLF 的 7 个文件，逐个比较「加属性后归一化得到的 blob」与「索引里的 blob」：

```
$ for f in ...; do git hash-object --path="$f" "$f"; git rev-parse ":$f"; done
SAME  src/cdn.cpp                      work=596420b8287033149de56486dd34d3cf0bd9b9bb  index=596420b8287033149de56486dd34d3cf0bd9b9bb
SAME  src/config.cpp                   work=da7fcf5dac33f921083dc0cce0cd8fb2ba9e1a39  index=da7fcf5dac33f921083dc0cce0cd8fb2ba9e1a39
DIFF  src/main.cpp                     work=a277f52532b1201d265d93a2e409ee9981c0ce2b  index=3a56ac34d02083655af342d67b23cd46bfc120e3
DIFF  src/svpn.hpp                     work=fde788861c0d4b90d99708574f95e9fd39cda0fc  index=ef8a7da7c865fbf782b45a9c89a53b6325c91d78
SAME  src/tls.cpp                      work=ad9d1140c2ba0f163f4edead144a1eb34539aa57  index=ad9d1140c2ba0f163f4edead144a1eb34539aa57
SAME  run.cmd                          work=1804c70db835ae6338af601b0eb68009d1b07223  index=1804c70db835ae6338af601b0eb68009d1b07223
SAME  _crew/reports/V1-cli-audit.md    work=a8622374ad45dc545619e8c26bd25616138658b6  index=a8622374ad45dc545619e8c26bd25616138658b6
```

结论：**纯行尾原因预计改动文件数 = 0**。两处 DIFF 是 P4 正在改的真实内容（`src/main.cpp`、`src/svpn.hpp`），与本任务无关。

### 3.3 `.cmd` 工作区行尾（任务 1 附带要求）

Python 读字节统计（`open(p,'rb').read()`，`.count(b'\r\n')` / `.count(b'\n')`）：

```
build.cmd:    bytes=1171  CRLF=0   LF-only=40
run.cmd:      bytes=823   CRLF=33  LF-only=0
dist/run.cmd: bytes=250   CRLF=13  LF-only=0
```

`build.cmd` 工作区已是 LF（40 行全 LF，0 个 CRLF）。按指示**未改文件**（且它是 Lead 的独占文件）。`build.cmd` 为纯 ASCII、无 `goto`、无 `chcp`，LF-only 触发 `cmd.exe` 解析错乱的概率低，但不符合 SPEC「`.cmd` 用 CRLF」，列入 §5。

### 3.4 ignore 效果（任务 2/3）

`git status --short`：`?? svpn-gui.exe` 已消失。

```
$ git status --short
 M .gitignore
 M README.md
 M _crew/reports/T1-tests.md
 M build.cmd
 M docs/BUILD.md
 M src/main.cpp
 M src/svpn.hpp
 M src/util.cpp
?? .gitattributes
?? README.en.md
?? _crew/SPEC-1.1.1-release.md
?? _crew/reports/D2-architecture.md
?? _crew/reports/D3-windows.md
?? _crew/reports/D4-readme-en.md
?? _crew/reports/G1-git-hygiene.md
?? _crew/reports/P4-upstream-kind.md
?? _crew/reports/R4-proc-docs.md
?? _crew/reports/V2-branch-coverage.md
?? _crew/reports/log-analysis.md
?? _crew/reports/packaging.md
?? docs/ARCHITECTURE.md
?? docs/WINDOWS.md
?? src/gui.cpp
?? tools/
```

（`?? tools/` 是因为目录里只剩未跟踪的 `shot.py`；`gui1.bmp` 已被忽略，见本条末尾 `--ignored` 输出。）

应命中的，每个都有出处：

```
$ ls dist/
LICENSE  README.md  RELEASE.txt  SHA256SUMS.txt  config.example.json  run.cmd  svpn-1.1.0-win64.zip  svpn.exe

$ git check-ignore -v svpn-gui.exe tools/gui1.bmp svpn.exe dist/svpn-1.1.0-win64.zip dist/svpn.exe dist/RELEASE.txt
.gitignore:12:/svpn-gui.exe	svpn-gui.exe
.gitignore:33:tools/*.bmp	tools/gui1.bmp
.gitignore:11:/svpn.exe	svpn.exe
.gitignore:40:dist/	dist/svpn-1.1.0-win64.zip
.gitignore:40:dist/	dist/svpn.exe
.gitignore:40:dist/	dist/RELEASE.txt
```

必须不命中的，全部无输出、退出码 1：

```
$ git check-ignore -v docs/BUILD.md docs/ARCHITECTURE.md docs/WINDOWS.md README.md README.en.md _crew/reports/T1-tests.md _crew/SPEC-1.1.1-release.md tools/shot.py
(无输出)
$ echo $?
1
```

`tools/shot.py` 会被纳入：

```
$ git add -n tools/shot.py
add 'tools/shot.py'

$ git status --short --ignored tools/
?? tools/
!! tools/gui1.bmp
```

旧规则一条没丢：

```
$ git check-ignore -v build_main.o build_util.o build_test_a6.exe test_net.exe tests/test_config.exe runtime/svpn.log config.json _crew/tmp_foo/x
.gitignore:5:*.o	build_main.o
.gitignore:5:*.o	build_util.o
.gitignore:7:build_*.exe	build_test_a6.exe
.gitignore:8:test_*.exe	test_net.exe
.gitignore:26:tests/*.exe	tests/test_config.exe
.gitignore:15:runtime/	runtime/svpn.log
.gitignore:23:config.json	config.json
.gitignore:36:_crew/tmp_*/	_crew/tmp_foo/x
```

```
$ git status --short --ignored | grep '^!!'
!! _crew/tmp_a6/
!! build/
!! build_cdn.o
!! build_config.o
!! build_json.o
!! build_main.o
!! build_net.o
!! build_pac.o
!! build_proxy.o
!! build_rd.o
!! build_rules.o
!! build_sni.o
!! build_test_a6.exe
!! build_tls.o
!! build_util.o
!! build_winsys.o
!! config.json
!! dist/
!! runtime/
!! svpn-gui.exe
!! svpn.exe
!! test_net.exe
!! tests/_tlstest.cpp
!! tests/err.txt
!! tests/out.txt
!! tests/out_default.json
!! tests/t.exe
!! tests/test_config.exe
!! tests/test_sni.exe
!! tests/test_sni_parse.exe
!! tests/test_sni_real.exe
!! tools/gui1.bmp
```

### 3.5 两个新改文件自身的字节检查

```
.gitattributes: bytes 655  BOM False  CRLF 0
.gitignore:     bytes 977  BOM False  CRLF 0
```

## 4. 契约变更

影响 Lead 与 W6（打包/流程报告），W1/W3/W4/W5 无感：

1. **新增仓库级行尾契约。** 从此源码在仓库里与 checkout 后都是 LF。任何「按工作区当前字节算 MD5 / 报二进制大小」的流程必须改为按 LF 字节算——直接影响 `dist/RELEASE.txt` 的源码指纹表（详见 §5 第 1 条）。
2. **`tools/` 首次入库，但只入 `shot.py`。** 若其他报告引用截图，需说明 `tools/gui1.bmp` 不入库、需自行重拍。
3. **`dist/` 口径统一后**，`dist/` 下任何文件（含 `RELEASE.txt`、`SHA256SUMS.txt`）都不进仓库，只能通过 Release 附件分发。若有报告写「RELEASE.txt 已入库」，需改口径。
4. 对 W1/P4 无影响：`src/` 属性只是 `text=auto eol=lf`，未改任何源码字节（`src/main.cpp`、`src/svpn.hpp` 的 DIFF 来自 P4 自己的编辑）。

## 5. 没做完/风险

1. **【需 Lead 决策，重要】`dist/RELEASE.txt` 里 1.1.0 的源码 MD5 是 CRLF 字节算出来的。** 证据：`src/cdn.cpp` 工作区仍是 CRLF，其原样 MD5 = `eabe35a7df866cfa16f6539d8fb4bb7c`，与 `dist/RELEASE.txt` 记录的完全一致；归一到 LF 后是 `910a3b9d1fb75b04c3ca2b5c1b793fba`（30,797 B → 30,054 B）。当前 14 个指纹源码里有 5 个工作区是 CRLF（`src/main.cpp`、`src/svpn.hpp`、`src/config.cpp`、`src/cdn.cpp`、`src/tls.cpp`），9 个是 LF——**现在从工作区直接算指纹会得到一张混杂的表**。建议 1.1.1 统一按 LF 算，取数用 `git show :src/xxx.cpp`（索引 blob 恒为 LF）最稳，并在 `RELEASE.txt` 补一行「源码指纹按 LF 行尾计算」。此事超出我的文件范围，我没有改 `dist/`。
2. **`build.cmd` 工作区是 LF**（40 行 / 0 CRLF），不符合「`.cmd` 用 CRLF」。按指示未改（它属 Lead）。`.gitattributes` 的 `*.cmd text eol=crlf` 会在**下次 checkout 该文件时**（fresh clone、或 `git checkout -f build.cmd`）把它写成 CRLF。缓解：纯 ASCII、无 `goto`、无 `chcp`，LF-only 实际触发解析错误的概率低；建议 Lead 提交后在干净工作区跑一次 `build.cmd` 验证。
3. **`_crew/reports/V1-cli-audit.md` 工作区是 CRLF**（`i/lf w/crlf`），属性是 `text eol=lf`。下次 checkout 会被改写成 LF，属预期；它在 git 眼里会「看起来变了」而实际只是行尾，别误判成有人改了内容。
4. **历史里的 1.6 MB dist blob 没有清理。** `0c1c602` / `9e6bfb4` 的 `dist/` 二进制仍在仓库历史中，本次只做到「今后不再入库」，历史体积不变。**按 SPEC §8 第 1 条，本轮明确不处理，不做 rebase / filter-branch / BFG**；后续若要做须由 Lead 单独开一轮（涉及全量提交哈希重写 + 强推 + 所有人重新 clone），不属 1.1.1 范围。
5. **`dist/svpn.exe` 只读位实测已具备，与任务描述不一致。** 实测 `dist/svpn.exe` mode=555（只读）、`svpn.exe` 与 `svpn-gui.exe` 均 mode=777（可写）。即当前 dist 内 1.1.0 产物本来就是只读的；**Lead 重建 1.1.1 的 `dist/svpn.exe` 与新增 `dist/svpn-gui.exe` 后需重新 `attrib +R`**，否则 `RELEASE.txt` 里「建议保留只读属性」落空。归 Lead，我未动。
6. **未实测「真跑 renormalize」。** 按指示只做 dry-run。「纯行尾改动 0」是基于 37 个文件的 `i/lf` 状态 + 逐文件 blob 比对推出来的，真跑一次不会有 diff；这是推理而非实测，如实标注。
7. **`.gitattributes` 自身尚未入库。** 它现在是未跟踪文件。`git check-attr` 已能读到它（读的是工作区文件，与是否跟踪无关），但契约要真正对所有人生效，仍需 Lead 提交。
