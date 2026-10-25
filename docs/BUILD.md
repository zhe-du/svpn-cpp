# 编译 svpn（C++ 版）

产物是**两个静态链接的可执行文件**（各约 1.7MB）：`svpn.exe`（控制台，命令行 + `svpn gui`）与
`svpn-gui.exe`（窗口子系统，双击不弹控制台）。两者是同一份源码、同一套逻辑，只是子系统不同；
目标机不需要装 MinGW 运行库、不需要 Python、不需要 .NET。

---

## 1. 工具链要求

| 项 | 要求 | 本机实测 |
|---|---|---|
| 编译器 | MinGW-w64 `g++` ≥ 11（编译用 `-std=c++20`；代码只依赖 `std::filesystem` / `std::atomic` / `std::thread` / `std::mutex` 这类标准库设施） | **16.2.0**（`x86_64-posix-seh-rev1`，MinGW-Builds） |
| 目标平台 | Windows 10 / 11 x64 | Windows 11 Pro for Workstations |
| 架构 | 必须是 **x86_64**（`-municode` + SChannel + 64 位 `SOCKET` 假定） | x86_64 |
| 额外依赖 | 无。全部用系统库：Winsock2 / SChannel / WinHTTP / Win32 | — |

`build.cmd` 里默认写死了一条本机路径，找不到就回落到 `PATH` 里的 `g++`：

```
set "CXX=D:\LE\AI\tools\mingw64\bin\g++.exe"
if not exist "%CXX%" set "CXX=g++"
```

换机器只改这一行，或者把 MinGW 的 `bin` 加进 `PATH`。

---

## 2. 一条可复制的编译命令

在项目根目录（`src\` 的上一级）执行。cmd 里用 `^` 续行：

```bat
D:\LE\AI\tools\mingw64\bin\g++.exe -std=c++20 -O2 -Wall -Wextra -municode -mconsole -static -s ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
    src\json.cpp src\util.cpp src\rules_data.cpp src\config.cpp src\rules.cpp ^
    src\net.cpp src\tls.cpp src\proxy.cpp src\cdn.cpp src\winsys.cpp ^
    src\pac.cpp src\sni.cpp src\main.cpp ^
    -o svpn.exe ^
    -lws2_32 -lcrypt32 -lsecur32 -lwinhttp -ladvapi32 -luser32 -lshell32 -liphlpapi -lole32 -luuid ^
    -ldnsapi -lwininet
```

Git Bash / MSYS 下把反斜杠换成正斜杠、续行用 `\` 即可，`-o svpn.exe` 与库列表不变。

**源文件顺序无所谓**，链接器会自己解决符号；但 `src\*.cpp` 必须**全部**参与编译（`main.cpp` 之外没有单独的头文件实现）。

编译完立刻验证：

```bat
svpn.exe -v
```

应输出：

```
1.1.1
```

---

## 3. 为什么必须 `-static`

不加 `-static` 时 MinGW 默认动态链接 libstdc++ / libgcc / winpthread，生成的 exe 在**没装 MinGW 的机器**上双击会立刻退出：

- 双击：窗口一闪，什么也不打印
- 命令行：退出码 **127**（`STATUS_DLL_NOT_FOUND` 的一种表现），或弹「找不到 libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll」

`-static` 把这些运行库全部塞进 exe，代价只是体积（约 1.6MB），换来「拷一个文件就能跑」。

配套的两个开关：

| 开关 | 作用 |
|---|---|
| `-s` | strip 符号表，体积从约 3MB 降到约 1.6MB；不影响报错可读性（本项目日志自带上下文，不靠符号表） |
| `-mconsole` | 生成控制台子系统程序。本项目全程用 `std::cout`，必须保留控制台，**不要**加 `-mwindows`（那会把标准输出吞掉） |

> 说明：`runtime\` 下的运行期文件、`config.json` 都放在 **exe 所在目录**，不是当前工作目录。所以 `svpn.exe` 可以放在任意路径，从别处调用也不影响它的行为。

---

## 4. 为什么必须 `-municode`

入口是 `src/main.cpp` 里的 **`wmain`**，不是 `main`：

```cpp
int wmain(int argc, wchar_t** argv) { ... }
```

用 `wmain` 是为了拿到**宽字符命令行**，再统一转成 UTF-8（`to_u8`）交给内部逻辑——否则中文路径、中文 `--exe` 参数在 CP936 控制台下会被截断/乱码。

MinGW 只有在 `-municode` 时才会把启动代码接到 `wmainCRTStartup`。**忘加就会链接失败**：

```
undefined reference to `WinMain'
... undefined reference to `main'
```

同理，`-finput-charset=UTF-8 -fexec-charset=UTF-8` 也不能省：源码是 UTF-8（含大量中文注释与提示语），GCC 默认按当前 locale 猜测输入编码，在 CP936 环境下会报 `converting to execution character set: Illegal byte sequence`，或者编出来的中文提示全是乱码。

---

## 5. `build.cmd` 做什么

双击或 `cmd /c build.cmd` 即可。它按顺序做五件事：

1. 定位编译器（本机路径 `D:\LE\AI\tools\mingw64\bin\g++.exe` → `PATH` 回落），打印 `[i] compiler: ...`
2. 收集 `src\*.cpp` 全量文件列表；一个都没有就 `exit /b 2`
3. **第一趟**：用同一套 `FLAGS + LIBS` 加 `-mconsole` 编出 `svpn.exe`，打印 `[1/2] svpn.exe (console, CLI + "svpn gui")`
4. **第二趟**：同样的源码加 `-mwindows -DSVPN_GUI_SUBSYSTEM` 编出 `svpn-gui.exe`，打印 `[2/2] svpn-gui.exe (windowed, no console flash)`
5. 打印两个产物的绝对路径与字节数：`[+] built D:\...\svpn.exe 1761792 bytes`（数字随版本变）

两趟用的是**同一份源码**，只有入口子系统与那个宏不同，所以两个 exe 的行为完全一致——GUI 版也不会另有一套逻辑。

失败时按阶段分别打印 `[x] console build failed` 或 `[x] gui build failed`，并原样返回非 0 退出码，方便脚本判断。

> `build.cmd` 里会 `mkdir build`，但当前编译是「一步到 exe」，不落 `.o`。`build\` 只是历史产物目录，空着不影响编译，可以删。
>
> 库列表含 `-ldnsapi -lwininet`（DoH 与系统代理要用；少这两个会报 `DnsFlushResolverCache` / `InternetSetOptionW` 未定义），GUI 还要 `-lcomctl32 -lgdi32`。

---

## 6. 常见错误对照

| 现象 | 原因 | 处理 |
|---|---|---|
| `undefined reference to 'WSAStartup'` / `'socket'` / `'select'` | 缺 `-lws2_32` | 补 `-lws2_32` |
| `undefined reference to 'AcquireCredentialsHandleW'` / `'InitializeSecurityContextW'` / `'QueryContextAttributesW'` | 缺 `-lsecur32`（SChannel） | 补 `-lsecur32` |
| `undefined reference to 'CertGetNameStringW'` / `'CertFreeCertificateContext'` | 缺 `-lcrypt32` | 补 `-lcrypt32` |
| `undefined reference to 'WinHttpOpen'` / `'WinHttpSendRequest'` | 缺 `-lwinhttp`（DoH 查询走 WinHTTP） | 补 `-lwinhttp` |
| `undefined reference to 'RegSetValueExW'` / `'RegOpenKeyExW'` | 缺 `-ladvapi32` | 补 `-ladvapi32` |
| `undefined reference to 'ShellExecuteExW'` | 缺 `-lshell32`（提权 UAC） | 补 `-lshell32` |
| `undefined reference to 'GetAdaptersAddresses'` | 缺 `-liphlpapi` | 补 `-liphlpapi` |
| `undefined reference to 'CoCreateInstance'` / `'IID_...'` | 缺 `-lole32 -luuid` | 两个都补 |
| `undefined reference to 'DnsFlushResolverCache'` | 缺 `-ldnsapi`（写 hosts 后刷 DNS 缓存，`winsys.cpp` 自己声明了 `dllimport`） | 补 `-ldnsapi` |
| `undefined reference to 'InternetSetOptionW'` | 缺 `-lwininet`（改完系统代理后通知 WinINET 刷新） | 补 `-lwininet` |
| `undefined reference to 'WinMain'` | 用了 `wmain` 但没加 `-municode` | 加 `-municode` |
| 编译期 `Illegal byte sequence` / 中文提示乱码 | 源文件按 CP936 解释 | 加 `-finput-charset=UTF-8 -fexec-charset=UTF-8` |
| 编译通过，换机器双击**闪退**，或命令行退出码 **127** | 忘了 `-static`，缺 libstdc++/libgcc/winpthread DLL | 加 `-static` |
| 双击后没有任何输出（日志里也没有） | 误加了 `-mwindows` | 去掉，改回 `-mconsole` |
| `'g++' 不是内部或外部命令` | 编译器不在 `PATH` | 用绝对路径，或把 `D:\LE\AI\tools\mingw64\bin` 加进 `PATH` |

---

## 7. 编不出来时的最小自检

```bat
:: 1. 编译器在不在、版本够不够
D:\LE\AI\tools\mingw64\bin\g++.exe --version

:: 2. 源文件齐不齐（应为 14 个 .cpp + 1 个 .hpp）
dir /b src\*.cpp
dir /b src\*.hpp

:: 3. 编译并确认产物
D:\LE\AI\tools\mingw64\bin\g++.exe ...
svpn.exe -v
svpn.exe doctor
```

`svpn.exe doctor` 在没有任何配置时也会给出可读结论：缺 `config.json` 会提示跑 `svpn.exe init`；JSON 写坏了打印 `[!] config.json 解析失败: …（当前按默认值运行）`、键名不认识打印 `[!] config.json 里有 N 个未知键…`，两种情况都返回 `1`。是「编出来的东西到底能不能跑」最快的判据。
