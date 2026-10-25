# D3 Windows 踩坑文档（docs/WINDOWS.md）

## 1. 摘要

新建 `docs/WINDOWS.md`（294 行，A–G 七组 32 张四段式卡片 + 一页速查），每条断言都带行内代码引用或实测输出。
全部结论都回到 C++ 版源码重新验证过：Python 版里对不上的 8 条单列成第 H 节，明确标「不能照搬」。
本轮顺手复现了 1.1.1 的 `ccs=UTF-8` 落盘缺陷（源 40 字节 → 落盘 63 字节乱码 + BOM），拿到 `json.loads` 的原始报错。
只新增这 1 个文件，未改任何代码。发现 1 处跨文件文档冲突（`docs/BUILD.md:76`），只报告不改。

## 2. 做了什么

新增 `docs/WINDOWS.md`，结构如下（每节都是「现象 → 根因 → 做法 → 校验」四段，行尾带 `[实测]`/`[代码]`/`[未验证]` 证据标记）：

| 节 | 卡片 | 关键结论（全部回源码核对过） |
|---|---|---|
| A 编码（6） | A1–A6 | 两个 `-f` 开关分工（`build.cmd:19`）；`console_utf8()` 是 `dispatch()` 第一句（`util.cpp:87-90`、`main.cpp:1125`）；中文窄流的成立前提是「字面量 UTF-8 + CP65001」两条同时成立，`-mwindows` 下彻底不成立；**`ccs=UTF-8` 只能配宽字符**，`stats.json` 必须 `L"wb"`（`util.cpp:202-208`）；BOM 读跳过（`json.cpp:242`）写不写（`config.cpp:366`）；`.cmd` 三条规矩 + 现状是纯 ASCII 故结构安全 |
| B 权限（4） | B1–B4 | `CheckTokenMembership` 而非 `IsUserAnAdmin`（`winsys.cpp:765-776`）；`elevate()` 是 `ShellExecuteExW`+`runas` 的**新进程**，`--no-elevate` 是打断 UAC 递归的唯一开关（`winsys.cpp:778-810`、`main.cpp:853/872/913`）；判成败看 hosts 条目数不看窗口（`main.cpp:856/875/916`）；不装服务/自启/驱动（`README.md:19`） |
| C WinINet（5） | C1–C5 | 四个值语义 + `system on` 会清掉 PAC（`winsys.cpp:694-696`）；备份只留第一份（`winsys.cpp:655-658`）；没备份说「没动过」并返回 1（`main.cpp:472-475, 945-949`）+ **`down` 是按备份无条件覆盖**这个副作用；`--keep-system` 的适用/误用；DoH 必须 `WINHTTP_ACCESS_TYPE_NO_PROXY`（`cdn.cpp:244`） |
| D Socket（4） | D1–D4 | Windows 上不设 `SO_REUSEADDR`（也不设 EXCLUSIVE），默认即独占（`net.cpp:624-633`）；10048/10061/10060 三者语义 + `rep_for` 映射（`proxy.cpp:531-540`）；`FD_SETSIZE`=64 且 `FD_SET` **静默丢弃**（`_fd_types.h:13,82`），本项目每次最多 2 个 socket 故不触发；半开连接 SOCKS/HTTP 有 1h 兜底、SNI 传 0 完全不限（`proxy.cpp:34`、`sni.cpp:192`） |
| E 进程文件（5） | E1–E5 | `Paths::resolve()` 跟 exe 目录（`util.cpp:130-146`）；`remove_pid_owned` 认主（`main.cpp:131-138`）；`up -b` 必须同一条命令内完成（本环境按命令回收进程树，`main.cpp:297-300`）；hosts/config 走 `MoveFileEx` 原子替换（`winsys.cpp:270-284`、`config.cpp:367-390`）；运行中的 exe 锁定 |
| F GUI（5） | F1–F5 | `-mwindows`+`SVPN_GUI_SUBSYSTEM` 分叉（`build.cmd:23/31`、`main.cpp:1189-1201`）；状态框高度按行数手算 `S(168)`（`gui.cpp:214-217`）；长任务走工作线程（`gui.cpp:171-194`）；跨线程只 `PostMessage`（`gui.cpp:88-95`）；忙时「停止」先拦下并提示（`gui.cpp:337-347`，1.1.1 的静默死键已修） |
| G 构建（3） | G1–G3 | `-static`/`-s`/`-municode` 分工；`objdump -p` 验导入表（23 项，含 UCRT 说明）；`-Wall -Wextra` 零警告 + `(void)`/`[[maybe_unused]]` 手法 |
| H 对照（7 行表） | — | `.ps1` BOM、`call python`、编码探测、hosts 备份策略、121 重试、Latin-1、界面差异 —— 逐条说明为什么**不能**当成 C++ 版行为 |

另附 15 行「一页速查」表（症状 → 查哪一节）。

## 3. 验证

### 3.1 DLL 依赖（`objdump -p`，两个产物完全一致）

```
$ objdump -p svpn.exe | grep "DLL Name"
	DLL Name: ADVAPI32.dll
	DLL Name: CRYPT32.dll
	DLL Name: DNSAPI.dll
	DLL Name: GDI32.dll
	DLL Name: KERNEL32.dll
	DLL Name: api-ms-win-crt-convert-l1-1-0.dll
	DLL Name: api-ms-win-crt-environment-l1-1-0.dll
	DLL Name: api-ms-win-crt-filesystem-l1-1-0.dll
	DLL Name: api-ms-win-crt-heap-l1-1-0.dll
	DLL Name: api-ms-win-crt-locale-l1-1-0.dll
	DLL Name: api-ms-win-crt-math-l1-1-0.dll
	DLL Name: api-ms-win-crt-private-l1-1-0.dll
	DLL Name: api-ms-win-crt-runtime-l1-1-0.dll
	DLL Name: api-ms-win-crt-stdio-l1-1-0.dll
	DLL Name: api-ms-win-crt-string-l1-1-0.dll
	DLL Name: api-ms-win-crt-time-l1-1-0.dll
	DLL Name: api-ms-win-crt-utility-l1-1-0.dll
	DLL Name: Secur32.dll
	DLL Name: SHELL32.dll
	DLL Name: USER32.dll
	DLL Name: WINHTTP.dll
	DLL Name: WININET.dll
	DLL Name: WS2_32.dll
```

无 `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`。`svpn-gui.exe` 输出逐行相同。
`api-ms-win-crt-*` 是 UCRT（Win10 起系统组件），所以文档里写的是「只依赖系统 DLL + UCRT」，不是「零 DLL」。

### 3.2 `system show`（只读，未改任何全局状态）

```
$ ./svpn.exe system show
ProxyEnable   0
ProxyServer
ProxyOverride
AutoConfigURL
```

### 3.3 两条 grep

```
$ grep -n "ccs=UTF-8\|\"wb\"\|L\"wb\"" src/*.cpp
src/cdn.cpp:182:    if (_wfopen_s(&f, tmp.wstring().c_str(), L"wb") != 0 || f == nullptr) {
src/util.cpp:106:        if (_wfopen_s(&f, g_log_file.wstring().c_str(), L"a, ccs=UTF-8") == 0 && f) {
src/util.cpp:202:    // 必须是 "wb"：ccs=UTF-8 是把宽字符流转 UTF-8 的开关，配 fwrite 写窄字节串时
src/util.cpp:204:    if (_wfopen_s(&f, p.stats.wstring().c_str(), L"wb") == 0 && f) {

$ grep -n "WINHTTP_ACCESS_TYPE_NO_PROXY\|WINHTTP_OPTION_PROXY" src/*.cpp
src/cdn.cpp:244:    HINTERNET ses = WinHttpOpen(L"svpn/1.0 (cdn)", WINHTTP_ACCESS_TYPE_NO_PROXY,
```

补充：`grep -rn FD_SETSIZE src/` → 无命中；`grep -rn "wprintf\|wcout\|fwprintf" src/` → 无命中。

### 3.4 复现 1.1.1 的 `ccs=UTF-8` 缺陷（临时程序，放在 wbwrk_tmp）

同一套 MinGW g++ 16.2.0，同一份 40 字节 JSON 文本，分别用旧/新写法落盘：

```
source bytes = 40
-rw-r--r--  old_w_ccs.json  63      （旧：L"w, ccs=UTF-8" + fwrite）
-rw-r--r--  new_wb.json     40      （新：L"wb" + fwrite）

$ xxd old_w_ccs.json | head -3
00000000: efbb bfe0 a9bb e280 a0e7 94a2 e791 b0e6  ................
00000010: b5a9 e5bd a5e2 89b3 e280 bae3 80b1 e0a8  ................
$ xxd new_wb.json | head -2
00000000: 7b0a 2020 2275 7074 696d 655f 733a 20  {.  "uptime_s": 
00000010: 3130 2c0a 2020 2263 6f6e 6e65 6374 696f  10,.  "connectio
```

Python 侧原文报错：

```
old_w_ccs.json JSONDecodeError Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)
new_wb.json decode+parse OK
```

`0x7B 0x0A` 两个字节被当成一个 UTF-16 码元 `U+0A7B` 再编码成 `e0 a9 bb` —— 与 `util.cpp:202-203` 注释描述逐字吻合。
修好后本机 `runtime/stats.json` 是 223 字节合法 JSON；`./svpn.exe status | wc -l` = 10 行（含「上次统计」那行）。

### 3.5 隔离实例实测（`svpn.exe` 拷到 `wbwrk_tmp/w4/inst/`，config 用 19443/19444，不碰 7899/7900）

同一条命令内「起→查→停」正常：

```
$ ./svpn.exe up -b && ./svpn.exe status | head -4 && ./svpn.exe up -b && ./svpn.exe down
[+] 后台已启动 pid=30544  socks5://127.0.0.1:19443  http://127.0.0.1:19444
[i] CDN 模式: hosts 直指最优 IP
[i] 上游: 无（纯直连，whitelist）
rc=0
svpn 1.1.1  ·  Steam 分流/加速
进程     运行中 pid=30544
端口     127.0.0.1:19443  监听中
端口     127.0.0.1:19444  监听中
=== 再 up 一次（端口已占）===
[=] 已在运行（端口被占用）；改配置请先 svpn down
rc=0
=== down ===
[+] 已停止 pid=30544
[!] 系统代理没动过：没有备份（可能从没接管过）
    当前 ProxyEnable=0
```

跨命令「人间蒸发」（E3 的原始证据）：

```
# 命令一
[+] 后台已启动 pid=30752  socks5://127.0.0.1:19443  http://127.0.0.1:19444
# 命令二
进程     未运行
端口     127.0.0.1:19443  未监听
端口     127.0.0.1:19444  未监听
```

### 3.6 hosts 字节保真（A6 自测，用 `SVPN_HOSTS_FILE` 改道，未碰真实 hosts）

```
[caseA_utf8bom_crlf] apply ok=1 entries=2 rejected=0
[caseA_utf8bom_crlf] orig=164 applied=338 cleared=164
caseA_utf8bom_crlf clear 后与原始字节完全一致    OK
[caseB_gbk_lf] orig=81 applied=250 cleared=81
caseB_gbk_lf clear 后与原始字节完全一致          OK
```

夹具前 3 字节 `efbb bf`（BOM）、`caseB` 是 GBK 字节 `b1be bbfa`（「本草」）—— 两种编码都原样存活。

### 3.7 编码/行尾

```
docs/WINDOWS.md bytes= 29878 CRLF= 0 LF= 294 BOM= False
build.cmd       bytes= 1211 CRLF= 40 LF= 40 BOM= False   （非 ASCII 行：无）
run.cmd         bytes= 823  CRLF= 33 LF= 33 BOM= False   （非 ASCII 行：无）
```

`git status --short` 里新增只有 `?? docs/WINDOWS.md`（另有他人文件，未触碰）。

## 4. 契约变更

无。只新增 `docs/WINDOWS.md`，未改 `src/svpn.hpp`、未改任何 `.cpp`、未改 `build.cmd`/`README.md`。
文中所有行内引用都是**当前工作区**的行号（含 P4 已改的 `src/main.cpp`、Lead 已改的 `build.cmd`）。

## 5. 没做完/风险

**A. 明确标「未验证」、写进文档的 3 处**

1. `C5`：把 `WINHTTP_ACCESS_TYPE_NO_PROXY` 改回 `DEFAULT_PROXY` 后的具体故障形态没实测（本轮禁止 `system on`）。文档只写「DoH 必须直连」的机制，故障现象标 `[未验证]` 并指向 Python 仓 commit `037a78f`。
2. `E5`：正在运行的 exe 不能被覆盖 —— 没做「边跑边编译」实验（会干扰并行测试），标 `[未验证]`。
3. `F3`/`F5`：GUI 的交互行为（窗口是否假死、忙时点「停止」的实际反馈）只做了代码核对，需要人点，本轮不启动窗口。

**B. 从 Python 版继承、C++ 版无法在本轮验证的**

4. `.cmd` 的「中文行偏移吃下一行」规则（A6 根因）来自 Python 仓 P5-scripts 的三轮实测；C++ 仓两个 `.cmd` 都是纯 ASCII，**复现不出来也不需要**，文档已写成「本项目结构上安全」而不是「我们踩过」。
5. `hosts` 提权路径（`is_admin` → `elevate` → 子进程 `--no-elevate`）**没有真跑**：会弹 UAC 且改真实 hosts，属本轮禁区。只验证了 `elevate()` 的代码形状与调用点，以及无备份时 `restore_wininet` 的拒绝语义（3.5 实测）。
6. `bind` 返回 10048 的分支**未触发**（被 `up` 的端口预检挡住了），443 被占只打 WARN 也是读代码得出。文档里已注明「本轮未触发」。
7. `SO_REUSEADDR` / `SO_EXCLUSIVEADDRUSE` 的 Windows 语义差异是平台行为，非本项目实测；文档只断言「本项目两者都不设，默认即独占」这一可由代码+实测（重复 `up` 被预检拦下）支撑的部分。

**C. 跨文件问题（只报告，未改）**

8. `docs/BUILD.md:76` 的表格仍写着「`-mconsole` … 必须保留控制台，**不要**加 `-mwindows`（那会把标准输出吞掉）」。1.1.1 已经是双产物（`svpn-gui.exe` 用 `-mwindows -DSVN_GUI_SUBSYSTEM`，`build.cmd:31`），这句会误导读者。建议改成「控制台产物用 `-mconsole`；GUI 产物用 `-mwindows`，其输出由 `gui.cpp` 的 `streambuf` 接管」。`BUILD.md` 属只读参考，我没动。
9. 仓库里 `build_test_a6.exe` 是**没加 `-static`** 编的（直接跑报 `libstdc++-6.dll` 找不到），且跑出 4 条 FAIL：其中 1 条是我少放夹具（`caseC_no_trailing_nl.bin` 为空，已排除），另外 3 条是 `sanitize: 6 个脏条目被拒` / `全非法 → ok=false` / `文案与 Python 一致` —— 与三套官方测试（`test_config` / `test_sni_parse` / `test_cli.py`）无关，疑似二进制与 `tests/test_winsys.cpp` 版本不同步。不在我的文件范围，未深究，建议 Lead 决定是否重编或移出仓库。

（`_crew/tmp_a6/` 的夹具目录已被 `.gitignore:36` 的 `_crew/tmp_*/` 覆盖，不进版本库，无卫生问题。）
