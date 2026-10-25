# svpn C++ 重写 · 模块规范

## 目标
把 `D:\LE\AI\dshwork\steam-vpn\`（Python，4387 行，364 用例已验证）**逐功能**重写成
`D:\LE\AI\wbwrk\svpn-cpp\` 的 Windows 原生单 exe（MinGW-w64 静态链接，零外部依赖）。

Python 版是**行为基准**：语义、默认值、提示文案、边界处理都要一致。有疑问先读 Python 源码。

## 硬约束

1. **只写自己名下的 .cpp**。`src/svpn.hpp` 是共享契约，**不许改**；发现签名不够用就在报告里写清楚，由 Lead 统一改。
2. **不许引入第三方库**。只能用 Win32 / Winsock2 / SChannel / WinHTTP / CRT / STL。
3. **C++20，`-Wall -Wextra` 零警告**。提交前必须自己编译通过：
   ```
   cd /d D:\LE\AI\wbwrk\svpn-cpp
   D:\LE\AI\tools\mingw64\bin\g++.exe -std=c++20 -O2 -Wall -Wextra -c ^
       -finput-charset=UTF-8 -fexec-charset=UTF-8 src\<你的>.cpp -o build_<你的>.o
   ```
4. **字符集**：源码与运行时字符串一律 UTF-8。控制台由 `console_utf8()` 处理。文件路径走 `std::filesystem::path`（宽字符）。
5. **并发模型**：线程 + 阻塞 socket + `select` 双向中继。禁止 IOCP/epoll。服务函数自己跑 accept 循环，
   每连接一个 `std::thread`（detach 前记得登记、退出时 join 或计数），并且**必须**响应 `stop` 原子量（每 100ms 检查一次）。
6. **异常**：跨线程边界不许抛。I/O 失败返回 `false` / `-1` 并写 `log_write("WARN", ...)`。
7. **不许 printf 中文到窄流**：统一用 `fmt()` + `std::cout`/`printf` 前先 `console_utf8()`（main 已调用）。

## 文件归属

| 代号 | 文件 | 参考（Python） |
|---|---|---|
| A1 | `src/config.cpp`、`src/rules.cpp` | `svpn/cli.py`(配置段)、`svpn/rules.py` |
| A2 | `src/net.cpp` | `svpn/netutil.py`(socket/DNS/竞速) |
| A3 | `src/tls.cpp` | `svpn/netutil.py`(tls_over/tls_probe/sni 解析) |
| A4 | `src/proxy.cpp` | `svpn/proxy.py` |
| A5 | `src/cdn.cpp` | `svpn/cdn.py` |
| A6 | `src/winsys.cpp`、`src/pac.cpp` | `svpn/system.py`、`svpn/pac.py` |
| A7 | `src/sni.cpp` | `svpn/forward.py` |
| Lead | `src/main.cpp`、`src/json.cpp`、`src/util.cpp`、`build.cmd` | `svpn/cli.py` |

## 验收（每个模块都要做）

1. 单 TU 编译零警告（命令见上）。
2. 至少 3 个自测用例：能在 `tests/test_<模块>.cpp` 里写 `main()` + `assert` 最好；不方便就写一次性脚本并在报告里贴输出。
3. 边界必须覆盖：空值/None、超长、非法字符、连接失败、超时。
4. 报告 `_crew/reports/<代号>.md`，五段：改了哪些文件 / 逐条实现 / 验证结果（贴真实输出）/ 契约问题 / 没做完。

## 语义对齐要点（易错）

- `Config.connect_timeout` 单位**秒**；socket 超时用**毫秒**。
- `http_port == 0` 表示**不启动 HTTP 入口**（不是 7900）。
- `upstream.host == "NODE_HOST"` 等于「没配节点」（`configured()` 已实现）。
- 分流优先级：`direct_hosts` > `proxy_hosts` > 内网/回环直连 > IP 段 > 后缀 > mode 兜底。
- `hosts` 写盘必须**原子**：先写 `hosts.tmp` 再 `MoveFileEx(REPLACE_EXISTING)`；保留原文件 BOM 与编码，只动托管块。
- 所有对外可见的提示文案（`[+]`/`[i]`/`[!]`/`[=]`）保持与 Python 版一致的风格。
