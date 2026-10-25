# R3 打包报告 — svpn 1.1.0-cpp

状态：**产物已交付并落库；本文为终态快照**

> **快照时间：2026-10-25 11:00 (+0800)，HEAD = `9ff8fdb`**
>
> 发版窗口内 `dist/` 与提交被多方并行改动（release-bot / Lead / T1 / D1 / V1）。
> 本文以快照时刻的实际状态为准，并保留全部变更过程，未作美化。
> 文中明确区分「release-bot 所做」与「他人后续改动」。

---

## 1. 终态产物

主程序 `e634fe15`（Lead 裁决 A，源码冻结）。

```
D:\LE\AI\wbwrk\svpn-cpp\dist\
```

| 文件 | 大小 | SHA256 |
|---|---|---|
| `svpn.exe` | 1,678,848 B | `45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b` |
| `README.md` | 22,219 B | `e1657da9ad034e70fc4d73a673af94ae1a85a4d699a945a9e31effe497425fa6` |
| `LICENSE` | 1,063 B | `0e14238e7ca576da271d122cfa4af4e6e6be05c26f6e51ff5aec44cb529f6206` |
| `run.cmd` | 250 B | `eb8d16b5501f9ffefda0a8708f92879ee631981d51d926c40ff78d8e56e8f82e` |
| `config.example.json` | 2,084 B | `2760af59dc4d49c9f499da434755869e4cd75ebb4cc2f0f2d103b7359274fcc2` |
| `RELEASE.txt` | 3,343 B | `8deafff4041df7e2d8718458aafd63990eb2c7208a67c964f04946f7b224ef92` |
| `SHA256SUMS.txt` | 385 B | `97e5fe5e79b44e494040a899b9015526b4d847c6c87994ee6b03a9b76edb766e` |
| `svpn-1.1.0-win64.zip` | 699,771 B | md5 `2146fb16b50b7abc5711917c197904c3` |

```
$ sha256sum -c SHA256SUMS.txt
svpn.exe: OK
run.cmd: OK
README.md: OK
LICENSE: OK
config.example.json: OK
```

`SHA256SUMS.txt` 覆盖 5 个文件（exe / run.cmd / README / LICENSE / config.example.json），
不含 `RELEASE.txt` 与自身；`RELEASE.txt` 的 sha256 见上表。

### 主程序

```
$ md5sum dist/svpn.exe
e634fe15067808ccf2ba0dad30d55d97

$ stat → 1,678,848 B   mtime 2026-10-25 10:52
$ ./dist/svpn.exe -v → 1.1.0   rc=0
```

与 V1-verify 签字值、与 tag `v1.1.0` 中的 exe 锚点三者一致。

### 压缩包

```
$ md5sum svpn-1.1.0-win64.zip
2146fb16b50b7abc5711917c197904c3        699,771 B

$ unzip -t → No errors detected

$ unzip -l
  1678848  svpn-1.1.0-win64/svpn.exe
      250  svpn-1.1.0-win64/run.cmd
    22219  svpn-1.1.0-win64/README.md
     1063  svpn-1.1.0-win64/LICENSE
     2084  svpn-1.1.0-win64/config.example.json
     3343  svpn-1.1.0-win64/RELEASE.txt
      385  svpn-1.1.0-win64/SHA256SUMS.txt

$ unzip -p … svpn.exe | md5sum
e634fe15067808ccf2ba0dad30d55d97        ← 包内与发布 md5 一致
```

### 排除项

```
config.json / runtime/ / *.o / tests/*.exe / tests/_* / hosts.backup /
wininet-backup.json / effective.json / *.pid / *.log
```

扫描 `dist/` → **零命中**。

另注：`_crew/tmp_a6/py_runtime/hosts.backup`（含本机 hosts 全文）在首次
`git add -A` 时曾被暂存，已由 `.gitignore` 的 `**/hosts.backup` 与 `_crew/tmp_*/` 挡下，
**从未进入任何提交**。

---

## 2. 产物形态变更记录

release-bot 交付的是**带哈希名 + 只读**副本；Lead 后续改为**通用名**以匹配 `run.cmd`，
并两次重打压缩包。

| 时间 | 执行方 | 变化 | 产物标识 |
|---|---|---|---|
| 10:52 | release-bot | `cp svpn.exe → dist/svpn-e634fe15.exe`，`attrib +R` | exe `e634fe15…`，只读 |
| 10:53 | release-bot | 建 `dist/` 7 文件、`SHA256SUMS.txt`、`RELEASE.txt` | zip `8884716e…` / 700,379 B（已删除） |
| 10:56 | release-bot | `RELEASE.txt` 用例数改 113 | — |
| 10:58 | Lead | `run.cmd` 换自动探测版（823→398 B），重打 zip | zip `558367d7…` / 699,886 B |
| 10:58 | Lead | 主程序改名 `svpn.exe`；`run.cmd` 简化直调（250 B） | zip `b5c22e7d…` / 699,771 B |
| 10:59 | Lead | 同步 `dist/README.md`，重打 zip | zip `2146fb16…` / 699,771 B（终态） |

exe 的字节**自始至终未变**（`e634fe15…` / 1,678,848 B），变的只有包级哈希。

---

## 3. git 落库

### 3.1 `git show --stat HEAD` 原文

```
commit 9ff8fdbd4d8b49b1714bd94d1594b362245a417c
Author: zhe-du <zhe-du@users.noreply.github.com>
Date:   Sun Oct 25 10:59:35 2026 +0800

    docs: 去掉过期的改名提示，收录 R3 与 V1 发布物门禁
    
    README 曾提示『发布包里的 exe 叫 svpn-<hash>.exe，先改名再双击』，
    但打包已统一为 svpn.exe，该句会误导使用者，删除。
    同步 dist/README.md、SHA256SUMS.txt 并重打 zip。
    
    另收录：
    - _crew/reports/R3-package.md  打包与 git 落库记录
    - _crew/reports/V1-cli-audit.md §10 发布物门禁（对副本实测）

 _crew/reports/R3-package.md   | 320 +++++++++++++++++++++++++++++-------------
 _crew/reports/V1-cli-audit.md |  48 ++++---
 tests/test_cli.py             |  42 ++++--
 3 files changed, 283 insertions(+), 127 deletions(-)
```

### 3.2 tag

```
$ git tag -n1
v1.1.0          svpn 1.1.0 (Windows x64 · 单 exe)

$ git rev-parse v1.1.0^{commit}
9ff8fdbd4d8b49b1714bd94d1594b362245a417c
```

annotated tag，message 只钉不变项，包指纹明确交给 `dist/SHA256SUMS.txt`：

```
【产物锚点】只钉不变的东西，包指纹见 dist/SHA256SUMS.txt
  exe md5    e634fe15067808ccf2ba0dad30d55d97
  exe sha256 45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b
  exe 大小   1,678,848 B
  源码指纹   src/main.cpp = ef09fc1c11844a04a76aae86b94c871b (49,232 B)

【构建】g++ 16.2.0 (MinGW-w64 x86_64-posix-seh-rev1), -O2 -static -s
【审计】_crew/reports/V1-cli-audit.md ①–⑮ 全闭（⑩ 为已接受取舍）
【用例】test_cli 113 / test_config 178 / test_sni_parse 27 全绿
```

该设计规避了 release-bot 早期指出的问题（tag 里写死 zip md5 会随重打包过期）。

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

$ git ls-files dist/ | wc -l
0
```

`dist/` 完全退出索引，符合 Lead 决定。

---

## 4. `dist/` 误入库事件始末（保留）

全部为本地库、无远程、未推送。

| 时间 | commit | 执行方 | 动作 | 结果 |
|---|---|---|---|---|
| 10:54:18 | `0c1c602` | release-bot | 首提，含源码 + `dist/` 7 文件 | **`dist/` 进入版本树** |
| 10:54:49 | `b14ec43` | Lead | `.gitignore` 加 `dist/` | 对已跟踪文件**无效** |
| 10:54:59 | `6bfe673` | Lead/T1 | test_cli 89→107 | — |
| 10:56:52 | `9e6bfb4` | release-bot | 据早前「我倾向入库」恢复 `dist/`，另打 `v1.1.0-cpp` | **`dist/` 重新入库**，与 Lead 决定冲突 |
| 10:57:34 | `3ed3999` | Lead | 再改 `.gitignore` + 收录本报告 | 仍**无效** |
| 10:57:58 | `64ab5c3` | Lead | `git rm -r --cached dist/` | **真正移出** |
| 10:59:35 | `9ff8fdb` | Lead | 修正 README/校验和、重打 zip、重打 tag | 终态 |

**根因两条**：

1. release-bot 在 Lead 作出「`dist/` 不入库」决定**之前**已提交 `dist/`；随后又收到一条
   更早发出的「我倾向入库」，据此把 Lead 的移除操作反转。两条指示指向相反且时序未对齐，
   release-bot 未先复核最新状态即执行，属判断失误。
2. `.gitignore` 对**已跟踪**文件不生效。`b14ec43`、`3ed3999` 只加忽略规则，
   必须配 `git rm --cached`，`64ab5c3` 才修对。

`dist/svpn-e634fe15.exe` 的 1.6MB blob 仍留在 `0c1c602` 与 `9e6bfb4` 两个 commit 中；
本地库、无远程。如需彻底清除须改写历史（未执行，也未被要求）。

`v1.1.0-cpp` 标签已由 Lead 清理，仅存 `v1.1.0`。

---

## 5. 如何验证这份产物

```bash
md5sum dist/svpn.exe                   # 期望 e634fe15067808ccf2ba0dad30d55d97
./dist/svpn.exe -v                     # 期望 1.1.0
python tests/test_cli.py               # 期望全部 PASS、FAIL 0
```

实测输出：

```
e634fe15067808ccf2ba0dad30d55d97  dist/svpn.exe
1.1.0
PASS 113 / FAIL 0 / SKIP 0（其中硬失败 0、契约待修 0）
```

用例数在发版窗口内由 89 → 107 → 113 增长（T1 增补）；113 为 release-bot 于 10:56 实测值，
与 tag message 一致。

---

## 6. 职责边界

- **release-bot**：`dist/` 组装、不可变副本（含只读位）、`SHA256SUMS.txt`、`RELEASE.txt`、
  包内 exe 校验、`git init`、首次提交 `0c1c602`、误作 `9e6bfb4` 与 `v1.1.0-cpp`。
- **Lead**：终止上述 git 操作，以 `b14ec43` / `3ed3999` / `64ab5c3` 修正索引与忽略规则；
  将产物改名 `svpn.exe`、简化 `run.cmd`、三次重打 zip；打 `v1.1.0` 并在 `9ff8fdb` 重打。

---

## 7. 与原始指令的偏差

| 指令 | 实际终态 | 说明 |
|---|---|---|
| 产物名 `svpn-e634fe15.exe` | `svpn.exe` | Lead 10:58 改名，使 `run.cmd` 可直接用 |
| `attrib +R` 设只读 | **无只读位** | 改名/重生成后属性丢失，见 §8.1 |
| `git init` + commit + tag 由 release-bot 执行 | 由 Lead 收尾 | 并发冲突后由 Lead 接手 |
| `dist/` 入库（Lead 早前倾向） | **不入库** | 以 Lead 最终决定为准 |
| 用例数 89 | 113 | T1 并发增补 |
| 报告写入 `R3-package.md` | 已写；版本随 `9ff8fdb` 入库，本文为其后重写 | 见 §8.4 |

---

## 8. 遗留问题

### 8.1 `dist/svpn.exe` 无只读属性

```
$ attrib dist\svpn.exe
A                    D:\LE\AI\wbwrk\svpn-cpp\dist\svpn.exe
                     ↑ 仅 A（归档），无 R（只读）
```

release-bot 在 10:52 设过 `+R`；产物改名并将文件重新生成后该属性未恢复。
**后果**：后续任何 `build.cmd` 或覆盖式复制都可能直接冲掉发布物，不可变副本保证失效。

修复只需 `attrib +R dist\svpn.exe`，但属交付物改动，未执行。

### 8.2 `dist/RELEASE.txt` 两处文字与实际不符

`9ff8fdb` 修掉了 `README.md` 里的改名提示，但 **`RELEASE.txt` 未同步**，
且该文件已打进 zip：

- **第 62 行**：`svpn.exe    主程序（已设只读，防止后续构建误覆盖）`
  → 与 §8.1 冲突，当前**没有**只读位。这是发布文档中的失实陈述。
- **第 80 行**：`请先把本文件重命名为 svpn.exe（或自行改脚本中的文件名）`
  → 产物本身已叫 `svpn.exe`，成了循环废话。

另第 61–68 行「包内文件」清单的列对齐已错乱（`svpn.exe` 一行缺缩进）。
均不影响 exe 行为，但会误导使用者。修改会变更 `RELEASE.txt` 的 sha256 并需重打 zip，
超出「只写报告」的授权范围，故未动。

### 8.3 `.gitignore` 残留自相矛盾的注释

文件末尾同时存在：

```
# 发布产物入库：exe 命名带哈希并设只读，与源码同仓才可追溯
...
# 发布产物：二进制不进源码树（靠 MANIFEST + tag 追溯）
dist/
```

第一段是 release-bot 反转期间写入、现已失效的说明。建议删除，否则后来者会误读为
「dist 应当入库」。未改。

### 8.4 本报告的版本状态

`_crew/reports/R3-package.md` 的上一版已随 `9ff8fdb` 入库；**当前磁盘版本是其后重写的**，
包含更完整的终态数据（tag 变更、zip 三次重打、§8.2 的两处失实文字），处于**未提交**状态。
按 Lead「别再碰 git」的要求未提交。

### 8.5 `core.autocrlf=true`

系统默认 `true`，全新检出会把文本文件转为 CRLF，使 `RELEASE.txt` 中记录的源码 md5 不再成立。
当前工作区为 LF，指纹有效。建议后续加 `.gitattributes`（`* text=auto eol=lf`）。发版节点未加。

---

## 9. 当前仓库状态（11:00:42）

```
$ git log --oneline --decorate -6
9ff8fdb (HEAD -> main, tag: v1.1.0)  docs: 去掉过期的改名提示，收录 R3 与 V1 发布物门禁
64ab5c3                              chore: 真正把 dist/ 移出版本索引
3ed3999                              chore: dist/ 再次移出版本控制，并收录 R3 打包报告
9e6bfb4                              chore: dist/ 纳入版本控制，发布物与源码同仓
6bfe673                              test: 补 ④⑧⑨⑩ 契约断言，test_cli 从 89 增至 107 项
b14ec43                              chore: dist/ 不纳入版本控制

$ git status --short
 M _crew/reports/R3-package.md
```

- `git ls-files` = 37，`dist/` 不在其中
- 工作区仅本文一处未提交改动
- 仅本地仓库，无 remote，未推送
