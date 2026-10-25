# D2 架构文档

## 1. 摘要

新建 `docs/ARCHITECTURE.md`（398 行，UTF-8 无 BOM + LF），按 8 个二级标题覆盖：边界、模块划分、进程与线程模型、三条数据流（ASCII 图）、契约边界（config.json / effective.json 逐键）、错误处理约定、本机实测事实、设计取舍。
全文 211 处行内引用，全部指向 `src/*` 或 `_crew/*` 的真实行；用脚本把每条引用回读原文件逐条核对，越界 0 条。
只新增了 `docs/ARCHITECTURE.md` 与本报告，未改任何源码。
一处与派单口述不一致，已在第 5 段说明（probe 实测结论）。

## 2. 做了什么

逐节对应派单要求：

1. **一句话与边界**（`docs/ARCHITECTURE.md:9-23`）——7 条「不做」逐条给代码依据：UDP（`src/proxy.cpp:622-626`）、TUN（`listen_on` 仅三处）、中间人（`src/sni.cpp:181-186`）、内置节点（`src/config.cpp:219`）、服务/自启（`src/main.cpp:298-300`）、注入（`src/main.cpp:117-122`）、回环监听（`src/config.cpp:211`）。
2. **模块划分**（:27-52）——15 个文件的职责 / 行数 / 依赖 / 类型，标出两个中枢（`proxy.cpp`、`main.cpp`）与契约文件（`svpn.hpp`）。
3. **进程与线程模型**（:56-142）——进程树图、10 行线程表（含创建点、结束条件）、四处 `select` 的用途表、`g_stop`/SNI 协同图、为什么禁 IOCP 与跨线程不抛异常、GUI 的 `streambuf`→`PostMessage`→`dispatch_cli` 通路。
4. **数据流**（:146-252）——SOCKS5、HTTP（CONNECT + 绝对 URI）、CDN 选边 + 两条消费路径（hosts 直指 / SNI 转发）共 5 张 ASCII 图，全部纯文本，无 mermaid。
5. **契约边界**（:256-348）——`svpn.hpp` 跨模块契约 12 类 + 内部细节 6 项；`config.json` 12+2 键逐键表（键名 `username`/`password` 与结构体 `user`/`pass` 的差异已标）；`effective.json` 7 键表（含本轮新加的 `upstream_kind`）；10 个运行期文件写入点。
6. **错误处理**（:352-369）——13 条约定：异常闸、socket 归属、`shutdown` 不 `close`、退出收敛、半开取舍、竞速不漏 fd、原子落盘、`wb` 落盘、`log_write` 重开文件与 `--log-to-file` 的互锁陷阱。
7. **本机实测硬事实**（:373-382）——4 条，带真实输出数值。
8. **设计取舍**（:386-400）——11 条，全部用代码或实测支撑（如 `sni_port` 必须 443 是因为 hosts 无端口位）。

## 3. 验证

行号怎么核：先用 `grep -n` 取关键锚点，再用脚本 `wbwrk_tmp/check_cites.py` 把文档里全部 `src/…:N` 形式的引用回读原文件逐条打印对照。

锚点核对（原文粘贴）：

```
$ grep -n "std::thread dumper\|for (int i = 0; i < 50" src/main.cpp
421:    std::thread dumper([&]() {
423:            for (int i = 0; i < 50; ++i) {

$ grep -n "int serve_all\|void accept_loop\|int kRelayIdleMs\|void shutdown_live" src/proxy.cpp
34:constexpr int kRelayIdleMs = 3600000;            // 中继空闲上限：1h，只是兜底
483:    void shutdown_live() {
941:void accept_loop(Core& core, socket_t ls, bool http_mode) {
1005:int serve_all(const Config& cfg, Stats& st, std::atomic<bool>& stop) {
```

全量回读（脚本输出尾部）：

```
TOTAL citations: 211
OUT OF RANGE: 0
```

首轮跑出 1 条越界（`src/rules_data.cpp:43-52`，该文件只有 51 行），已改成 `43-51`；同一轮还发现 3 处指向错行（`apply_ip_map` 365→362、`serve_sni` 调用 415→418、`src/main.cpp:448-452`→`449-450`），均已修正。另修了两处正文事实性措辞（stats 落盘滞后改成「最多滞后一个 5 秒周期」）。

格式验证：

```
$ wc -l docs/ARCHITECTURE.md
398 docs/ARCHITECTURE.md
$ head -c 3 docs/ARCHITECTURE.md | od -An -tx1
 23 20 73          # "# s"，无 BOM
$ grep -c $'\r' docs/ARCHITECTURE.md
0                  # 全 LF
```

实测数据来源（第 7 段）——`probe` 连跑两次，结论一致：

```
[i] 直连可用 6 / 需节点 3 / 死端点 0 / 当前不可用 3
avatars.steamstatic.com            直连可用  281ms
login.steampowered.com             直连可用  688ms
api.steampowered.com               需节点 · SNI 阻断  4235ms
store.steampowered.com             需节点 · SNI 阻断  4234ms
steamcommunity.com                 需节点 · SNI 阻断  0ms
```

`cdn` / `status` 也各跑一次取真实值（8 个域名、2 个有 best_ip；hosts 0 条）。这些命令都不占端口、不改系统设置。

## 4. 契约变更

无。未改 `src/`、未改 `svpn.hpp`、未改 `README.md`、未改 `build.cmd`。
新增文件仅两个：`docs/ARCHITECTURE.md`、`_crew/reports/D2-architecture.md`。

## 5. 没做完/风险

**只读接口没读实现的模块**（点名）：

- `src/tls.cpp`（702 行）：只看了函数/结构清单与 `parse_client_hello_sni` 的签名位置，SChannel 握手、`TlsConn::send/recv` 的分段逻辑、证书 CN 提取都没读。文档里提到 TLS 的地方只有 3 处，都是「谁调用了它」，没有描述其内部行为。
- `src/winsys.cpp`（812 行）：读了函数清单、托管块常量（:29-30）、`sanitize_entries` 调用点（:499）、几处注册表读写行；`write_hosts_atomic` 的字节保真逻辑、`elevate` 的提权实现、备份/还原的完整分支没逐行读。
- `src/pac.cpp`（137 行）：只看了 `build_pac` 的位置，生成的 JS 内容没读。
- `src/json.cpp`（333 行）：只看了导出函数清单。

**没实测的分支**（按 SPEC §2.6 禁止）+ 原因：

- `system on/off`、`--system-proxy`、`hosts apply/clear`、`down --clear-hosts` 全未执行——会改本机全局 WinINET 或弹 UAC。文档里这些路径只做静态推理，依据都标了行号，但没有运行证据。
- `up` / `_serve` 未起实例（端口纪律），线程表是从创建点静态推出来的，不是 `Process Explorer` 观察到的。

**风险 / 需要 Lead 决策的两件事**：

1. **派单口述的 4 条「硬事实」与实测不一致**。派单写的是「商店/登录/CDN 直连正常；`steamcommunity.com` 与 `avatars.steamstatic.com` SNI 阻断」；我在本机跑两次 `probe`，结论都是 `store`/`api`/`steamcommunity` 三条阻断、`avatars` 与 `login`、CDN 那几个直连可用（6/3）。`_crew/reports/V1-cli-audit.md:39` 的另一次独立测量也是 6/3、阻断的正是 store/api/community。文档按实测写了，并注明结果会漂移。**发版说明/README 若要提这条，建议用实测口径。**
2. **`README.md` 的子命令表被一行正文截断**：`README.md:81` 是 `| gui | …`，`README.md:83` 是一段正文，`README.md:84` 又接着 `| pac | …`。Markdown 表格被空行+段落打断，`pac`/`steam`/`doctor` 三行会渲染成裸文本。README 归 Lead，我没动。

**其他如实记录**（都不改代码，只写进文档）：

- 前台模式（`up` 不带 `-b`）不启动 SNI 转发、也不启动统计定时落盘（`src/main.cpp:263-275`）——`cdn.enabled=true` 时前台跑等于 443 没人听。
- `sni_stop` 声明于 `src/main.cpp:414` 后无读取点，是遗留变量。
- `main.cpp` 在我写作期间被 P4 从 1195 行改到 1204 行（加了 `upstream_kind`），所有 `main.cpp:4xx` 之后的行号按我读到的 1204 行版本；若再改动会整体漂移，文档开头已注明「以 `grep -n` 为准」。
