# P4 upstream_kind 写入 effective.json 与 status 读回

## 1. 摘要

`up -b --node http://...` 的 kind 现在会随 `upstream` 一起写进 `runtime/effective.json` 的
`upstream_kind`，`status` 读回后「节点」行显示 `（http）`，不再谎报 `（socks5）`。
旧格式 effective.json（无该键）保持原样、不报错（已实测）。
新增 `[M2]` 用例组 5 项；`test_cli.py` 由 113 升到 **PASS 118 / FAIL 0 / SKIP 0**，
`test_config.exe` 178/0、`test_sni_parse.exe` 27/0，构建零警告。

## 2. 做了什么

### 2.1 写侧：`src/main.cpp:398-401`（`cmd_serve` 里构造 `Json ej`）

在 `upstream_configured == true` 分支内、紧跟 `upstream` 之后新增一行，与 `upstream` 同条件：

```cpp
        if (cfg.upstream.configured()) {
            ej.set("upstream", Json::make_str(fmt("%s:%d", cfg.upstream.host.c_str(),
                                                  cfg.upstream.port)));
            // kind 也必须落盘：`--node http://...` 只改了内存里的 cfg，config.json 仍是旧
            // kind，status 读回后会把「节点」行显示成 socks5（链路实际走 HTTP = 假信息）
            ej.set("upstream_kind", Json::make_str(cfg.upstream.kind));   // ← 第 400 行，新增
        }
```

取值直接取 `cfg.upstream.kind`，没有引入新枚举。代码里真实存在的取值只有两个来源：

- `src/main.cpp:156` —— `cfg.upstream.kind = starts_with(node, "http://") ? "http" : "socks5";`
- `src/config.cpp:160` —— `const char* const kKinds[] = {"socks5", "http", "direct"};`
  （`direct` 只可能来自手写 config.json，`--node` 造不出来；本次不新造取值。）

### 2.2 读侧：`src/main.cpp:525-530`（`cmd_status` 里 `upstream_configured` 分支）

```cpp
                // 1.1.0 及更早的 effective.json 没有这个键：缺省就保持 config.json 的
                // kind（不清空、不报错），只有真读到值才覆盖。            ← 第 525-526 行
                if (const Json* k = j.get("upstream_kind")) {              // 第 527 行，新增
                    std::string kind = k->as_str();                        // 第 528 行
                    if (!kind.empty()) cfg.upstream.kind = kind;           // 第 530 行
                }
```

- 只在这一个分支内读，与 `upstream` 的覆盖条件完全一致（`upstream_configured` 为假时不碰 kind）。
- 键缺失 / 空串 → 一个字节都不改 `cfg.upstream.kind`，即保持 `load_config` 从 config.json 读到的值。
- `else if (j.has("upstream_configured"))`（第 531-534 行，未配置节点）分支没动：此时上游为空，
  「节点」行走「未配置（纯直连）」，kind 不参与显示。

### 2.3 显示侧：`src/main.cpp:546-548`（未改动）

`fmt("节点 %s:%d（%s）\n", ..., cfg.upstream.kind.c_str())` 本来就读 `cfg.upstream.kind`，
读侧补上后自动正确，因此**显示代码一行没动**。

### 2.4 用例：`tests/test_cli.py:708-746`（新增函数）+ `tests/test_cli.py:830`（挂进 main）

新增 `[M2] up --node http:// 覆盖后 kind 必须一致（⑯）`，紧跟 `[M]`（⑮）之后，照抄其结构：
`pick_free_port()` 两次（一个给 `-p`，一个给假上游，避开 7899/7900）、`up -b -p <alt> --node http://<spec>`、
`wait_pidfile()` 等 pidfile、读 `effective.json`、跑 `status`、最后 `down` + `wait_closed()`。
5 项断言：rc=0；`upstream_kind == "http"`；`upstream == spec`；节点行含 `http` 且不含 `socks5`；down 成功。

### 2.5 未改动的文件

`src/svpn.hpp` 本轮**没动**（`UpstreamCfg::kind` 已存在，无需加字段/声明）。
`build.cmd`、`src/gui.cpp`、`docs/`、`README*`、`.gitignore` 一律未碰（见第 5 节 git status 说明）。

## 3. 验证

### 3.1 端口纪律（构建前）

```
$ netstat -ano | grep -E ":(7899|7900) "
---ports checked---
```
（空输出 = 7899/7900 空闲。）

### 3.2 构建（`cmd /c build.cmd`，`-Wall -Wextra`，零警告）

```
[i] compiler: D:\LE\AI\tools\mingw64\bin\g++.exe
[1/2] svpn.exe  (console, CLI + "svpn gui")
[2/2] svpn-gui.exe  (windowed, no console flash)
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn.exe  1761792 bytes
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn-gui.exe  1760256 bytes
```

MD5（新旧，含被覆盖的 GUI 产物）：

```
改动前  svpn.exe      6f068265d4e0217c85cb06e28d381aca  1761792  12:34:39
改动前  svpn-gui.exe  401103a4b94bd6527a870385310058bb  1759744  12:34:39
改动后  svpn.exe      cee89338fbf048d8c516f377dbfe8cb9  1761792  12:54:31
改动后  svpn-gui.exe  8a8faea613693d47ee9b0b0d06733200  1760256  12:56:23
```

（`test_cli.py` 硬编码读 `ROOT/svpn.exe`，故必须重建根目录二进制；`svpn-gui.exe` 与
`svpn.exe` 编译同一组 `src\*.cpp`，同样含本次改动。）

### 3.3 手工冒烟：写侧 + 读侧（临时目录 `D:\LE\AI\wbwrk\wbwrk_tmp\p4_smoke`）

```
$ ./svpn.exe init && ./svpn.exe up -b -p 19301 --node http://127.0.0.1:19302
[+] 已写入 D:\LE\AI\wbwrk\wbwrk_tmp\p4_smoke\config.json
[i] SOCKS5 127.0.0.1:7899 / HTTP 127.0.0.1:7900
[+] 后台已启动 pid=25748  socks5://127.0.0.1:19301  http://127.0.0.1:7900
rc=0
--- effective.json ---
{
  "listen_host": "127.0.0.1",
  "listen_port": 19301,
  "http_port": 7900,
  "mode": "whitelist",
  "upstream_configured": true,
  "upstream": "127.0.0.1:19302",
  "upstream_kind": "http"
}
--- status ---
svpn 1.1.1  ·  Steam 分流/加速
进程     运行中 pid=25748
端口     127.0.0.1:19301  监听中
端口     127.0.0.1:7900  监听中
节点     127.0.0.1:19302（http）
```

（config.json 里 `upstream.kind` 仍是 `socks5`；节点行现在显示 `http`，与 `--node` 一致。）

### 3.4 向后兼容冒烟：手写 1.1.0 格式的 effective.json（无 `upstream_kind`）

```
=== 旧格式（无 upstream_kind）status 节点行 ===
端口     127.0.0.1:19303  未监听
端口     127.0.0.1:19304  未监听
节点     127.0.0.1:19305（socks5）
rc=0
```

（config.json 的 kind 为 `socks5`；节点行照旧显示 `socks5`，既没清空成空括号，也没报错。）

### 3.5 三套测试（最终二进制 `md5=cee89338fbf048d8c516f377dbfe8cb9`）

```
$ tests/test_config.exe
[+] PASS 178 / FAIL 0（共 178 项）

$ tests/test_sni_parse.exe
[+] PASS 27 / FAIL 0（共 27 项）
```

```
$ python tests/test_cli.py
svpn CLI 回归（T1）
  被测二进制: D:\LE\AI\wbwrk\svpn-cpp\svpn.exe
  md5=cee89338fbf048d8c516f377dbfe8cb9  size=1761792  mtime=2026-10-25 12:54:31
  端口: SOCKS 53155 / HTTP 53156（避开 7899/7900）

[M2] up --node http:// 覆盖后 kind 必须一致（⑯）
  PASS  up -b --node http://127.0.0.1:55092 → rc=0
  PASS  effective.json upstream_kind == 'http'
  PASS  effective.json upstream == 127.0.0.1:55092
  PASS  status 节点行 kind 与 --node 一致（http 而非 socks5）
  PASS  ⑯ 用例收尾：down 成功

PASS 118 / FAIL 0 / SKIP 0（其中硬失败 0、契约待修 0）
```

对照基线 113 → 118（+5，正是 `[M2]` 的五项）。`grep FAIL` 只命中末行统计，无失败项。

## 4. 契约变更

- **文件格式**：`runtime/effective.json` 新增可选键 `"upstream_kind"`，字符串，取值域同
  `config.json` 的 `upstream.kind`（`socks5` | `http`；`--node` 只会产出这两个）。
  写入条件与 `upstream` 完全相同（`upstream_configured == true`）。
- **读取方**：仅 `cmd_status`（`src/main.cpp:527`）。`down` 只删文件、不读该键，不受影响。
- **兼容性**：旧 effective.json 无该键 → 保持 config.json 的 kind，不清空、不报错（3.4 实测）。
- **`src/svpn.hpp`**：无变更。

## 5. 没做完/风险

1. **只验证了「kind 落盘→读回→显示」这条链路，没验证真实 HTTP 上游转发。** `[M2]` 与 `[M]` 一样用
   假上游（空闲端口，不监听），`up` 不做上游连通性探测，因此用例不依赖网络。真实 HTTP/CONNECT
   转发走 `src/proxy.cpp`，本轮未实连（那需要真代理凭据，超出本任务边界）。
2. **读侧不做白名单校验。** `config.json` 的 kind 经 `config.cpp:176` 的 `kKinds` 归一化，
   `effective.json` 的 kind 只判空串。若有人手工把 `upstream_kind` 写成 `"foobar"`，`status` 会原样
   显示 `（foobar）`。这是为「不发明取值」而留的弱校验；effective.json 是 `up` 自己写的运行期文件，
   正常路径不会出现非法值。
3. **`src/pac.cpp` 在 12:51:47 被他人改动**（不在我的所有权清单内，我没有碰它）。我的最终重建
   （12:54）已包含该改动，3.5 的三套测试是在这个最终二进制上跑的，结论对当前工作树成立。
4. **第一次 `build.cmd` 在 `[2/2] svpn-gui.exe` 链接时报
   `ld.exe: cannot open output file svpn-gui.exe: Permission denied`**（当时有别的 teammate 的进程/构建
   占用该文件）。重跑一次即成功，产物见 3.2。如果 Lead 打包前再遇同样报错，先确认没有
   `svpn-gui.exe` 在运行再重试。
5. **`svpn-gui.exe` 未做功能实测**（GUI 不属本轮 P4 范围）。只确认它随 `build.cmd` 重新链接成功、
   零警告；`src/gui.cpp` 我一个字节都没改。
6. **`git status --short` 里我这两个文件是：`M src/main.cpp`、`M tests/test_cli.py`。**
   同屏出现的 `M src/svpn.hpp`、`M src/pac.cpp`、`M src/util.cpp`、`?? src/gui.cpp` 等均为他人
   1.1.1 既有改动，非本轮 P4 所为（`src/svpn.hpp` 的 diff 内容是 GUI 的 `dispatch_cli`/`gui_main`
   声明，与本任务无关）。
7. **`up -b` 只在进程启动时写一次 effective.json**，运行期再改 config.json 不会刷新它，`status`
   以 effective.json 为准 —— 这是既有行为，本轮未改（若要改属新需求）。
