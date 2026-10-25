# R3 打包报告 — svpn 1.1.0-cpp

状态：**产物已交付，git 落库由 Lead 收尾完成**

> 本文按仓库**实际状态**记录，包含 `dist/` 被误入库、以及后续由 Lead 补提交移出的全过程，未作删改。
> 边界：打包与校验由 release-bot 执行；`git init` 后的提交与 tag 由 Lead 接手（见 §6）。
> 本报告本身**未提交**，为避免在 V1 最终门禁期间改动仓库（见 §9）。

---

## 1. 发布产物

`e634fe15`（Lead 裁决 A，源码冻结）。

```
D:\LE\AI\wbwrk\svpn-cpp\dist\
```

| 文件 | 大小 | SHA256 |
|---|---|---|
| `svpn-e634fe15.exe` | 1,678,848 B | `45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b` |
| `README.md` | 22,219 B | `e1657da9ad034e70fc4d73a673af94ae1a85a4d699a945a9e31effe497425fa6` |
| `LICENSE` | 1,063 B | `0e14238e7ca576da271d122cfa4af4e6e6be05c26f6e51ff5aec44cb529f6206` |
| `run.cmd` | 398 B | `cc290bf9b64dde410df4748f1f9502398e2f575e42478c9709089c8404c28d95` |
| `config.example.json` | 2,084 B | `2760af59dc4d49c9f499da434755869e4cd75ebb4cc2f0f2d103b7359274fcc2` |
| `RELEASE.txt` | 3,397 B | `4497e7597018ad9f71afda1e3f1ddaad8e2c86610d5abdc6cd9671b8d4a5a9df` |
| `SHA256SUMS.txt` | 394 B | `8352d644bbfdd19d384b5fb340e8be08ac4c6519a340ab79ce8aeacc9f3ea608` |
| `svpn-1.1.0-win64.zip` | 699,886 B | md5 `558367d76d37d280de374525c2004694` |

`sha256sum -c SHA256SUMS.txt` → **5/5 OK**（exe / README / LICENSE / run.cmd / config.example.json）。

### 不可变副本

```
svpn.exe  →  dist/svpn-e634fe15.exe     : cp 后复核 md5，再 attrib +R 设只读
```

```
$ md5sum dist/svpn-e634fe15.exe
e634fe15067808ccf2ba0dad30d55d97

$ attrib dist\svpn-e634fe15.exe
A    R               D:\LE\AI\wbwrk\svpn-cpp\dist\svpn-e634fe15.exe
                     ↑ R = 只读

设只读后二次复核 → e634fe15067808ccf2ba0dad30d55d97（未变）
$ ./dist/svpn-e634fe15.exe -v → 1.1.0   rc=0
```

打包全程未再跑 `build.cmd`、未 strip、未加版本资源、未压缩 exe，故 md5 与 V1 签字一致。

### 压缩包

```
$ unzip -t svpn-1.1.0-win64.zip
No errors detected …

$ unzip -l svpn-1.1.0-win64.zip
  1678848  svpn-1.1.0-win64/svpn-e634fe15.exe
      398  svpn-1.1.0-win64/run.cmd
    22219  svpn-1.1.0-win64/README.md
     1063  svpn-1.1.0-win64/LICENSE
     2084  svpn-1.1.0-win64/config.example.json
     3397  svpn-1.1.0-win64/RELEASE.txt
      394  svpn-1.1.0-win64/SHA256SUMS.txt
                                  7 files

$ unzip -p … svpn-e634fe15.exe | md5sum
e634fe15067808ccf2ba0dad30d55d97        ← 包内 exe 与发布 md5 一致
$ unzip -p … | wc -c
1678848
```

包内 `run.cmd` 的 sha256 与磁盘上的一致（`cc290bf9…`）。

### RELEASE.txt

写死：版本 `1.1.0-cpp`、md5 `e634fe15067808ccf2ba0dad30d55d97`、大小 1,678,848 B、
构建时间 `2026-10-25 10:46:27`、编译器 `g++ 16.2.0 (x86_64-posix-seh-rev1,
Built by MinGW-Builds project)`、14 个 `src/` 文件 md5 清单，以及
「本 exe 由上述源码用 `build.cmd` 构建」。

编译器经实机核对：

```
$ D:/LE/AI/tools/mingw64/bin/g++.exe --version | head -1
g++.exe (x86_64-posix-seh-rev1, Built by MinGW-Builds project) 16.2.0

$ ... -dumpmachine
x86_64-w64-mingw32
```

### 排除项

```
config.json / runtime/ / *.o / tests/*.exe / tests/_* / hosts.backup /
wininet-backup.json / effective.json / *.pid / *.log
```

扫描 `dist/` → **零命中**。

---

## 2. run.cmd 的命名问题（已解决）

Lead 指定产物名为 `svpn-e634fe15.exe`，而原 `run.cmd` 硬编码调用 `svpn.exe`，
双击会找不到程序。

**当前 `run.cmd` 已改为自动探测**（823 B → 398 B）：

```bat
set "SVPN=svpn.exe"
if not exist "%SVPN%" set "SVPN=svpn-e634fe15.exe"
if not exist "%SVPN%" (
    echo [!] svpn.exe not found in this folder.
    pause
    exit /b 1
)
```

两种命名都能直接启动，该问题关闭。`dist/SHA256SUMS.txt` 已同步新哈希并通过校验。

---

## 3. git 落库

### 3.1 `git show --stat HEAD` 原文

```
commit 64ab5c364450c4b02b80f1f54359bcbe2b99c931
Author: zhe-du <zhe-du@users.noreply.github.com>
Date:   Sun Oct 25 10:57:58 2026 +0800

    chore: 真正把 dist/ 移出版本索引
    
    上一条 3ed3999 只改了 .gitignore，但 dist/ 在 9e6bfb4 里已经入库，
    gitignore 对已跟踪文件无效 —— 必须 git rm --cached。
    现在 git ls-files 不含任何 dist/ 路径。

 dist/LICENSE             |  21 ----
 dist/README.md           | 280 -----------------------------------------------
 dist/RELEASE.txt         |  87 ---------------
 dist/SHA256SUMS.txt      |   5 -
 dist/config.example.json |  42 -------
 dist/run.cmd             |  33 ------
 dist/svpn-e634fe15.exe   | Bin 1678848 -> 0 bytes
 7 files changed, 468 deletions(-)
```

### 3.2 tag

```
$ git tag -n1
v1.1.0          svpn 1.1.0 (Windows x64 · 单 exe)

$ git rev-parse v1.1.0^{commit}
6bfe673b8243c1b857d1ba13ee5222d65096d80e
```

annotated tag，message 含 exe md5 / zip md5 / 编译器 / 源码指纹。

### 3.3 `.gitignore` 全文

```
# 编译中间产物
*.o
build/
build_*.exe
test_*.exe

# 根目录构建输出（唯一发布物在 dist/，命名带哈希且设只读）
/svpn.exe

# 运行期数据（日志 / pid / 缓存 / 备份，含本机信息）
runtime/
**/hosts.backup
**/wininet-backup.json
**/effective.json
**/*.pid
**/*.log

# 本机配置（含本机端口/模式改动，属运行时状态）
config.json

# 测试可执行文件与临时脚手架
tests/*.exe
tests/_*
tests/out*.txt
tests/out*.json
tests/err.txt

# 团队协作临时产物（含本机 hosts 备份等）
_crew/tmp_*/
_crew/**/runtime/

# 发布产物入库：exe 命名带哈希并设只读，与源码同仓才可追溯
# 注意 dist/ 下只允许发布文件，运行时数据仍由上面的规则排除

# 发布产物：二进制不进源码树（靠 MANIFEST + tag 追溯）
dist/
```

### 3.4 跟踪文件数

```
$ git ls-files | wc -l
37
```

`git ls-files dist/ | wc -l` → **0**，`dist/` 已完全退出索引。

---

## 4. `dist/` 误入库事件始末（按要求保留）

按时间顺序，全部为本地库、无远程、未推送：

| 时间 | commit | 动作 | 结果 |
|---|---|---|---|
| 10:54:18 | `0c1c602` | release-bot 首提，含源码 + `dist/` 7 个文件 | **`dist/` 进入了版本树** |
| 10:54:49 | `b14ec43` | Lead：`.gitignore` 加 `dist/` | 仅改忽略规则，**对已跟踪文件无效** |
| 10:54:59 | `6bfe673` | Lead/T1：test_cli 89→107（tag `v1.1.0` 打在此处） | — |
| 10:56:52 | `9e6bfb4` | release-bot 据 Lead「我倾向入库」恢复 `dist/` 并打 `v1.1.0-cpp` | **`dist/` 重新入库**，与 Lead 决定冲突 |
| 10:57:34 | `3ed3999` | Lead：再次移出 + 收录本报告 | 仅改 `.gitignore`，**仍无效** |
| 10:57:58 | `64ab5c3` | Lead：`git rm -r --cached dist/` | **真正移出**，`ls-files` 不含 dist |

**根因两点**：
1. release-bot 在 Lead 作出「`dist/` 不入库」的决定前提交了 `dist/`，之后又依据 Lead 早前
   「我倾向入库」的一句话把移除操作反转了——同一决策期内收到两条指向相反的指示，
   而两条指示的时序没有对齐。
2. `.gitignore` 对**已跟踪**文件不生效。`b14ec43` 与 `3ed3999` 都只加了忽略规则，
   必须配 `git rm -r --cached` 才真正退出索引，`64ab5c3` 才修对。

`dist/svpn-e634fe15.exe` 的 1.6MB blob 仍留在 `0c1c602` 与 `9e6bfb4` 两个 commit 中。
本地库、无远程，如需彻底清除须改写历史（尚未执行）。

**当前状态符合 Lead 的最终决定**：`dist/` 不入 git，可追溯性由 `dist/RELEASE.txt`
（含 14 个源文件 md5）+ annotated tag `v1.1.0` 保证。

---

## 5. 如何验证这份产物

```bash
md5sum dist/svpn-e634fe15.exe          # 期望 e634fe15067808ccf2ba0dad30d55d97
./dist/svpn-e634fe15.exe -v            # 期望 1.1.0
python tests/test_cli.py               # 期望全部 PASS、FAIL 0
```

三条实测输出：

```
e634fe15067808ccf2ba0dad30d55d97  dist/svpn-e634fe15.exe
1.1.0
PASS 113 / FAIL 0 / SKIP 0（其中硬失败 0、契约待修 0）
```

注：`test_cli` 的用例数在发版窗口内由 89 → 107 → 113 持续变化（T1 增补），
113 为 release-bot 于 10:56 实测值。

---

## 6. 职责边界

- **release-bot**：`dist/` 组装、不可变副本、`SHA256SUMS.txt`、`RELEASE.txt`、
  包内 exe 校验、`git init`、首次提交、`v1.1.0-cpp` 标签（后由 Lead 清理）。
- **Lead**：终止上述 git 操作，以 `b14ec43` / `3ed3999` / `64ab5c3` 修正索引，
  打 `v1.1.0`，并将 `run.cmd` 改为自动探测命名。

本报告由 release-bot 按实际状态撰写，未对上述过程作美化。

---

## 7. 遗留与偏差

### 7.1 tag `v1.1.0` 里的 zip md5 已过期

tag message 与 Lead 口头引用均为：

```
zip md5 8884716eee3f8735504d89195b1ff312   700,379 B
```

那是 release-bot 早期生成的包（已删除）。`run.cmd` 换成自动探测版后包被重新生成，
**当前实际值是**：

```
zip md5 558367d76d37d280de374525c2004694   699,886 B
```

exe 的 md5（`e634fe15…`）未变，只有包级哈希变了。
若 tag message 需与发布页一致，须重写 tag；本报告不代为修改。

### 7.2 `.gitignore` 残留一段自相矛盾的注释

文件末尾同时存在：

```
# 发布产物入库：exe 命名带哈希并设只读，与源码同仓才可追溯
# 注意 dist/ 下只允许发布文件，运行时数据仍由上面的规则排除
...
# 发布产物：二进制不进源码树（靠 MANIFEST + tag 追溯）
dist/
```

第二段是最终决定，第一段是 release-bot 反转期间写入的、已失效的说明。
**建议删除第一段**，否则后来者会误读为「dist 应当入库」。未改，避免发版节点动仓库。

### 7.3 `core.autocrlf=true`

系统默认 `true`，全新检出会把文本文件转为 CRLF，使 `RELEASE.txt` 记录的源码 md5
不再成立。当前本机工作区为 LF，指纹有效。
建议后续加 `.gitattributes`（`* text=auto eol=lf`）固定；发版节点未加。

### 7.4 本报告未提交

`_crew/reports/R3-package.md` 的上一版随 `3ed3999` 入库，本版为重写，
当前处于未提交状态（`git status` 可见）。按 Lead「别再碰 git、只写报告」的要求，
release-bot 未提交，交由 Lead 决定何时收进历史。

---

## 8. 当前仓库状态

```
$ git log --oneline --decorate -5
64ab5c3 (HEAD -> main) chore: 真正把 dist/ 移出版本索引
3ed3999                chore: dist/ 再次移出版本控制，并收录 R3 打包报告
9e6bfb4                chore: dist/ 纳入版本控制，发布物与源码同仓
6bfe673 (tag: v1.1.0)  test: 补 ④⑧⑨⑩ 契约断言，test_cli 从 89 增至 107 项
b14ec43                chore: dist/ 不纳入版本控制

$ git status --short
 M README.md
 M _crew/reports/V1-cli-audit.md
```

- `git ls-files` = 37，`dist/` 不在其中
- 上述两处改动属 D1-docs / V1-verify 的并行编辑，与本发布无关，release-bot 未触碰
- 仅本地仓库，无 remote，未推送
