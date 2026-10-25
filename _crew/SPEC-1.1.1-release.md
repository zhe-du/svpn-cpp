# SPEC · svpn-cpp 1.1.1 发版收尾（本轮下发规范）

> 本文件是本轮所有子 AI 的**唯一共同约定**。开工第一件事：读它。
> 与 `_crew/SPEC.md`（1.1.0 模块分工）并列，不替代它。

## 0. 背景（一句话）

C++ 版已从 1.1.0 走到 **1.1.1**（新增 Win32 原生 GUI，双产物 `svpn.exe` + `svpn-gui.exe`），
代码级缺陷已清零、测试全绿，但**还没发布**：`dist/` 仍是 1.1.0 的单产物包，
10 项改动未提交，tag `v1.1.1` 未打，发布面还有若干文档欠账。本轮就是把这些做完。

## 1. 仓库与工具链（照抄，别猜）

| 项 | 值 |
|---|---|
| 仓库 | `D:\LE\AI\wbwrk\svpn-cpp`（分支 `main`，HEAD `a84dd7a`） |
| 远端 | `git@github.com:zhe-du/svpn-cpp.git` |
| 编译器 | `D:\LE\AI\tools\mingw64\bin\g++.exe`（g++ 16.2.0，MinGW-w64 x86_64-posix-seh-rev1） |
| 构建 | `build.cmd`（一趟编两个产物：`-mconsole` / `-mwindows -DSVPN_GUI_SUBSYSTEM`） |
| Python | `C:\Users\zhedu\.workbuddy\binaries\python\envs\default\Scripts\python.exe`（无 pip） |
| gh | `gh` 2.102.0，已登录 `zhe-du`，scope 含 `repo` |
| 版本号来源 | `src/main.cpp` 的 `constexpr ... kVersion`（当前 `1.1.1`；`-v` 打印的就是它） |

## 2. 硬约束（破一条即返工）

1. **无第三方库**。只用 Win32 / Winsock2 / SChannel / WinHTTP / CRT / STL。
2. `-std=c++20 -Wall -Wextra` **零警告**。你改了代码就必须自带一次干净编译验证。
3. `src/svpn.hpp` 是共享契约。**本轮只有 P4 那位可以改它**，且只能加字段/加声明，不许改既有签名。
4. 源码与运行期字符串一律 **UTF-8**；`build.cmd` 已带 `-finput-charset=UTF-8 -fexec-charset=UTF-8`。
5. 线程 + 阻塞 socket + `select`；禁 IOCP/epoll；跨线程不抛异常；禁把中文 `printf` 到窄流。
6. **禁止 `system on` / `system off` / `--system-proxy` / `hosts apply` / `hosts clear --no-elevate`** ——
   会改本机全局 WinINet 或弹 UAC，污染并行测试。要测这些分支：只读代码 + 静态推理，在报告里明写"未实测，原因"。
7. 一个文件一个主人（见 §4）。**不是你的文件，一个字节都不能改**；发现问题只写进报告的"跨文件问题"一节。
8. 文档里每条断言必须有出处：源码行号、实测命令输出、或已有报告。**不许写"应该"、"预计"、"未来会支持"**。

## 3. 编码与写入（本轮踩过的坑）

- 改 UTF-8 源码**不要**用 PowerShell `Get-Content | Set-Content`（PS 5.1 按 ANSI 读、按 UTF-8 写，中文双重编码毁掉）。用 edit 类工具或 Python 显式 `encoding='utf-8'`。
- 新建 `.md` / `.cpp` 一律 **UTF-8 无 BOM + LF**（仓库即将用 `.gitattributes` 锁 LF）。
- `.cmd` 里注释只能 ASCII；含中文的行后必须留一个空行（CP936 行偏移）。
- 报告文件名、路径全 ASCII。

## 4. 文件所有权（互斥表）

| 主人 | 独占文件 | 任务 |
|---|---|---|
| **W1 p4** | `src/main.cpp`、`src/svpn.hpp`、`tests/test_cli.py` | P4：`upstream_kind` 进 effective.json + status 读回 + 用例 |
| **W2 git** | `.gitattributes`(新)、`.gitignore`、`tools/` | git 卫生 |
| **W3 arch** | `docs/ARCHITECTURE.md`(新) | 架构文档 |
| **W4 win** | `docs/WINDOWS.md`(新) | Windows 踩坑文档 |
| **W5 en** | `README.en.md`(新) | 英文 README |
| **W6 rep** | `_crew/reports/packaging.md`(新)、`_crew/reports/log-analysis.md`(新) | 流程报告 |
| **W7 v2** | `_crew/reports/V2-branch-coverage.md`(新) | 分支补测 |
| **Lead** | `build.cmd`、`dist/`、`README.md`、`_crew/SPEC-1.1.1-release.md`、`_crew/reports/R4-release.md` | 重建、打包、提交、发版 |

只读参考（谁都能读，谁都不能改）：`src/*.cpp`（非自己那份）、`README.md`、`docs/BUILD.md`、
`_crew/SPEC.md`、`_crew/reports/*.md`、`config.example.json`、`run.cmd`。

## 5. 测试与端口纪律

- 全量测试三套：
  - `tests\test_config.exe` → 期望 `178 PASS / 0 FAIL`
  - `tests\test_sni_parse.exe` → 期望 `27 PASS / 0 FAIL`
  - `python tests\test_cli.py` → 期望 `PASS 113+ / FAIL 0 / SKIP 0`（Lead 本轮已复跑确认 113/0/0）
- 跑测试前**先确认 7899 / 7900 / 443 空闲**：`netstat -ano | grep -E ":(7899|7900) "`。
- `test_cli.py` 会自己挑空闲端口（`pick_free_port()`），但仍**同一时刻只许一个进程跑它**。
- 要起实例就用 `-p <高位唯一端口>`（如 19xxx），跑完立刻 `down`。**绝不占用 7899/7900**。

## 6. 报告格式（五段式，缺段即不合格）

每份报告必须按这个骨架写，标题用 `#`，不要 emoji：

```
# <编号> <标题>
## 1. 摘要        —— 5 行以内，结论先行
## 2. 做了什么    —— 逐项，带文件路径与行号
## 3. 验证        —— 命令 + 真实输出（粘贴原文，不要复述）
## 4. 契约变更    —— 有无影响他人；无则写"无"
## 5. 没做完/风险 —— 诚实列，带原因
```

## 7. 交付路径

- 报告写到 `_crew/reports/`。
- 临时脚本、临时目录写到 `D:\LE\AI\wbwrk\wbwrk_tmp\`，**不许留在仓库根目录**。
- 收工时确认 `git status --short` 里**只有你那份文件**是新增/修改。

## 8. 本轮不做的事（明确划出，别顺手做）

1. 不改写 git 历史（`dist/` blob 仍留在 `0c1c602`/`9e6bfb4`，用 tag + 说明覆盖，不动历史）。
2. 不动 Python 仓（`D:\LE\AI\dshwork\steam-vpn`）——`_DNS_CACHE` 那条 P1 是独立一轮。
3. 不做 GUI 功能扩展（不加新按钮、不改布局），本轮只发版。
4. 不引入 CI、不写 GitHub Actions。
