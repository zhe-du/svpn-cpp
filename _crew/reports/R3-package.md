# R3 打包报告 — svpn 1.1.0-cpp

状态：**完成**

发布产物 `e634fe15`（Lead 裁决 A，源码冻结）。产物落地 `dist/`、纳入 git、打标签。

---

## 1. dist 清单与逐文件 SHA256

```
D:\LE\AI\wbwrk\svpn-cpp\dist\
```

| 文件 | 大小 | SHA256 |
|---|---|---|
| `svpn-e634fe15.exe` | 1,678,848 B | `45dcee1899fc3620feb04632ba74ba80a7bd61e296ed0604c26f7bd8f25e0e6b` |
| `README.md` | 22,219 B | `e1657da9ad034e70fc4d73a673af94ae1a85a4d699a945a9e31effe497425fa6` |
| `LICENSE` | 1,063 B | `0e14238e7ca576da271d122cfa4af4e6e6be05c26f6e51ff5aec44cb529f6206` |
| `run.cmd` | 823 B | `ca2a940e1dd9b2be40e7d6d2064d467b17c2d8836afe05d967e65cc79690adfe` |
| `config.example.json` | 2,084 B | `2760af59dc4d49c9f499da434755869e4cd75ebb4cc2f0f2d103b7359274fcc2` |
| `SHA256SUMS.txt` | 394 B | （校验清单本体） |
| `RELEASE.txt` | 3,397 B | （发布说明本体） |

`sha256sum -c SHA256SUMS.txt` → **5/5 OK**。

### 不可变副本

```
svpn.exe  →  dist/svpn-e634fe15.exe
```

复制后复核，再设只读：

```
$ md5sum dist/svpn-e634fe15.exe
e634fe15067808ccf2ba0dad30d55d97  dist/svpn-e634fe15.exe

$ attrib dist\svpn-e634fe15.exe
A    R               D:\LE\AI\wbwrk\svpn-cpp\dist\svpn-e634fe15.exe
                     ↑ R = 只读

设只读后再次复核 → e634fe15067808ccf2ba0dad30d55d97（未变）
```

### RELEASE.txt 内容

按 Lead 指定写死：版本 `1.1.0-cpp`、md5 `e634fe15067808ccf2ba0dad30d55d97`、
大小 1,678,848 B、构建时间 `2026-10-25 10:46:27`、编译器
`g++ 16.2.0 (x86_64-posix-seh-rev1, Built by MinGW-Builds project)`、
14 个 `src/` 文件 md5 清单，以及「本 exe 由上述源码用 `build.cmd` 构建」。

编译器事实经实机核对：

```
$ D:/LE/AI/tools/mingw64/bin/g++.exe --version | head -1
g++.exe (x86_64-posix-seh-rev1, Built by MinGW-Builds project) 16.2.0

$ ... -dumpmachine
x86_64-w64-mingw32
```

### 排除项（已核）

```
config.json / runtime/ / *.o / tests/*.exe / tests/_* / hosts.backup /
wininet-backup.json / effective.json / *.pid / *.log
```

扫描 `dist/` → **零命中**。

---

## 2. git

`git init -q -b main`，身份 `zhe-du <zhe-du@users.noreply.github.com>`（仓库局部）。
**仅本地，未建远程、未推送。**

### `git show --stat` 原文

```
commit 9e6bfb4fe7fd1bd6d4d0a9224bb39629813f296b
Author: zhe-du <zhe-du@users.noreply.github.com>
Date:   Sun Oct 25 10:56:52 2026 +0800

    chore: dist/ 纳入版本控制，发布物与源码同仓
    
    发布物 svpn-e634fe15.exe（md5 e634fe15067808ccf2ba0dad30d55d97，
    1,678,848 B）与源码置于同一 commit 图，可直接用 tag 追溯"这个 exe 由哪版源码产出"。
    
    dist/ 下仅发布文件；runtime/、config.json、*.o、测试 exe 仍由 .gitignore 排除。
    RELEASE.txt 补齐源码指纹与用例数（发布时 test_cli 113 项全绿）。

 .gitignore               |   5 +-
 dist/LICENSE             |  21 ++++
 dist/README.md           | 280 +++++++++++++++++++++++++++++++++++++++++++++++
 dist/RELEASE.txt         |  87 +++++++++++++++
 dist/SHA256SUMS.txt      |   5 +
 dist/config.example.json |  42 +++++++
 dist/run.cmd             |  33 ++++++
 dist/svpn-e634fe15.exe   | Bin 0 -> 1678848 bytes
 8 files changed, 470 insertions(+), 3 deletions(-)
```

### 提交与标签

| 项 | 值 |
|---|---|
| 发布 commit | `9e6bfb4fe7fd1bd6d4d0a9224bb39629813f296b` |
| 初始 commit | `0c1c6028ed06f9e0e1d73a68e1f18288c313be27`（含源码 + 首版 dist） |
| tag | `v1.1.0-cpp` → `9e6bfb4` |

完整的提交图：

```
9e6bfb4  chore: dist/ 纳入版本控制，发布物与源码同仓      ← tag v1.1.0-cpp
6bfe673  test: 补 ④⑧⑨⑩ 契约断言，test_cli 从 89 增至 107 项
b14ec43  chore: dist/ 不纳入版本控制
0c1c602  feat: svpn 1.1.0-cpp —— Windows 原生单 exe 重写（初始提交）
```

### 入库二进制的完整性（关键校验）

`core.autocrlf=true` 环境下入库二进制有被换行符过滤破坏的风险，故从 object 库直接回算：

```
$ git show HEAD:dist/svpn-e634fe15.exe | md5sum
e634fe15067808ccf2ba0dad30d55d97

$ git cat-file -s $(git rev-parse HEAD:dist/svpn-e634fe15.exe)
1678848
```

**与发布产物完全一致**，二进制未被转换。

---

## 3. 如何验证这份产物

```bash
md5sum dist/svpn-e634fe15.exe          # 期望 e634fe15067808ccf2ba0dad30d55d97
./dist/svpn-e634fe15.exe -v            # 期望 1.1.0
python tests/test_cli.py               # 期望全部 PASS、FAIL 0
```

三条实测结果：

```
e634fe15067808ccf2ba0dad30d55d97  dist/svpn-e634fe15.exe
1.1.0
PASS 113 / FAIL 0 / SKIP 0（其中硬失败 0、契约待修 0）
```

---

## 4. 与 Lead 指令的偏差（需知悉）

### 4.1 `dist/` 入库出现过反复

我首次提交（`0c1c602`）已含 `dist/`。约 30 秒后 `b14ec43`
「chore: dist/ 不纳入版本控制」将其移除，理由写在 `.gitignore` 注释里：
「二进制不进源码树，靠 MANIFEST + tag 追溯」。

Lead 的原话是「要不要入库你定：我倾向**入库**」。据此我在 `9e6bfb4` 恢复入库，
并补了 `v1.1.0-cpp` 标签——原注释提到「靠 tag 追溯」，但没有 tag，逻辑不闭环。

**若 Lead 其实倾向不入库，回退只需一条：**
`git rm -r --cached dist && 在 .gitignore 加回 dist/`。历史里那 1.6MB 会留在
`0c1c602`/`9e6bfb4` 两个 commit 中，需改写历史才能彻底清除——本地库、无远程，
代价可控。

### 4.2 `RELEASE.txt` 的用例数

Lead 给的 commit message 写「test_cli 89」，而 T1 在并发中已增至 107，
我打包时实测为 **113**。`RELEASE.txt` 记为「发布时实测 113 项」，
并注明「用例集仍在增补，以仓库内 tests/ 为准」，避免数字再次过期。

那句「test_cli 89」保留在 `0c1c602` 的 commit message 里未改（不改写历史）。

### 4.3 `run.cmd` 与 exe 文件名不一致

Lead 指定产物名为 `svpn-e634fe15.exe`，而 `run.cmd` 内部调用的是 `svpn.exe`。
直接双击 `run.cmd` 会找不到程序。

未改 `run.cmd`（不在授权范围，且会改变其 SHA256）。已在 `RELEASE.txt` 的
「注意」第 3 条显式说明：要用 `run.cmd` 请先重命名为 `svpn.exe`。
命令行直接调用 `svpn-e634fe15.exe` 不受影响，三条验证命令均基于后者。

若希望开箱即用，两个选项：把产物改名回 `svpn.exe`，或改 `run.cmd` 内的文件名。

### 4.4 `core.autocrlf`

系统默认 `true`，会在检出时把文本文件转成 CRLF，从而使 `RELEASE.txt` 里记录的
源码 md5 在全新检出后对不上。**当前本机工作区为 LF，指纹有效。**

建议后续加 `.gitattributes`（`* text=auto eol=lf`）固定该行为。本次未加，
避免在发版节点引入与产物无关的改动。

---

## 5. 当前仓库状态

```
$ git status --short
 M _crew/reports/T1-tests.md
```

唯一改动是 T1 正在写的报告，与本发布无关，未纳入我的提交。

`dist/` 内 7 个文件与 `9e6bfb4` 一致，`svpn-e634fe15.exe` 只读属性在位。
