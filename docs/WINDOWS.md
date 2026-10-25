# Windows 平台备忘录（C++ 版）

`README.md` 讲怎么用，`docs/BUILD.md` 讲怎么编；这里只讲 **Windows 这道坎卡在哪、卡住了看到什么、怎么确认绕过去了**。

适用 Windows 10 1809+ / 11，MinGW-w64 g++ 16.2.0，产物 `svpn.exe` / `svpn-gui.exe`。每条四段：现象 → 根因 → 做法 → 校验。
证据等级：`[实测]` 本轮跑过、命令可复现；`[代码]` 从当前源码可直接读出；`[未验证]` 推理或继承 Python 版，本轮没在 C++ 上验。
Python 版 `docs/WINDOWS.md` 是这批坑的原始记录，但实现是两套（`subprocess`/`winreg` vs Win32），对不上号的集中在第 H 节。

---

## A. 编码

### A1 `-finput-charset` 与 `-fexec-charset` 管的是两件事
- 现象：去掉后编译报 `converting to execution character set: Illegal byte sequence`，或编过但中文全乱。
- 根因：`-finput-charset` 决定 GCC **怎么读源文件字节**（默认按 locale 猜，CP936 下把 UTF-8 中文认成非法序列）；`-fexec-charset` 决定**窄字面量在 exe 里存什么编码**（默认转成 locale 的 ANSI，中文会变成 GBK 字节）。
- 做法：两个都写死在 `build.cmd` 的 `FLAGS`（`build.cmd:19`），别手敲编译命令。
- 校验：`[实测]` `grep -a "已启动" svpn.exe` 命中 2 处，字节是 `e5b7b2 e590af e58aa8`（UTF-8 的「已启动」）——说明 `.rdata` 里存的是 UTF-8，不是 GBK。`[代码] build.cmd:19`、`docs/BUILD.md:99`

### A2 控制台代码页：`chcp 65001` / `SetConsoleOutputCP` 能做什么、不能做什么
- 现象：控制台里中文变 `????` 或乱码。
- 根因：控制台按**自己的代码页**解释收到的字节；程序写 UTF-8、控制台按 936 解，就是乱码，兜底就是 `?`。
- 做法：程序在**任何输出之前**把代码页切成 65001——`console_utf8()`（`src/util.cpp:87-90`）在 `dispatch()` 第一句（`src/main.cpp:1125`）。用户手动 `chcp 65001` 是同一效果的会话级版本。
- **做不到的四件事**：改不了终端**字体**（方块是字体问题）；改不了**已经写出**的字节；对 `-mwindows` 产物完全无效（没有控制台，见 F1）；不能让你混用宽窄两套 API（见 A3）。`[代码] util.cpp:87-90, main.cpp:1125`

### A3 中文写窄流：真实约束是「两个前提同时成立」
- 现象：接手后加一行中文 `printf`，别人机器上变 `????`，或者干脆什么都看不到。
- 根因：`std::cout`/`printf` 写的是**窄字节流**。中文能显示靠两件事同时成立：字面量是 UTF-8（A1）＋ 代码页 65001（A2）；少一条就乱码。`-mwindows` 下更没有控制台，写进黑洞，不报错也不显示。
- 做法：① 命令行只在 `dispatch()` 之后输出，不要在 `wmain` 之前或新入口里 `printf` 中文；② GUI 侧靠自定义 `streambuf` 逐行 `PostMessage`，所以 `svpn-gui.exe` 里同一句 `std::cout` 也能看见（`src/gui.cpp:67-98, 139-147`）；③ **不要**混用 `wprintf`/`printf`（流方向一经确定不可改，要改先 `_setmode`），本项目全程只用窄流。
- 为什么不用 `WriteConsoleW`：它对管道/文件重定向无效，而本项目依赖 `status | grep` 和后台重定向进日志。`[实测]` 重定向后仍是合法 UTF-8（见 A4）；`[实测]` `grep -rn "wprintf\|wcout\|fwprintf" src/` 无命中，印证「全程只用窄流」；`[代码] gui.cpp:67-98`

### A4 `ccs=UTF-8` 只能配宽字符写入 —— 1.1.1 真缺陷
- 现象：`runtime/stats.json` 落成几百字节乱码、开头是 BOM；Python `json.loads` 报 `Unexpected UTF-8 BOM`；C++ 自己读回来也解析失败，于是 `status` 的「上次统计」行**永久不出现**（`src/main.cpp:564-573` 只有取到 `connections` 才打印）。
- 根因：旧代码是 `L"w, ccs=UTF-8"` + `std::fwrite` 写**窄字节串**（`git diff src/util.cpp`）。`ccs=UTF-8` 开的是**宽字符文本流**：每两个字节被当成一个 UTF-16 码元重新编码。
- 做法：字节已经是最终形态的文件（JSON、日志）一律 `L"wb"`（`src/util.cpp:202-208`、`src/cdn.cpp:182`）；只有真用 `fwprintf` 写 `wchar_t` 才该用 `ccs=UTF-8`。
- 校验：`[实测]` 同编译器复现，源 40 字节 → 旧写法 63 字节，`json.loads` 报 `Unexpected UTF-8 BOM (decode using utf-8-sig)`；修好后本机 `runtime/stats.json` 是 223 字节合法 JSON、无 BOM。
  ```
  old(w, ccs=UTF-8): efbbbf e0a9bb e280a0 e794a2 ...   每两字节一个 UTF-16 码元
  new(wb)          : 7b0a 2020 2275 7074 696d 655f ...   原样
  ```

### A5 BOM：读要跳过，写不要写
- 现象：记事本存过的 `config.json`/`hosts` 带 BOM，程序报解析失败。
- 根因：BOM 不是 JSON 语法的一部分，Python `json.loads` 直接报错（A4 实测）。
- 做法：读 JSON 由 `Json::parse` 内部跳过 BOM（`src/json.cpp:242-243`）；读 `hosts` **不解码**，把 BOM 单独摘出、写完原样放回（`src/winsys.cpp:307-352, 528`）；写配置用 `ofstream` 二进制写 `dump()`，天然无 BOM 无尾随换行（`src/config.cpp:366-375`）。
- 校验：`[实测]` 真实夹具（BOM+CRLF+中文 / GBK+LF+中文）：`caseA orig=164 applied=338 cleared=164`、`caseB orig=81 applied=250 cleared=81`，clear 后**与原始字节完全一致**。夹具与断言 `tests/test_winsys.cpp:59-100`（用 `SVPN_HOSTS_FILE` 改道，不碰真实 hosts）。

### A6 `.cmd` 三条规矩，以及本项目为什么「结构上安全」
- 现象：从 CP936 控制台跑 `.cmd` 冒 `'?' is not recognized as an internal or external command`，从第二行起整段错位；带 BOM 则报 `'ï»¿@echo'`。
- 根因：UTF-8 无 BOM 的 `.cmd` 里出现中文（**`rem` 注释也算**）时，cmd.exe 按字节记行偏移，多字节字符让它把**下一行行首吃掉**。这是行偏移，不是乱码。
- 做法：① 注释只用 ASCII；② 含中文的行后紧跟一个空行；③ CRLF、无 BOM。三条缺一不可。
- 现状与校验：`build.cmd`/`run.cmd` 是**纯 ASCII**，故 1、2 天然满足；`.gitattributes` 锁 `*.cmd text eol=crlf`。`[实测]` 非 ASCII 行 0/0，CR 收尾行 40/40、33/33。往里加中文时必须三条同时补。

---

## B. 权限与 UAC

### B1 `is_admin()` 判的是令牌，不是「账号在不在管理员组」
- 现象：管理员组里的账号跑 `svpn hosts` 仍提示需要提权。
- 根因：UAC 下即使是管理员，进程拿到的也是**过滤令牌**，用它开 hosts 一样 `ERROR_ACCESS_DENIED`(5)。
- 做法：`AllocateAndInitializeSid(BUILTIN\Administrators)` + `CheckTokenMembership(nullptr, ...)` 查**当前进程令牌**（`src/winsys.cpp:765-776`）；只依赖 advapi32，不拉 shell32 的 `IsUserAnAdmin`。
- 校验：调用点三处都是「非管理员且没带 `--no-elevate`」才提权（`main.cpp:853, 872, 913`）。`[代码] winsys.cpp:765-776`

### B2 `elevate()` 起的是**新进程**，参数必须自己拼全
- 现象：提权后子进程参数全丢，或者 UAC 弹个没完。
- 根因：走 `ShellExecuteExW` + `lpVerb=L"runas"`（`src/winsys.cpp:778-810`），这是一次全新 CreateProcess，不继承内存/argv/句柄，只有命令行字符串过去。
- 做法：传完整命令行尾部 `elevate("hosts apply --no-elevate")`（`main.cpp:874, 915, 489`）；子进程**必须**带 `--no-elevate`，否则它又发现「不是管理员」再提权，无限弹 UAC——这个参数的唯一作用就是打断递归（`main.cpp:853, 872, 913`）；`lpDirectory=nullptr` 继承父进程 CWD，本项目路径跟 exe 走（见 E1）所以无影响；点「否」给 `ERROR_CANCELLED` 时静默返回 false，不喷堆栈（`winsys.cpp:798`）。
- 校验：`[代码] winsys.cpp:778-810`

### B3 别靠「窗口闪了一下」判断提权成功
- 现象：点 `hosts apply`，黑窗口一闪就没了，用户以为没生效；UAC 点「否」时也可能一闪（其实根本没起来）。
- 根因：`SW_SHOWNORMAL` + `WaitForSingleObject(INFINITE)` 等子进程退出（`winsys.cpp:794, 804`）；子进程是普通控制台程序，干完就退，窗口自然一闪。**成功和失败长一个样**。
- 做法：只看**副作用**——回头数 hosts 托管块，`hosts_managed_entries(p) > 0` 才算成功（`main.cpp:856, 875, 916`）。调用方有意丢弃 `elevate()` 的返回值。
- 校验：`[代码] main.cpp:856/875/916`

### B4 为什么不装服务、不自启、不加载驱动
- 现象：用户会问「为什么不用 Windows 服务一劳永逸」。
- 根因：服务以 SYSTEM 跑，一个 bug 的影响面从「一个进程」变成「整机」；而本项目要动的只有 `HKCU` 下四个注册表值 + 一个文本文件 `hosts`，都是用户态、可逆的。
- 做法：不装服务、不加自启、不注入进程、不加驱动、不做 TLS 中间人（`README.md:19`）。代价是每次开机要手动 `up`，这是刻意的取舍。
- 校验：注册表访问只在 `src/winsys.cpp:409-491` 与 `src/main.cpp:74-87`（后者只写 `AutoConfigURL`），键名写死 `HKEY_CURRENT_USER\...\Internet Settings`；`grep -n "RegOpenKeyExW\|RegSetValueExW" src/*.cpp` 只命中这两个文件。`[代码] README.md:13-19`

---

## C. WinINet 系统代理

### C1 四个值，`system on` 只写三个并清掉 PAC
- 现象：`system on` 之后用户原来的 PAC 配置没了。
- 根因：`HKCU\...\Internet Settings` 下 `ProxyEnable`（是否启用固定代理）、`ProxyServer`（`host:port`）、`ProxyOverride`（绕过表，`;` 分隔，`<local>` 是特殊标记）、`AutoConfigURL`（PAC，**非空时优先于 ProxyServer**）互相影响。
- 做法：`apply_wininet()` 一次写四个：`AutoConfigURL=""`、`ProxyEnable=1`、`ProxyServer=host:port`、`ProxyOverride=<bypass 或 "<local>">`（`src/winsys.cpp:694-696`），随后 `InternetSetOptionW(SETTINGS_CHANGED/REFRESH)` 通知已打开的程序（`winsys.cpp:475-479`）。要 PAC 走 `pac --apply`（只写 `AutoConfigURL`，`main.cpp:977-987`），两条路别同时用。
- 校验：`[实测]` 本机 `./svpn.exe system show`：
  ```
  ProxyEnable   0
  ProxyServer
  ProxyOverride
  AutoConfigURL
  ```
  （后三行是空串，不是「读不到」——`read_wininet()` 对不存在的值也返回空串。）

### C2 备份只留最初那一份，且 `apply` 自己会再备份一次
- 现象：反复 `system on` 后回滚，代理没回到「最初的样子」。
- 根因：每次覆盖备份的话，第二次记下的就是 svpn 自己的 `127.0.0.1:7899`，回滚等于没回滚。
- 做法：`backup_wininet()` 见文件存在即 return（`src/winsys.cpp:655-658`）；`apply_wininet()` 自己先调一次（`winsys.cpp:683`），调用方漏调也不丢原始值。缺失的值写 `null`（`winsys.cpp:666-670`）。
- 校验：`[实测]` 本机 `runtime/wininet-backup.json` = `{"ProxyEnable":0,"ProxyServer":null,"ProxyOverride":null,"AutoConfigURL":null}`。

### C3 没备份必须说「没动过」，绝不能说「已还原」
- 现象：全新机器 `svpn down` 若打印「系统代理已还原」，用户会以为 svpn 改过他的设置。
- 根因：`restore_wininet()` 备份不存在时返回 `ok=false, reason="no backup"`（`src/winsys.cpp:711-715`），**不去猜默认值**——用户原本可能是公司 PAC，瞎写比不改更糟。
- 做法：`down` 打印「系统代理没动过：没有备份（可能从没接管过）」+ 当前 `ProxyEnable`（`main.cpp:472-475`）；`system off` 打印「没有备份；当前 ProxyEnable=N」并**返回 1**（`main.cpp:945-949`）。
- 副作用要知道：`down` 只看「备份文件在不在」，不看「这次是不是我们改的」。所以只要历史上接管过，之后每次 `down` 都会**按备份覆盖**一次（`winsys.cpp:750-752` 无条件写四个值）——接管后你手改的代理会被顶回旧值。`[实测]` 无备份实例：`[!] 系统代理没动过：没有备份（可能从没接管过）` / `当前 ProxyEnable=0`

### C4 `--keep-system` 什么时候用
- 现象：`svpn down` 之后浏览器全挂。
- 根因：`down` 默认还原系统代理（`main.cpp:469-479`）；若你正要「换端口/换节点再 `up`」，还原反而打断会话。
- 做法：只在①马上要再 `up`、②你要手动接管 这两种情况用。**不要**在「今天不打算再跑」时用——`ProxyEnable` 会留在 1 指向已关端口，所有走系统代理的程序都连不上。
- 校验：`[代码] main.cpp:469`

### C5 WinHTTP 必须绕开系统代理，否则 `system on` 之后 DoH 绕回自己
- 现象：`system on` 后 `cdn --refresh` / `hosts apply` 大面积超时，`system off` 又好了。
- 根因：CDN 选边的 DoH 走 **WinHTTP**（`src/cdn.cpp:212-303`）。WinHTTP 与 WinINet 是两套栈，但 `WinHttpOpen` 默认的 `DEFAULT_PROXY` 会读**同一份** IE 设置；`system on` 后那份设置就指向 `127.0.0.1:7899`（自己）。于是 DoH 请求先回到自己的代理循环；代理没起来时（`cdn` 常在 `up` 之前跑）直接全失败。
- 做法：`WinHttpOpen(..., WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)`（`src/cdn.cpp:244`）——显式声明不读代理。代码里**没有**任何 `WINHTTP_OPTION_PROXY` 调用，绕开就靠这一处。
- 校验：`grep -n "WINHTTP_ACCESS_TYPE_NO_PROXY\|WINHTTP_OPTION_PROXY" src/*.cpp` 只命中 `cdn.cpp:244`。`[未验证]` 改成 `DEFAULT_PROXY` 的具体故障形态未实测（本轮禁止 `system on`）；原始 bug 在 Python 仓 commit `037a78f`。

---

## D. Socket 与网络

### D1 Windows 上不设 `SO_REUSEADDR` 才对
- 现象：照 Linux 习惯加 `SO_REUSEADDR`，会出现「两个进程都绑上同一端口，只有一个能收包」。
- 根因：Windows 的 `SO_REUSEADDR` 允许**另一个进程**绑同一地址端口且不检测冲突（Unix 上它主要解决 TIME_WAIT）；真正独占的语义是 `SO_EXCLUSIVEADDRUSE`。加了它等于放弃端口冲突检测。
- 做法：`listen_on()` **不加任何 reuse 选项**（`src/net.cpp:624-633`，注释写了原因）。Windows 默认（两者都不设）就是独占，被占时 `bind` 返回 10048。上层再加预检：`up` 前 `port_open()` 探一次，占了就打「已在运行（端口被占用）」并**以 0 退出**（`src/main.cpp:255-261`）——不报错，因为「已经在跑」是好消息。
- 校验：`[实测]` 隔离实例第二次 `up -b` → `[=] 已在运行（端口被占用）；改配置请先 svpn down`，`rc=0`。

### D2 10048 / 10061 / 10060 分别意味着什么
- 现象：日志里只有裸错误码，不知道查谁。
- 根因与语义：
  | 码 | 名字 | 谁给的 | 处置 |
  |---|---|---|---|
  | 10048 | `WSAEADDRINUSE` | `bind` | 端口被占。先 `svpn status` 看是不是自己；`cdn.enabled` 下 443 被 IIS/VMware/Watt Toolkit 占只打一条 WARN 继续跑（`sni.cpp:229-234`） |
  | 10061 | `WSAECONNREFUSED` | `connect` | 对端没监听/被 REJECT。典型：`down` 之后浏览器还指着 7900 |
  | 10060 | `WSAETIMEDOUT` | `connect` | SYN 被丢弃（被墙/防火墙 DROP），`probe` 判「需节点」 |
- 做法：`rep_for()` 把错误文本里的 `refused`/`10061` 映射成 SOCKS5 `REP=0x05`、`10051`/不可达映射成 `0x03`、其余 `0x04`（`src/proxy.cpp:531-540`），客户端拿到的是正确的 SOCKS 码而不是裸「连接失败」。
- 校验：`[实测]` 上面的重复 `up` 走的是**预检**分支，没走到 `bind`；`bind` 失败时的文案在 `proxy.cpp:1011-1014`（本轮未触发）。

### D3 `FD_SETSIZE` 默认 64，超了是**静默丢弃**
- 现象：改成「一个 `select` 管很多连接」后，第 65 个连接永远收不到数据，也不报错。
- 根因：MinGW 头里 `FD_SETSIZE` 默认 64（`_fd_types.h:13`），`FD_SET` 实现是 `if (fd_count < FD_SETSIZE)`（`_fd_types.h:82`）——超限的 fd 直接被忽略。
- 做法：本项目结构上碰不到，`src/` 里从没有 `#define FD_SETSIZE`：每次 `select` 的 `fd_set` 最多 2 个 socket（`relay_stream` 的 a/b，`src/net.cpp:521-525`），accept 循环只放 1 个监听 socket（`src/sni.cpp:259-261`）。若改成单线程多路复用，**必须**先 `#define FD_SETSIZE 1024` 再 `#include <winsock2.h>`。
- 校验：`[实测]` `grep -rn FD_SETSIZE src/` 无命中。

### D4 半开连接：两方向都 EOF 才收工，是取舍不是 bug
- 现象：客户端断开后中继线程还挂着。
- 根因：`relay_stream` 语义是「一侧 EOF 就 `shutdown(对侧, SD_SEND)`，**两个方向都结束**才返回」（`src/net.cpp:504-589`）；不因一侧结束就掐另一侧，否则会截断还在返回的数据。
- 做法（按调用点分开）：SOCKS5/HTTP 入口 `idle_ms = kRelayIdleMs = 1 小时`（`src/proxy.cpp:34, 535, 894`），有兜底上限；SNI 透明转发 `idle_ms = 0`，**完全不限空闲**（`src/sni.cpp:192`，注释写明「Steam 下载是长连接」），一直等到系统 TCP 超时。
- 校验：`[代码] net.cpp:521-583`、`README.md:221`（已知差异 2）。没有分钟级 idle timeout，「进程里挂着一堆长连接」是预期现象。

---

## E. 进程与文件

### E1 `Paths::resolve()` 跟 **exe 所在目录**走
- 现象：在工程根直接跑 `./svpn.exe`，之后 rebuild 报「文件被占用」；把 exe 拷到别处，配置全空。
- 根因：`GetModuleFileNameW` 取自身路径，`root = exe.parent_path()`，其余全从 `root` 派生（`src/util.cpp:130-146`）。所以在哪敲命令不影响读写哪个 `config.json`/`runtime/`；拷到新目录＝全新空实例；正在跑的 exe 就是工程根那个，镜像被 Windows 锁定，`g++ -o svpn.exe` 会失败。
- 做法：重建前先 `down`；要并行跑多实例，把 exe 拷到独立目录配自己的 `config.json`。
- 校验：`[实测]` `svpn.exe` 拷到 `wbwrk_tmp/w4/inst/` 配 19443/19444，`status` 读的就是那份配置（端口显示 19443/19444，共 9 行含横幅）。

### E2 单实例靠 pid 文件 + `pid_alive()`，删 pid 要认主
- 现象：第二个实例启动失败后，`down` 找不到在跑的实例，端口再也放不掉。
- 根因：`up` 把 pid 写进 `runtime/svpn.pid`（`main.cpp:125-129`）；`pid_alive()` 用 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess()==STILL_ACTIVE`（`main.cpp:115-123`）。第二个实例失败时若**无条件**删 pid，就把在跑实例的记录抹了。
- 做法：`remove_pid_owned(p, self)` 只在 pid 文件里的值等于 `self`（或 `self==0` 表示明确要清）时才删（`main.cpp:131-138`）。判断实例活没活要**两行交叉**：`进程 运行中 pid=N` 与 `端口 … 监听中`（`main.cpp:537-545`）。
- 校验：`[实测]` 隔离实例 `down` 后 `status` 显示「未运行 / 未监听 / 未监听」。

### E3 `up -b` 必须和后续命令写在**同一条 shell 命令**里
- 现象：一条命令看到「后台已启动 pid=N」，下一条 `status` 却「未运行」。
- 根因：`up -b` 用 `CreateProcessW(..., DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW)` 起 `_serve`（`main.cpp:277-300`）。子进程本身是分离的，但**本仓库的自动化环境在每条 shell 命令结束时回收整棵进程树**，分离进程也一起被收走。
- 做法：需要「起→查→停」就写一条命令：`./svpn.exe up -b -p 19443 && ./svpn.exe status && ./svpn.exe down`。`tests/test_cli.py` 的设计约束就来自这里——它不假设上个用例留下的进程还在。
- 校验：`[实测]` 命令一 `[+] 后台已启动 pid=30752`；命令二 `进程 未运行 / 端口 未监听`。同一条命令内则正常（pid=30544 → 运行中/监听中/监听中）。

### E4 hosts 与配置都用 `MoveFileEx` 原子替换
- 现象：写 hosts 中途断电/被杀，留下半截文件，整机 DNS 出问题。
- 根因：`hosts` 是系统级文件，非原子写入中途失败会留下不完整内容。
- 做法：先写同目录 `hosts.tmp`，再 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 原子替换；任一步失败都删 tmp 再报错（`src/winsys.cpp:270-284`）。`save_config()` 对 `config.json` 同样套路（`src/config.cpp:367-390`）。
- 校验：`[实测]` A6 自测：两次 `apply` 字节相同（幂等）、`clear` 后与原文件逐字节一致（见 A5）。

### E5 正在运行的 exe 不能被覆盖
- 现象：`build.cmd` 报 `cannot open output file svpn.exe: Permission denied`。
- 根因：Windows 加载器锁定进程的镜像文件，不给写也不给删。
- 做法：`down` 之后再 rebuild。若 `down` 说没在跑但端口仍被占（`main.cpp:456-462` 会提示），用 `netstat -ano | grep ":7899 "` 找 pid。注意 `svpn.exe` 与 `svpn-gui.exe` 是两个独立进程，关一个不影响另一个。
- 校验：`[未验证]` 本轮没做「边跑边编译」实验（会干扰并行测试），这是 Windows 加载器的既有语义。

---

## F. GUI

### F1 同一份源码，靠 `-mwindows` + 一个宏分叉
- 现象：`svpn-gui.exe` 双击不弹黑框，`svpn.exe` 双击有控制台，两者命令行行为一致。
- 根因：`build.cmd` 编两遍，唯一差别是 `-mconsole` 与 `-mwindows -DSVPN_GUI_SUBSYSTEM`（`build.cmd:23, 31`）；入口按宏切开：有宏 → `wWinMain` 直接进 `gui_main()`，无宏 → `wmain` 解析 argv、`svpn gui` 才进 GUI（`main.cpp:1189-1201`）。
- 做法：GUI 里所有动作都走 `dispatch_cli()`（`gui.cpp:143, 183`），**绝不**给 GUI 另写一套逻辑，否则两个入口必然漂移；`-mwindows` 下没有控制台，中文输出只能走 F3 的 streambuf 通道。
- 校验：`[实测]` 两个 exe 的导入表完全一致（见 G2），说明是同一份代码的两种链接形态。

### F2 Win32 没有布局引擎：坐标硬编码 + `S()` 缩放，状态框高度按行数手算
- 现象（1.1.1 真出过）：状态面板把「CDN缓存」和「hosts」挤出可视区，而这两行正是判断「下载加速有没有生效」的唯一依据，用户看到半截状态还以为没生效。
- 根因：`layout()` 全是常量乘 DPI 缩放 `S(v)`（`gui.cpp:101, 210-252`）；`status` 实际 9 行，原 `S(96)` 只够 6 行，多的落到 `ES_AUTOVSCROLL` 滚动区外。
- 做法：按**当前实际行数**手算并留余量：`status_h = S(168)`，注释写明「按 10 行留高」（`gui.cpp:214-217`）。以后往 `status` 加行**必须同步改这个高度**。第二行 7 个按钮在窗口变窄时自动折行（`gui.cpp:231-241`），最小窗口 `S(720)xS(520)`（`gui.cpp:302-307`）。
- 校验：`[实测]` `./svpn.exe status | wc -l` = 10（1 行横幅 + 9 行状态），与 `gui.cpp:214` 注释一致；GUI 会砍掉首行横幅（`gui.cpp:150-153`）。

### F3 长任务放工作线程，UI 线程绝不跑阻塞 socket
- 现象：点「端点探测」后窗口白掉、拖不动。
- 根因：Win32 消息循环单线程，`WM_COMMAND` 里跑 15 秒阻塞 socket 就没法重绘、没法响应鼠标（`probe` 并发连 9 个端点、`cdn --refresh` 要几十秒）。
- 做法：`run_async()` 起 `std::thread`，里面把 `cout`/`cerr` 换成自定义 `LogBuf`，执行完 `PostMessage(WM_APP_DONE, rc)` 回主线程（`gui.cpp:171-194`）；主线程只画界面 + 收消息。
- 校验：`[代码] gui.cpp:171-194`；`[未验证]` 交互式验证需要人点，本轮不启动窗口。

### F4 跨线程改 UI 只能用 `PostMessage`
- 现象：把日志改成 `SendMessage(WM_APP_LOG)` 后，任务跑到一半整个窗口卡死。
- 根因：`SendMessage` 是**同步**的——工作线程阻塞到 UI 线程处理完；若 UI 线程此刻正在等这个工作线程收尾，两边互等即死锁。
- 做法：工作线程一律 `PostMessageW`（异步、立即返回）：`WM_APP_LOG` 带 `new std::string` 指针，`WM_APP_DONE` 带退出码（`gui.cpp:49-50, 88-95, 309-328`）。代价是内存所有权自己管：`new` 的必须在 UI 线程 `delete`；关窗时 `g_closing` 置位后不再投递，避免往已销毁窗口发消息（`gui.cpp:63, 90`）。
- 校验：`[代码] gui.cpp:49-50, 63, 88-95, 309-328`

### F5 忙时按「停止」：必须拦下并说清楚，不能静默 return
- 现象（1.1.1 真出过）：探测跑着时点「停止」，日志只打一行「正在停止…」然后什么都不发生——按钮成了**静默死键**。
- 根因：`run_async()` 用 `g_busy.compare_exchange_strong` 做单任务闸门，忙时**直接 return**（`gui.cpp:172-173`）；「停止」也走 `run_async`，被同一个闸门吞掉，连「没执行」都没说。
- 做法：在 `WM_COMMAND` 里**先判忙、先给话、再决定**（`gui.cpp:337-347`）：`if (g_busy.load()) { append_log("[!] 上一个任务还在执行，本次「停止」未提交；等底部显示「完成」后再点。"); SetWindowTextW(g_bottom, L"任务执行中，稍后再停止"); return 0; }`。另外 `set_busy(true)` 把按钮全 `EnableWindow(FALSE)` 后单独放开「停止」（`gui.cpp:113-123`），所以它看得见、点得着、点了有回话。
- 校验：`[代码] gui.cpp:113-123, 337-347`

---

## G. 构建环境

### G1 `-static` / `-s` / `-municode` 各管什么
- 现象：拷到没装 MinGW 的机器，双击一闪就退，命令行退出码 127。
- 根因：MinGW 默认动态链接 `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`。
- 做法（`build.cmd:19`）：
  | 开关 | 作用 | 不加的后果 |
  |---|---|---|
  | `-static` | 把 gcc 运行库塞进 exe | 缺 3 个 DLL，双击立刻退 |
  | `-s` | strip 符号表 | 体积约 3MB 而非 1.7MB |
  | `-municode` | 启动代码接到 `wmainCRTStartup` | 链接失败 `undefined reference to 'WinMain'` |
  | `-mconsole` / `-mwindows` | 子系统，见 F1 | — |

  只用 MinGW（`build.cmd:5` 写死路径，找不到才回落 `PATH` 的 `g++`），不支持 MSVC。
- 校验：`[实测]` 仓库里 `build_test_a6.exe` 恰好是没加 `-static` 编的，直接跑报 `error while loading shared libraries: libstdc++-6.dll`——这就是用户看到的「双击闪退」。

### G2 「零 DLL 依赖」怎么验：`objdump -p` 看导入表
- 现象：口头说零依赖没有说服力。
- 根因：`-static` 只静态链接 **gcc 运行库**；Windows 系统 DLL 与 UCRT 仍是运行时导入。
- 做法：`objdump -p svpn.exe | grep "DLL Name"`，确认**没有** `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` 三个即达标。
- 校验（`[实测]`，两个产物导入表完全相同，23 项）：
  ```
  ADVAPI32 / CRYPT32 / DNSAPI / GDI32 / KERNEL32 / Secur32 / SHELL32 / USER32
  WINHTTP / WININET / WS2_32
  api-ms-win-crt-{convert,environment,filesystem,heap,locale,math,private,
                  runtime,stdio,string,time,utility}-l1-1-0.dll
  ```
  `api-ms-win-crt-*` 是 **UCRT**，Win10 起是系统组件（更早系统需 KB2999226）。准确说法是「只依赖系统 DLL + UCRT」，不是「一个 DLL 都不依赖」。

### G3 `-Wall -Wextra` 零警告是硬门槛
- 现象：警告越攒越多，真 bug 混在里面没人看。
- 根因：`-Wall -Wextra` 能抓未使用变量、符号/无符号比较、隐式窄化这类真问题。
- 做法：两个开关写死在 `FLAGS`（`build.cmd:19`），任何提交前必须一趟编完零警告。故意不用的参数用 `(void)var;`（如 `proxy.cpp:535`、`winsys.cpp:548`），只给测试用的函数标 `[[maybe_unused]]`（`main.cpp:48`）。
- 校验：编译输出里不应出现任何 `warning:` 行。`[未验证]` 本轮未重编（`build.cmd` 归 Lead）。

---

## H. 继承自 Python 版但没在 C++ 版对上号的条目

这些**不要**当成 C++ 版的行为答复用户：

| 条目 | 为什么不能照搬 |
|---|---|
| `.ps1` 必须带 BOM、`call python` 的 `call` 坑 | C++ 版没有 `.ps1`，也不调 `python` |
| Python hosts 的编码探测（utf-8/gbk/mbcs 依次试） | C++ 版**故意不解码**，全程按字节处理（`winsys.cpp:3-7`） |
| Python hosts 备份「只留第一份」 | C++ 版**每次写入前覆盖** `runtime/hosts.backup`（`winsys.cpp:515-522`） |
| `ERROR_SEM_TIMEOUT`(121) 的 bind 重试 3 次 | 那是 asyncio 抖动，C++ 版无 asyncio、无这段重试 |
| `uninstall.ps1` 的 Latin-1 字节技巧 | C++ 版无卸载脚本，等价物是 `down --clear-hosts` |
| Win10/Win11 代理界面差异、终端字体 | 与程序行为无关，属环境说明 |
| hosts 托管块标记文案 | 两边一致（`winsys.cpp:29-30`），改一处必须改两处 |

## 一页速查

| 症状 | 先查 |
|---|---|
| 中文变 `????` / 方块 | A2（代码页）、A3（字体，与程序无关） |
| `stats.json` 乱码、`status` 没有「上次统计」 | A4（必须是 `L"wb"`） |
| 双击 `build.cmd` 冒 `'?' is not recognized` | A6（中文行 + 空行 + CRLF） |
| 提权后什么都没发生 | B3（看 hosts 条目数，不看窗口） |
| UAC 反复弹 | B2（子进程漏了 `--no-elevate`） |
| `down` 之后浏览器全挂 | C4（`--keep-system` 用错时机） |
| `down` 说「已还原」但设置没变 | C3（备份存在 ≠ 这次改过） |
| `system on` 后 `cdn --refresh` 全超时 | C5（WinHTTP 必须 `NO_PROXY`） |
| `up` 说「已在运行」但 `status` 说未运行 | E2（pid 与端口两行交叉看） |
| 上一条命令 `up -b` 成功，下一条就没了 | E3（本环境按命令回收进程树） |
| rebuild 报 `Permission denied` | E5（先 `down`） |
| GUI 状态面板少了几行 | F2（高度按行数手算） |
| 点按钮窗口假死 / 日志不刷新 | F3、F4（阻塞跑在 UI 线程 / 用了 `SendMessage`） |
| 忙时点「停止」没反应 | F5（闸门吞掉，要拦下并提示） |
| 换机器双击闪退 | G1、G2（`-static` + 导入表） |
