# 打包与发布流程（svpn 1.1.1 · 双产物）

状态：**终态（可照着做）**

> 适用版本：`svpn-cpp` 1.1.1（`svpn.exe` + `svpn-gui.exe`）。
> 与 1.1.0 的**唯一结构差异**：1.1.0 是单 exe、包内 7 文件、`SHA256SUMS.txt` 覆盖 6 项；
> 1.1.1 是双 exe、包内 9 文件、覆盖 8 项。下文所有条目按双产物写，**不要拿 1.1.0 的报告照抄**。
>
> 本文每个数字都标了来源：`实测` 表示本机命令输出粘在文中；`源码` 表示 `src/*.cpp` 行号；
> `SPEC` 表示 `_crew/SPEC-1.1.1-release.md`；`R3` 表示 `_crew/reports/R3-package.md`。
> 二进制指纹是**易变值**：文中快照仅供核对命令格式，发版以 `dist/RELEASE.txt` 为准。

---

## 1. 前置检查（顺序不可省）

### 1.1 端口必须全空闲——这是「门禁恒真」的唯一防线

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
netstat -ano | grep -E ":(7899|7900) "
netstat -ano | grep -E ":443 .*LISTENING"
```

期望：**三条都无输出**（`grep` 返回 1）。

实测（2026-10-25 12:52，本机）：

```
$ netstat -ano | grep -E ":(7899|7900) "
$ echo $?
1
$ netstat -ano | grep -E ":443 .*LISTENING"
（空）
```

**为什么必须先查**：`up` 的「已在运行」判定是**端口探测**，不是进程探测（`src/main.cpp:255-261`）：

```cpp
if (port_open(cfg.listen_host, cfg.listen_port) ||
    (cfg.http_port && port_open(cfg.listen_host, cfg.http_port))) {
    std::cout << "[=] 已在运行（端口被占用）；改配置请先 svpn down\n";
    return 0;
}
```

只要有**任何**进程占着 `config.json` 里的 `http_port`（默认 7900），任何 `up` 都会立刻打印
「已在运行」并返回 `rc=0`——哪怕被测的 exe 是坏的。发布门禁第 2 条（「第二次 `up` 必须被拒」）
于是**恒为真**。1.1.0 发版窗口内真出过这件事：残留实例 `PID 9008` 占着 7899+7900，
R2 §「环境风险：残留实例会让门禁恒真」与总日志 §6.4.7 都记了这一条。

443 单独查的原因：`cdn.sni_port` 实际必须是 443（hosts 只能表达 IP，客户端连的就是
`127.0.0.1:443`，总日志 §6.2.3）。443 被占时 CDN 的 SNI 转发线程只会打一条
`WARN SNI 转发未启动（端口被占用？）`（`src/main.cpp:416`），主流程照样 rc=0——**静默降级**。

发现占用后的处置：

```bash
tasklist | grep -i svpn                    # 先认人
netstat -ano | grep -E ":(7899|7900) "     # 拿 PID（最后一列）
```

确认是自己残留的 `svpn.exe` 才 `svpn.exe down`；不是自己的进程**不要杀**，改端口或等对方下线。

### 1.2 工具链在位

```bash
D:/LE/AI/tools/mingw64/bin/g++.exe --version | head -1
gh --version | head -1
command -v sha256sum unzip md5sum
```

实测：

```
g++.exe (x86_64-posix-seh-rev1, Built by MinGW-Builds project) 16.2.0
gh version 2.102.0 (2025-...)
/usr/bin/sha256sum
/usr/bin/unzip
/usr/bin/md5sum
```

**没有 `zip` 命令**（实测 `command -v zip` → 空）。打包用 7-Zip 或 Windows 自带 bsdtar，见 §9。

### 1.3 工作区状态

```bash
git status --short
git check-ignore -v svpn.exe svpn-gui.exe dist/svpn.exe tools/gui1.bmp
```

期望 4 条全部命中忽略规则（实测）：

```
.gitignore:12:/svpn-gui.exe	svpn-gui.exe
.gitignore:11:/svpn.exe	svpn.exe
.gitignore:40:dist/	dist/svpn.exe
.gitignore:33:tools/*.bmp	tools/gui1.bmp
```

`svpn-gui.exe` 的忽略规则是 1.1.1 才加的（1.1.0 只有 `/svpn.exe`）。**少这一条，双产物发布会把
1.7MB 二进制提进源码树**——1.1.0 已经因为 `dist/` 入库返工过一次（R3 §4）。

---

## 2. 构建

### 2.1 一条命令出两个产物

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
cmd //c build.cmd
```

`build.cmd` 用同一份 `src\*.cpp` 编两遍（`build.cmd:23` 与 `build.cmd:31`）：

| 产物 | 关键开关 | 作用 |
|---|---|---|
| `svpn.exe` | `-mconsole` | 控制台子系统：CLI 全部子命令 + `svpn gui`（`gui.cpp` 同进程开窗） |
| `svpn-gui.exe` | `-mwindows -DSVPN_GUI_SUBSYSTEM` | 窗口子系统：双击不闪黑框；`main` 直接进 Win32 消息循环 |

两个产物共用 `FLAGS`（`build.cmd:19`）：

```
-std=c++20 -O2 -Wall -Wextra -municode -static -s -finput-charset=UTF-8 -fexec-charset=UTF-8
```

各开关的必要性（细节见 `docs/BUILD.md` §3/§4）：

| 开关 | 少了会怎样 |
|---|---|
| `-static` | 动态链 libstdc++/libgcc/winpthread，没装 MinGW 的机器双击即退（退出码 127） |
| `-s` | strip 符号表；体积从约 3MB 降到约 1.7MB |
| `-municode` | 入口是 `wmain`（`src/main.cpp`），不加直接链接失败：`undefined reference to 'WinMain'` |
| `-finput-charset/-fexec-charset=UTF-8` | 源码含大量中文，CP936 环境下编译报 `Illegal byte sequence` 或中文提示乱码 |
| `-Wall -Wextra` | 见 §2.2，这是硬门槛，不是建议 |

期望输出尾部：

```
[i] compiler: D:\LE\AI\tools\mingw64\bin\g++.exe
[1/2] svpn.exe  (console, CLI + svpn gui)
[2/2] svpn-gui.exe  (windowed, no console flash)
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn.exe  <字节数> bytes
[+] built D:\LE\AI\wbwrk\svpn-cpp\svpn-gui.exe  <字节数> bytes
```

### 2.2 零警告是硬门槛

只编一次，把输出存下来再判（重跑一次 `build.cmd` 只为 grep 会白编一遍）：

```bash
cmd //c build.cmd 2>&1 | tee /d/LE/AI/wbwrk/wbwrk_tmp/build.log | tail -5
grep -ci "warning:" /d/LE/AI/wbwrk/wbwrk_tmp/build.log
```

期望：`tail` 看到 `[+] built …` 两行；`grep -ci` 输出 **0**。有一条 `warning:` 就停下来修，
不许「反正是警告」。

> 本轮实测记录：总日志 §5.4「源码干净重建，`-Wall -Wextra` 零警告，两个产物都产出」。
> 本条本报告未重跑（重编会覆盖 Lead 正在用于打包的 exe），构建归 Lead。

### 2.3 零 DLL 依赖验证

```bash
D:/LE/AI/tools/mingw64/bin/objdump.exe -p svpn.exe | grep -i "DLL Name"
D:/LE/AI/tools/mingw64/bin/objdump.exe -p svpn-gui.exe | grep -i "DLL Name"
```

实测 `svpn.exe`（23 条，全部系统 DLL）：

```
	DLL Name: ADVAPI32.dll
	DLL Name: CRYPT32.dll
	DLL Name: DNSAPI.dll
	DLL Name: GDI32.dll
	DLL Name: KERNEL32.dll
	DLL Name: api-ms-win-crt-convert-l1-1-0.dll
	...（api-ms-win-crt-* 共 11 条，属 Windows 10/11 自带 UCRT）
	DLL Name: Secur32.dll
	DLL Name: SHELL32.dll
	DLL Name: USER32.dll
	DLL Name: WINHTTP.dll
	DLL Name: WININET.dll
	DLL Name: WS2_32.dll
```

判据不是「有没有 DLL」，而是**有没有非系统 DLL**：

```bash
D:/LE/AI/tools/mingw64/bin/objdump.exe -p svpn.exe | grep -Ei "libstdc|libgcc|winpthread|libwinpthread"
```

期望无输出。命中任意一条 = `-static` 没生效，产物不可发布。

`svpn-gui.exe` 实测**同样是 23 条、集合完全一致**（同一份源码、同一套 `LIBS`；
`-lcomctl32` 虽在链接列表里，但 GUI 只用 EDIT/BUTTON/STATIC 标准控件，没有 comctl32 的导入，
链接器自然不会写进导入表）。非系统 DLL 检查同样是 0 命中。

> 数一下更保险：`objdump -p svpn.exe | grep -ci "DLL Name"` → 23；`grep -Eic "libstdc|libgcc|winpthread"` → 0。

### 2.4 版本号确认

```bash
./svpn.exe -v
```

实测：`1.1.1`，rc=0（版本号来源 `src/svpn.hpp` 的 `kVersion`）。

> `svpn-gui.exe` 是窗口程序，**不要**用 `-v` 去测它：实测执行 `./svpn-gui.exe -v` 会直接开窗并挂住，
> 只能强制结束。GUI 的版本看窗口标题（`svpn 1.1.1 · Steam 分流/加速`）或 `svpn.exe -v`。

---

## 3. 全量测试（三套，缺一不可）

### 3.1 环境纪律（先做，否则假红）

```bash
unset NODE_OPTIONS http_proxy https_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY all_proxy
```

原因：`test_cli.py` 会起真实子进程并按 `http_proxy` 之类的环境变量判断网络行为；
`NODE_OPTIONS` 会让 `doctor`/`pac` 相关的 node 校验行为漂移。带代理变量跑出的红不是代码的红。

同时确认：**同一时刻只允许一个 `test_cli.py` 在跑**（它自己 `pick_free_port()` 挑端口，
但并发两份会互相抢临时目录与真实 hosts 备份）。

### 3.2 三套命令与期望值

```bash
cd /d/LE/AI/wbwrk/svpn-cpp

./tests/test_config.exe
./tests/test_sni_parse.exe
python tests/test_cli.py
```

| 套件 | 期望 | 来源 |
|---|---|---|
| `tests/test_config.exe` | `[+] PASS 178 / FAIL 0（共 178 项）`，rc=0 | 实测（本次复跑） |
| `tests/test_sni_parse.exe` | `[+] PASS 27 / FAIL 0（共 27 项）`，rc=0 | 实测（本次复跑） |
| `python tests/test_cli.py` | `PASS 113+ / FAIL 0 / SKIP 0` | SPEC §5（Lead 本轮复跑 113/0/0） |

实测原文（本次复跑，两套 C++ 用例）：

```
$ ./tests/test_config.exe | tail -2
ok    顶层数组 → 默认（不报错，Python 同）

[+] PASS 178 / FAIL 0（共 178 项）
rc=0

$ ./tests/test_sni_parse.exe | tail -2
  （守卫页大小 = 4096 字节；任何越界读都会被捕获）

[+] PASS 27 / FAIL 0（共 27 项）
rc=0
```

`test_cli.py` 的判定口径（三条都要满足）：

1. `FAIL 0`；
2. `SKIP 0`——**出现 SKIP 视为异常**，不要当通过。总日志 §3.3 记过一次 `111 PASS / 1 SKIP`，
   §6.4.2 把它列为待复核项；SPEC §5 的 113/0/0 是本轮基准；
3. 用例数是**下限**：`113+` 表示只许多不许少（本轮新增了 `upstream_kind` 相关用例，
   见 §5 的 `effective.json`）。

> 本条本报告未实跑：`tests/test_cli.py` 属 W1 所有且正在改动，并发跑会与 Lead 的发版复跑撞车。

### 3.3 测试 exe 的重建命令（源码变了才需要）

```bash
D:/LE/AI/tools/mingw64/bin/g++.exe -std=c++20 -O2 -static -I src -o tests/test_config.exe \
  tests/test_config.cpp src/config.cpp src/rules.cpp src/json.cpp src/util.cpp src/rules_data.cpp -lws2_32

D:/LE/AI/tools/mingw64/bin/g++.exe -std=c++20 -O2 -static -I src -o tests/test_sni_parse.exe \
  tests/test_sni_parse.cpp src/tls.cpp src/net.cpp src/util.cpp src/json.cpp \
  -lws2_32 -lcrypt32 -lsecur32
```

（命令来源 T1-tests.md §「编译命令与结果」，两个 exe 均 `-static`，换机器直接跑不缺 DLL。）

---

## 4. 发布门禁（三条，必须同一个 shell 会话里连跑）

```bash
cd /d/LE/AI/wbwrk/svpn-cpp

# 0) 前置：端口全空闲（§1.1）
netstat -ano | grep -E ":(7899|7900) " ; echo "should be empty"

# 1) 带覆盖项启动
./svpn.exe up -b -p 17899 -m blacklist ; echo "GATE1 rc=$?"
sleep 1
cat runtime/svpn.pid ; echo
cat runtime/effective.json
md5sum runtime/effective.json

# 2) 第二次 up 必须被拒，且 pid / effective.json 一个字节都不许变
./svpn.exe up -b ; echo "GATE2 rc=$?"
cat runtime/svpn.pid ; echo
md5sum runtime/effective.json
tasklist | grep -ci svpn.exe

# 3) down 后三个端口全部 10061（拒绝）
./svpn.exe down ; echo "GATE3 rc=$?"
ls runtime/svpn.pid runtime/effective.json 2>&1
netstat -ano | grep -E ":(7899|7900|17899) "
```

期望（R2 §「在 e634fe15 上复现 V1 全套检查：全绿」原文口径）：

```
GATE1  [+] 后台已启动 pid=<A>  socks5://127.0.0.1:17899  http://127.0.0.1:7900   rc=0
       pid 文件 = <A>
       effective.json = { listen_port:17899, http_port:7900, mode:"blacklist", ... }
GATE2  [=] 已在运行（端口被占用）；改配置请先 svpn down                            rc=0
       pid 文件 = <A>（未变）
       effective.json md5 = 与 GATE1 逐字相同
       本目录 svpn.exe 进程数 = 1
GATE3  [+] 已停止 pid=<A>                                                          rc=0
       svpn.pid / effective.json 均不存在
       netstat：17899 / 7899 / 7900 全部无监听
```

三个判据的「为什么」：

| 判据 | 挡的是什么 |
|---|---|
| GATE2 `rc=0` 且输出「已在运行」 | 回归 V1 ⑫：早期版本第二次 `up` 会拉起第二个实例、覆盖 pid、前一个变孤儿（V1 §7.5） |
| GATE2 后 `pid` 与 `effective.json` 的 md5 不变 | 「已经在跑」必须是**只读判定**，不许顺手改写运行态 |
| GATE3 后端口探测全 `10061` | `down` 是 `TerminateProcess`，端口释放有约 100ms 异步窗口（V1 §「回归」）；探测要留这个窗口 |

**单会话是硬要求**：把三条拆成多次 shell 调用会得到假的
`[=] 没有在运行的实例`（后台 `_serve` 随会话被回收，pid 文件却残留），
看起来完全像 P1 复发。V1 §9.1 与 R2 §「一个实验陷阱」独立踩到同一现象。

**端口选择**：`-p 17899` 是 R2/V1 用的既有约定。若本机 7900 被占，门禁恒真（§1.1），
此时改用临时 `config.json` 把 `http_port` 也改成 17900（R2 的做法），
**不要**为了跑门禁去杀别人的进程。

---

## 5. 打包清单（1.1.1 双产物，逐项）

目标目录：`dist/`（已在 `.gitignore` 中，**不入库**，靠 tag + Release 附件追溯）。

| # | 文件 | 来源 | 1.1.1 相对 1.1.0 的变化 |
|---|---|---|---|
| 1 | `svpn.exe` | 根目录刚构建的产物 | 内容变了（版本 1.1.1） |
| 2 | `svpn-gui.exe` | 根目录刚构建的产物 | **新增** |
| 3 | `README.md` | 仓库根 `README.md` | 更新（GUI / 1.1.1） |
| 4 | `README.en.md` | 仓库根 `README.en.md` | **新增**（W5） |
| 5 | `LICENSE` | 仓库根 `LICENSE` | 不变（MIT，1,063 B） |
| 6 | `run.cmd` | **`dist/run.cmd` 那份，不是仓库根那份** | 见下面的坑 |
| 7 | `config.example.json` | 仓库根 | 不变（2,084 B） |
| 8 | `RELEASE.txt` | 按 §7 生成 | 重写（双产物） |
| 9 | `SHA256SUMS.txt` | 按 §8 生成 | 6 项 → 8 项 |

包内 9 个文件，`SHA256SUMS.txt` 覆盖其中 8 个（除自身）。

### 5.1 `run.cmd` 的坑：仓库根和发布包里的**不是同一份**

实测（`diff <(cat run.cmd) <(cat dist/run.cmd)` 有差异，rc=1）：

| 位置 | 大小 | 行为 |
|---|---|---|
| 仓库根 `run.cmd` | 823 B | 开发版：`chcp` + `init` + `up -b` + `status` + 失败分支 `:failed` + 提示 system on/down |
| `dist/run.cmd` | 250 B | 发布版：先检查 `svpn.exe` 是否在同目录，再 `init` + `up -b` + `status` + `pause` |

发布包要的是 `dist/` 那份——它多一条「`svpn.exe` 不在旁边就报错退出」的自检，
这正是双击用户最常见的失败场景。**不要 `cp run.cmd dist/`**，那会用 823B 的开发版覆盖掉。

### 5.2 组包步骤

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
mkdir -p dist
cp svpn.exe svpn-gui.exe README.md README.en.md LICENSE config.example.json dist/
# run.cmd 已经在 dist/ 里，确认它是 250 B 那份：
stat -c "%n %s" dist/run.cmd      # 期望 250
```

`RELEASE.txt`（§7）**先**写，`SHA256SUMS.txt`（§8）**后**生成——因为前者要被后者覆盖。

### 5.3 泄漏扫描（必须零命中）

包内绝不允许出现本机运行态数据：`config.json`（含本机端口/模式）、`runtime/`（日志、pid、
hosts 备份、wininet 备份）、`*.o`、`tests/*.exe`、`effective.json`、`hosts.backup`。

```bash
ls dist/ | grep -Ei "config\.json|runtime|\.o$|test_.*\.exe|effective\.json|hosts\.backup|\.log$|\.pid$"
```

期望无输出。1.1.0 打包时还额外要求 `_crew/tmp_*/` 不得入库（R3 §1「排除项」）。

---

## 6. 只读位（两个 exe 都要设）

```bash
cmd //c "attrib +R dist\svpn.exe dist\svpn-gui.exe"
cmd //c "attrib dist\svpn.exe dist\svpn-gui.exe"
```

期望输出带 `R`：

```
A    R               D:\LE\AI\wbwrk\svpn-cpp\dist\svpn.exe
A    R               D:\LE\AI\wbwrk\svpn-cpp\dist\svpn-gui.exe
```

**为什么必须做**：1.1.0 出过这件事——`attrib +R` 设过，但之后产物改名 + 重新生成，
属性没跟着恢复，`dist/svpn.exe` 只剩 `A`（R3 §8.1）。后果是任何一次 `build.cmd`
或覆盖式 `cp` 都能直接冲掉发布物，「不可变副本」的保证失效。
本机现状（实测）：`dist\svpn.exe` 已带 `R`（`a84dd7a` 修的就是这条）。

顺序要求：**先复制、再设只读、最后打包**。设了只读再想覆盖会失败（这正是不想发生的事）。

---

## 7. `RELEASE.txt` 怎么写

模板（`<...>` 是打包时填的实测值；结构照 1.1.0 的 `dist/RELEASE.txt`，内容按双产物改写）：

```
svpn 1.1.1-cpp  ·  发布说明
==================================================

版本            1.1.1-cpp
可执行文件      svpn.exe        （控制台：CLI 全部子命令 + `svpn gui`）
                svpn-gui.exe    （窗口：双击即用，无控制台黑框）
MD5             <svpn.exe 的 md5>          svpn.exe
                <svpn-gui.exe 的 md5>      svpn-gui.exe
大小            <svpn.exe 字节> B / <svpn-gui.exe 字节> B
构建时间        <YYYY-MM-DD HH:MM:SS> (+0800)
编译器          g++ 16.2.0
                (MinGW-w64 x86_64-posix-seh-rev1, MinGW-Builds project)
目标平台        x86_64-w64-mingw32 / Windows x64
构建脚本        build.cmd

本 exe 由下列源码用 `build.cmd` 构建。


源码指纹（15 个文件，MD5）
--------------------------------------------------
<md5>  src/<name>.cpp        <字节> B
...（15 行，按文件名排序）


行为基准
--------------------------------------------------
Python 版 svpn，commit 037a78f
https://github.com/zhe-du/svpn/commit/037a78f35df8ed32b98d6e35ee66aa054b798fe6

本 exe 为该实现的原生重写：单文件、零 DLL 依赖，
TLS 走 SChannel，转发用线程 + select。


验证这份产物
--------------------------------------------------
1) 校验哈希
     certutil -hashfile svpn.exe MD5
   应输出 <svpn.exe 的 md5>
     certutil -hashfile svpn-gui.exe MD5
   应输出 <svpn-gui.exe 的 md5>

2) 打印版本
     svpn.exe -v
   应输出 1.1.1

3) 跑离线用例
     python tests\test_cli.py
   本产物发布时实测 PASS 113+ / FAIL 0 / SKIP 0。
   用例集仍在增补，实际数量以仓库内 tests/ 为准。


包内文件
--------------------------------------------------
  svpn.exe             主程序（控制台）
  svpn-gui.exe         图形界面（双击即用）
  README.md            使用说明（中文）
  README.en.md         使用说明（英文）
  LICENSE              MIT
  run.cmd              启动脚本
  config.example.json  配置模板
  SHA256SUMS.txt       上述 8 个文件的 SHA256（sha256sum -c 可直接校验）
  RELEASE.txt          本文件


注意
--------------------------------------------------
1. 本包不含 config.json。首次运行会自动生成：svpn.exe init
2. 本包不含 runtime/。该目录由程序运行时创建，内含本机代理备份、
   日志与探测缓存，不属于发布内容。
3. run.cmd 与 svpn.exe 同目录，直接双击即可。
4. config.example.json 中 upstream.host 为 NODE_HOST 占位符，
   上游地址与账号需自行填写。本包不含任何真实凭据。
5. 已知问题（不影响转发）
   - `probe` 的「直连可用」只代表竞速里至少一个 IP 成功，不是成功率。
   - `cdn.sni_port` 实际必须是 443（hosts 只能表达 IP）。
   - 只有两个 CDN 域名做真实吞吐实测，其余只做 HEAD（显示 `-` 是设计如此）。
   - `node scan` 只扫固定 16 个本机端口；上游只认 socks5:// 与 http://。
```

### 7.1 源码指纹的「15」怎么来的（别硬编码）

```bash
ls src/*.cpp src/*.hpp | wc -l
```

实测：`15`（14 个 `.cpp` + 1 个 `svpn.hpp`；1.1.0 是 14 个，1.1.1 新增 `src/gui.cpp`）。

生成指纹行（实测可用）：

```bash
for f in src/*.cpp src/*.hpp; do
  printf "%s  %-20s %s B\n" "$(md5sum "$f" | cut -d' ' -f1)" "$f" "$(stat -c %s "$f")"
done
```

实测前 4 行：

```
eabe35a7df866cfa16f6539d8fb4bb7c  src/cdn.cpp          30797 B
5c7aea0555ee6e2832655d31918d6f68  src/config.cpp       17466 B
d983d86b43266b7adb24197719928460  src/gui.cpp          18000 B
acc699c0f9896dd2c62aad9073dcb6b9  src/json.cpp         11490 B
```

口径提醒：1.1.0 的 `RELEASE.txt` 用了千分位（`49,232 B`），上面命令输出的是原始字节（`49232 B`）。
两种都行，但**整份文件统一一种**；`RELEASE.txt` 里写的「15 个文件」必须和 `wc -l` 的输出一致。

### 7.2 三条易错点

1. **不要写死 MD5**。同一份源码在不同构建窗口产出的字节可能因时间戳等元数据不同而变化
   （本机 12:34 与 12:46 两次构建，`svpn.exe` 的 md5 分别是 `21f07511…` 与 `d4c00335…`）。
   一律现算现填。
2. **`svpn.exe` 与 `svpn-gui.exe` 都要列**。1.1.0 的单产物模板只有一行 MD5，直接改名会漏掉 GUI。
3. **已知问题只能写源码/实测支持的**。1.1.1 已修掉「`status` 节点行不读 `effective.json`」这一条
   （`src/main.cpp:522-531` 现在读 `upstream_kind`），不要再从 1.1.0 的 `RELEASE.txt` 抄过来。

---

## 8. `SHA256SUMS.txt` 怎么写

顺序：**先有 `RELEASE.txt`，再生成校验和**（它要被覆盖）。

```bash
cd /d/LE/AI/wbwrk/svpn-cpp/dist
sha256sum --text \
  svpn.exe svpn-gui.exe README.md README.en.md LICENSE run.cmd config.example.json RELEASE.txt \
  > SHA256SUMS.txt
```

- `--text` 是必须的：本机 `sha256sum` 默认按二进制模式输出 `hash *file`（星号），
  1.1.0 的既有文件是两空格格式 `hash  file`，用 `--text` 才一致（实测两种输出见下）。
- 覆盖**包内除自身之外的全部文件**：1.1.0 是 6 项（exe/run.cmd/README/LICENSE/config.example/RELEASE），
  1.1.1 是 **8 项**（多 `svpn-gui.exe` 与 `README.en.md`）。数量按 §5 的表算，不要沿用 6。

实测：

```
$ sha256sum run.cmd | cat -A
ca2a940e1dd9b2be40e7d6d2064d467b17c2d8836afe05d967e65cc79690adfe *run.cmd$
$ sha256sum --text run.cmd | cat -A
ca2a940e1dd9b2be40e7d6d2064d467b17c2d8836afe05d967e65cc79690adfe  run.cmd$
```

自校验（必须全 OK，且要有 8 行）：

```bash
sha256sum -c SHA256SUMS.txt
wc -l SHA256SUMS.txt
```

1.1.0 实测（6 项，格式可直接对照）：

```
$ sha256sum -c SHA256SUMS.txt
svpn.exe: OK
run.cmd: OK
README.md: OK
LICENSE: OK
config.example.json: OK
RELEASE.txt: OK
```

---

## 9. zip 命名与结构

命名：`svpn-1.1.1-win64.zip`，放在 `dist/`。
结构：**顶层一个目录** `svpn-1.1.1-win64/`，解开就是解压即用的一层（与 1.1.0 一致，
R3 §1 `unzip -l` 原文每个条目都带 `svpn-1.1.0-win64/` 前缀）。

```bash
cd /d/LE/AI/wbwrk/svpn-cpp
rm -rf /d/LE/AI/wbwrk/wbwrk_tmp/pkg && mkdir -p /d/LE/AI/wbwrk/wbwrk_tmp/pkg/svpn-1.1.1-win64
cp dist/svpn.exe dist/svpn-gui.exe dist/README.md dist/README.en.md dist/LICENSE \
   dist/run.cmd dist/config.example.json dist/RELEASE.txt dist/SHA256SUMS.txt \
   /d/LE/AI/wbwrk/wbwrk_tmp/pkg/svpn-1.1.1-win64/
cd /d/LE/AI/wbwrk/wbwrk_tmp/pkg
"/c/Program Files/7-Zip/7z.exe" a -tzip /d/LE/AI/wbwrk/svpn-cpp/dist/svpn-1.1.1-win64.zip \
   svpn-1.1.1-win64
```

或不用 7-Zip（Windows 自带 bsdtar）：

```bash
/c/Windows/System32/tar.exe --format zip -c -f svpn-1.1.1-win64.zip svpn-1.1.1-win64
```

**实测两个坑**：

1. `tar -a -c -f x.zip dir` **不会**产出 zip：Windows 的 bsdtar 3.8.9 忽略 `-a` 的扩展名推断，
   实际写出 10,240 B 的 pax tar，`unzip -l` 直接报
   `End-of-central-directory signature not found`。必须显式 `--format zip`。
2. 本机**没有 `zip` 命令**（`command -v zip` 为空）。写文档别让人跑 `zip -r`。

打包后必查：

```bash
cd /d/LE/AI/wbwrk/svpn-cpp/dist
unzip -t svpn-1.1.1-win64.zip            # 期望 No errors detected
unzip -l svpn-1.1.1-win64.zip            # 期望 9 个文件 + 1 个目录，全带 svpn-1.1.1-win64/ 前缀
unzip -p svpn-1.1.1-win64.zip svpn-1.1.1-win64/svpn.exe | md5sum
md5sum svpn.exe                          # 两条必须一致（包内外同一批字节）
md5sum svpn-1.1.1-win64.zip              # 记录到本次发版日志
```

1.1.0 的对照值（证明这套检查能真的抓到问题）：`svpn-1.1.0-win64.zip` **699,901 B**、
md5 `0704cb0841a2402544bdfff065393e33`、包内 7 个文件全带 `svpn-1.1.0-win64/` 前缀
（实测 `unzip -l`；R3 §1 记的 699,771 B / `2146fb16…` 是 `11:02` 重打之前的快照，已作废）。

---

## 10. 提交、打标、推送、Release

### 10.1 三个 commit（顺序固定）

```bash
cd /d/LE/AI/wbwrk/svpn-cpp

# 1) 源码（含 tests 与 build.cmd、.gitattributes 等构建卫生）
git add src/ tests/ build.cmd run.cmd .gitattributes .gitignore
git commit -m "feat(gui): 1.1.1 Win32 原生 GUI，双产物 svpn.exe + svpn-gui.exe"

# 2) 文档
git add README.md README.en.md docs/ _crew/
git commit -m "docs: 补 ARCHITECTURE / WINDOWS / README.en，收录本轮报告"

# 3) （无需提交发布物：dist/ 在 .gitignore 里，靠 tag + Release 附件追溯）
git status --short        # 期望：只剩未跟踪的 dist/（被忽略，不显示）
```

`dist/` 不入库是既定决定（R3 §4 踩过两次，`.gitignore:40` 生效）。
所以**发布物的可追溯性只靠三样**：tag message 里的 exe 指纹 + `RELEASE.txt` 的源码 MD5 + GitHub Release 附件。

### 10.2 tag

```bash
git tag -a v1.1.1 -F - <<'EOF'
svpn 1.1.1 (Windows x64 · 双产物 · GUI)

【产物锚点】只钉不变的东西，包指纹见 dist/RELEASE.txt 与 Release 附件
  svpn.exe      md5 <...>  sha256 <...>  <字节> B
  svpn-gui.exe  md5 <...>  sha256 <...>  <字节> B
  源码指纹      15 个文件，见 dist/RELEASE.txt

【构建】g++ 16.2.0 (MinGW-w64 x86_64-posix-seh-rev1), -O2 -static -s
【用例】test_cli 113+ / test_config 178 / test_sni_parse 27 全绿
【新增】Win32 原生 GUI（svpn-gui.exe）；effective.json 补 upstream_kind
【审计】_crew/reports/V1-cli-audit.md
EOF
```

设计原则照 1.1.0：tag 里**不写 zip 的 md5**（重打包就过期），只钉 exe 与源码指纹（R3 §3.2）。

### 10.3 推送与 Release

```bash
git push origin main
git push origin v1.1.1
gh release create v1.1.1 \
  dist/svpn-1.1.1-win64.zip \
  --title "svpn 1.1.1 · Windows x64（GUI）" \
  --notes-file dist/RELEASE.txt
```

检查清单：

- `git rev-parse v1.1.1^{commit}` 与 `git rev-parse HEAD` 相同；
- `gh release view v1.1.1` 能看到 `svpn-1.1.1-win64.zip` 附件且大小与本地一致；
- Release 正文里的 md5 与本地 `md5sum dist/svpn.exe dist/svpn-gui.exe` 一致。

---

## 11. 本轮踩过的坑（checklist，逐条打勾）

| # | 规则 | 怎么验 | 破了会怎样 |
|---|---|---|---|
| 1 | `.cmd` 必须 **CRLF**（工作区） | `git ls-files --eol build.cmd` → `i/lf  w/crlf  attr/text eol=crlf`（实测） | 含 `goto`/中文的 `.cmd` 用 LF 会解析错乱 |
| 2 | `.cmd` 注释只能是 ASCII | 人工看；中文行后必须留一个空行 | CP936 行偏移，`goto` 跳到错行 |
| 3 | 中文 `.md` 必须 **UTF-8 无 BOM** | `head -c 3 x.md \| od -An -tx1`，不能是 `ef bb bf` | GitHub/编辑器显示乱码 |
| 4 | `.md` 用 LF | `git ls-files --eol "*.md"` | 混合行尾，diff 全红 |
| 5 | `dist/` 在 `.gitignore` 里 | `git check-ignore -v dist/svpn.exe` | 1.7MB 二进制进历史，返工（R3 §4） |
| 6 | `svpn-gui.exe` 必须被忽略 | `git check-ignore -v svpn-gui.exe` | 双产物发布会把 GUI 二进制提进源码树 |
| 7 | `core.autocrlf` 会让源码指纹漂移 | `.gitattributes` 里 `* text=auto eol=lf` + `*.cmd text eol=crlf` | 全新 clone 后 `RELEASE.txt` 的 MD5 全对不上（R3 §8.5） |
| 8 | 两个 exe 都要设只读 | `cmd //c "attrib dist\*.exe"` 看 `R` | 一次误构建冲掉发布物（R3 §8.1） |
| 9 | `run.cmd` 别用仓库根那份覆盖 | `stat -c %s dist/run.cmd` 应为 250 | 发布包丢掉「exe 不在旁边」的自检分支 |
| 10 | `SHA256SUMS.txt` 用 `sha256sum --text` | `cat -A` 看是两空格还是 `*` | 与 1.1.0 格式不一致，`-c` 仍能过但风格断裂 |
| 11 | 打包前扫泄漏 | §5.3 的 `ls dist/ \| grep -Ei ...` 零命中 | 把本机 `config.json`/hosts 备份发给用户 |
| 12 | 门禁单会话连跑 | 三条写在同一个 bash 块里 | 假的 `[=] 没有在运行的实例`（V1 §9.1） |
| 13 | 门禁前确认 7899/7900/443 空闲 | §1.1 | 门禁恒真（PID 9008 事故，R2 §环境风险） |
| 14 | 打包机与构建机同一份 exe | `md5sum dist/svpn.exe svpn.exe` | 发布物与源码指纹不对应 |
| 15 | `svpn-gui.exe -v` 会开窗挂住 | 只用 `svpn.exe -v` | 命令行卡死（本报告实测踩到） |

---

## 12. 一页速查（完整序列）

```bash
cd /d/LE/AI/wbwrk/svpn-cpp

# 1 前置
netstat -ano | grep -E ":(7899|7900) " ; netstat -ano | grep -E ":443 .*LISTENING"
git status --short

# 2 构建（零警告硬门槛）
cmd //c build.cmd 2>&1 | tee /d/LE/AI/wbwrk/wbwrk_tmp/build.log | tail -5
grep -ci "warning:" /d/LE/AI/wbwrk/wbwrk_tmp/build.log      # 期望 0
D:/LE/AI/tools/mingw64/bin/objdump.exe -p svpn.exe | grep -Ei "libstdc|libgcc|winpthread"   # 期望无输出
./svpn.exe -v                                    # 1.1.1

# 3 测试
unset NODE_OPTIONS http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
./tests/test_config.exe        # PASS 178 / FAIL 0
./tests/test_sni_parse.exe     # PASS 27 / FAIL 0
python tests/test_cli.py       # PASS 113+ / FAIL 0 / SKIP 0

# 4 门禁（单会话）
./svpn.exe up -b -p 17899 -m blacklist ; sleep 1
cat runtime/svpn.pid ; md5sum runtime/effective.json
./svpn.exe up -b ; cat runtime/svpn.pid ; md5sum runtime/effective.json
./svpn.exe down
netstat -ano | grep -E ":(7899|7900|17899) "    # 期望空

# 5 组包
cp svpn.exe svpn-gui.exe README.md README.en.md LICENSE config.example.json dist/
stat -c %s dist/run.cmd                          # 250
# 写 dist/RELEASE.txt（§7 模板）
cd dist && sha256sum --text svpn.exe svpn-gui.exe README.md README.en.md LICENSE run.cmd \
  config.example.json RELEASE.txt > SHA256SUMS.txt && sha256sum -c SHA256SUMS.txt
cd .. && cmd //c "attrib +R dist\svpn.exe dist\svpn-gui.exe"

# 6 zip
rm -rf /d/LE/AI/wbwrk/wbwrk_tmp/pkg && mkdir -p /d/LE/AI/wbwrk/wbwrk_tmp/pkg/svpn-1.1.1-win64
cp /d/LE/AI/wbwrk/svpn-cpp/dist/{svpn.exe,svpn-gui.exe,README.md,README.en.md,LICENSE,run.cmd,config.example.json,RELEASE.txt,SHA256SUMS.txt} \
  /d/LE/AI/wbwrk/wbwrk_tmp/pkg/svpn-1.1.1-win64/
cd /d/LE/AI/wbwrk/wbwrk_tmp/pkg && "/c/Program Files/7-Zip/7z.exe" a -tzip \
  /d/LE/AI/wbwrk/svpn-cpp/dist/svpn-1.1.1-win64.zip svpn-1.1.1-win64
cd /d/LE/AI/wbwrk/svpn-cpp/dist && unzip -t svpn-1.1.1-win64.zip && unzip -l svpn-1.1.1-win64.zip

# 7 git
cd /d/LE/AI/wbwrk/svpn-cpp
git add src/ tests/ build.cmd run.cmd .gitattributes .gitignore && git commit -m "feat(gui): ..."
git add README.md README.en.md docs/ _crew/ && git commit -m "docs: ..."
git tag -a v1.1.1 -F - <<'EOF'
...
EOF
git push origin main && git push origin v1.1.1
gh release create v1.1.1 dist/svpn-1.1.1-win64.zip --title "svpn 1.1.1 · Windows x64（GUI）" --notes-file dist/RELEASE.txt
```
