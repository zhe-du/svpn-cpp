# svpn (C++) 架构文档

适用版本 `1.1.1`（`src/main.cpp:22`）。本文只描述**代码实际做到的事**，每条断言后面标源码位置，格式 `(src/文件:行)`。
行号取自 2026-10-25 工作树快照；结构大体稳定，细节行号会随修改变动，读时以 `grep -n` 为准。
构建与工具链不在这里，见 `docs/BUILD.md`；用户面用法见 `README.md`。

---

## 1. 一句话与边界

一句话：**在本机开两个 TCP 监听（SOCKS5 + HTTP），按域名把 Steam 流量判给「直连」或「你的上游节点」，再附一个 CDN 选边器把下载域名钉到当前最快的 Akamai 边缘。**

它只解决「Steam 的这条连接该走哪条路」，不解决出口、不改变系统网络栈。

| 不做的事 | 代码依据 |
|---|---|
| 不代理 UDP，无 UDP ASSOCIATE | SOCKS5 只接受 `cmd == 0x01`，其余回 `0x07` 并关闭 `(src/proxy.cpp:622-626)` |
| 没有 TUN / 虚拟网卡 / 驱动 | `listen_on` 的调用点全程序只有三处：SOCKS5 `(src/proxy.cpp:1010)`、HTTP `(src/proxy.cpp:1025)`、SNI `(src/sni.cpp:229)`；无任何网卡/过滤驱动 API |
| 不做 TLS 中间人 | 已读到的 ClientHello 原样 `send_all` 给目标，不生成证书、不改字节 `(src/sni.cpp:181-186)` |
| 不内置节点 | 默认 `upstream.host = "NODE_HOST"` `(src/config.cpp:219)`，`configured()` 判否 `(src/util.cpp:125-127)` |
| 不装服务、不开机自启 | 常驻体是 `CreateProcessW` 拉起的普通进程，`DETACHED_PROCESS` + `CREATE_NO_WINDOW` `(src/main.cpp:298-300)`；全仓无 SCM 调用 |
| 不注入进程 | `OpenProcess` 只用于 pid 存活判断 `(src/main.cpp:117-122)` 与 `down` 的 `TerminateProcess` `(src/main.cpp:449-450)` |
| 默认只听回环 | `listen_host` 默认 `127.0.0.1` `(src/config.cpp:211)`；SNI 硬编码绑回环 `(src/sni.cpp:229)` |

---

## 2. 模块划分

`src/` = 14 个 `.cpp` + 1 个 `.hpp`，`wc -l` 合计 7747 行。**依赖方向单一向下**，没有环。

| 文件 | 行数 | 职责 | 依赖谁 | 类型 |
|---|---|---|---|---|
| `json.cpp` | 333 | 手写 JSON 解析/序列化、BOM 跳过 | 无 | 叶子 |
| `util.cpp` | 211 | 字符串、时间、`fmt`、`log_write`、`Paths::resolve`、`Stats` | json | 叶子 |
| `rules_data.cpp` | 51 | Python 版导出的常量表：Steam 后缀 / Valve 网段 / 国内直连后缀 / CDN 域名 / 默认 bypass | 无 | 叶子（自动生成，勿手改） |
| `rules.cpp` | 358 | 域名归一、后缀匹配、CIDR 判定、`Router::decide` | rules_data | 叶子 |
| `config.cpp` | 400 | 默认值、深合并、逐键归一、原子保存、未知键收集 | json, util, rules_data | 叶子 |
| `pac.cpp` | 137 | 生成 PAC 脚本 | rules_data | 叶子 |
| `net.cpp` | 659 | socket 生命周期、非阻塞 connect、DNS 带超时、多 IP 竞速、收发、`relay_stream` | util | 基础设施 |
| `tls.cpp` | 702 | SChannel 客户端握手、证书 CN 提取、`parse_client_hello_sni` | net | 基础设施 |
| `cdn.cpp` | 743 | DoH 解析、TLS 竞速、1MB 实测、缓存读写、`apply_ip_map` | net, tls, json | 功能 |
| `winsys.cpp` | 812 | hosts 托管块原子写、WinINET 备份/接管/还原、提权 | json, util | 功能（写系统） |
| `sni.cpp` | 299 | 443 上的 SNI 透明转发服务 | net, tls, rules | 服务 |
| `proxy.cpp` | 1065 | SOCKS5 / HTTP 代理内核、分流拨号、中继、统计 | net, tls, rules, util | **中枢** |
| `gui.cpp` | 460 | Win32 窗口、控件布局、日志捕获、按钮 → 命令 | main（`dispatch_cli`） | 前端 |
| `main.cpp` | 1204 | 子命令编排、进程管理、线程调度、`effective.json`、`dispatch` | 全部 | **中枢** |
| `svpn.hpp` | 313 | 共享契约：类型 + 全部跨模块函数声明 | — | **契约** |

要点：

- **`proxy.cpp` / `main.cpp` 是中枢**：前者持有连接生命周期，后者持有进程与线程生命周期。改这两处影响最大。
- `gui.cpp` 反向依赖 `main.cpp` 的 `dispatch_cli`（声明在 `src/svpn.hpp:309`，实现在 `src/main.cpp:1186`），因此链接顺序上 GUI 在最上层；`rules_data.cpp` 是唯一的生成文件，改动会被下一次 Python 导出覆盖 `(src/rules_data.cpp:1-2)`。

---

## 3. 进程与线程模型

### 3.1 进程

```
svpn.exe up -b                     ← 父进程，只活几秒
   │ CreateProcessW: "svpn.exe" _serve --log-to-file --listen-port … --mode … [--node …]
   │ 句柄不继承 (bInheritHandles=FALSE)                       src/main.cpp:298-300
   ▼
svpn.exe _serve                    ← 常驻体，写 runtime/svpn.pid
   └ 轮询等 SOCKS5 端口就绪 → 写入 pid → 父进程退出            src/main.cpp:309-330
```

`up` 把 `-p/-m/--node` 显式翻译成 `--listen-port/--mode/--node` 传给子进程 `(src/main.cpp:284-290)`，因为子进程看不到父进程 argv；生效值再落到 `runtime/effective.json` 供 `status` 读回 `(src/main.cpp:386-404)`。

### 3.2 线程表

| 线程 | 创建点 | 干什么 | 结束条件 |
|---|---|---|---|
| 主线程 | CRT → `dispatch` | `_serve` 时跑 `serve_all`：**HTTP 入口的 accept 循环跑在主线程** `(src/proxy.cpp:1053-1054)`；`http_port==0` 时退化为纯 sleep 轮询 `(src/proxy.cpp:1056-1058)` | `stop` / `abort` |
| SOCKS5 入口线程 | `serve_all` `(src/proxy.cpp:1046)` | 同上的 accept 循环 | `abort` 被任一入口置真 `(src/proxy.cpp:958)` |
| 每连接线程 | `spawn` `(src/proxy.cpp:917-926)`，`detach` | `conn_main` → `socks_session` / `http_session` | 连接结束，`finish()` 减计数 `(src/proxy.cpp:923-924)` |
| 连接线程创建失败 | `spawn` 的 catch `(src/proxy.cpp:927-937)` | 手工回退计数 + `close_socket` | — |
| SNI 转发线程 | `cmd_serve`，仅 `cdn.enabled` `(src/main.cpp:414-421)` | `serve_sni` → 443 的 accept 循环 `(src/sni.cpp:257)` | `g_stop` |
| SNI 每连接线程 | `serve_sni_to` `(src/sni.cpp:281-284)`，**join 不 detach** | `handle_conn`，完成后置 flag，由 `reap` 回收 `(src/sni.cpp:243-254)` | 连接结束 / `live.shutdown_all()` |
| 统计落盘线程 `dumper` | `cmd_serve` `(src/main.cpp:424-433)` | 50 × `Sleep(100)` = **5 秒**一次 `st.dump(p)` | `dump_stop` |
| 拨号竞速线程 | `race_connect`，最多 4 条 `(src/net.cpp:229-264)` | `connect_ip_wait` 抢第一个成功的 IP | 全部 `join` `(src/net.cpp:270-272)` |
| DNS 工作线程 | `resolve_system` `(src/net.cpp:320-350)` | 跑不可中断的 `getaddrinfo` | 正常 `join`；超时则 `detach`（只持有 `shared_ptr`，无 fd 泄漏）`(src/net.cpp:365-370)` |
| GUI 任务线程 | `run_async` `(src/gui.cpp:177-193)`，`detach` | `dispatch_cli(args)` | 结束后 `PostMessage(WM_APP_DONE)` |

注意一处事实：**前台模式（`up` 不带 `-b`）没有 SNI 线程、也没有 `dumper`** —— 直接 `serve_all` 然后 `st.dump` 一次退出 `(src/main.cpp:263-275)`，因此前台跑 `cdn.enabled=true` 时 127.0.0.1:443 无人监听。常驻模式的线程上限 = 2 个 accept + 1 个 SNI accept + 1 个 dumper + N 个连接线程（N 无上限，见 §8）。

### 3.3 `select` 轮询结构

全程序四处 `select`，全部是「带超时的轮询」而非事件驱动：

| 位置 | 超时 | 作用 |
|---|---|---|
| `wait_readable` `(src/proxy.cpp:165-174)` | 100ms `(src/proxy.cpp:35)` | accept 前等可读，顺便轮询 `stop` |
| `relay_stream` `(src/net.cpp:540-541)` | 空闲上限（代理 1h、SNI 0=不限） | 双向中继主循环 |
| `wait_fd` `(src/net.cpp:91-106)` | 由 deadline 算 | 非阻塞 `send/recv` 写不动时等可写 |
| SNI accept `(src/sni.cpp:259-274)` | 100ms `(src/sni.cpp:40)` | 同第一行 |

`relay_stream` 是唯一的热路径：进函数就把两个 socket 置非阻塞 `(src/net.cpp:511-513)`，`select` 只等**可读**，读到数据后 `push_all` 内部再 `select` 等可写 `(src/net.cpp:191-209)`；一侧 EOF 时 `shutdown(SD_SEND)` 做半关闭而不是关连接 `(src/net.cpp:557-559, 574-576)`；退出前恢复阻塞模式 `(src/net.cpp:585-587)`。

### 3.4 `g_stop` 与 SNI 的协同

```
Ctrl+C / CTRL_CLOSE
   └ ctrl_handler: g_stop = true                    src/main.cpp:336-342（g_stop 定义在 335）
        ├ serve_all 的两个 accept_loop 看到 stop 退出  src/proxy.cpp:943
        ├ SNI accept 循环看到 stop 退出                src/sni.cpp:257
        └ serve_all 返回 → dump_stop=true → join dumper → join sni_thread
                                                       src/main.cpp:434-441
```

`serve_sni` 收的是 `g_stop` 的引用 `(src/main.cpp:418)`，不是局部量；`sni_stop` 在 `src/main.cpp:414` 声明后**没有任何读取点**，属于遗留变量。`down` 不走这条路：它直接 `TerminateProcess` `(src/main.cpp:449-450)`，因此 `runtime/stats.json` 的最后一次 dump 最多滞后一个 dumper 周期（5 秒）。

### 3.5 为什么是这套模型

- **硬约束来自项目规范**：`_crew/SPEC.md:20-22` 明确要求「线程 + 阻塞 socket + `select`，禁止 IOCP/epoll，跨线程不抛异常」。代码里对应的就是上面的线程表。
- **connect 超时只能这样做**：Windows 上阻塞 `connect` 的超时不可控（SYN 重传约 21s），所以拨号一律走非阻塞 connect + `WSAEventSelect` 等落定 `(src/net.cpp:108-188)`，注释里写了原因。
- **跨线程不抛异常**落实为三道闸：`conn_main` 全包 `try/catch` 并计 `errors` `(src/proxy.cpp:900-915)`；`spawn` 建线程失败手工回退 `active` 计数 `(src/proxy.cpp:927-937)`；`run_pool` 的任务体 `try/catch` 后直接退出该 worker `(src/cdn.cpp:148-154)`。
- **`select` 只在等待点出现**，业务代码全是同步顺序调用；代价是每连接一个线程（§8）。

### 3.6 GUI 线程模型

```
UI 线程（GetMessage 循环 src/gui.cpp:453-457）
   │ 按钮 WM_COMMAND → run_async({"svpn.exe","up","-b"}, …)     src/gui.cpp:330-383
   │                        │ g_busy CAS 失败就直接 return（防重入）
   ▼                        ▼
 只画界面            工作线程: dispatch_cli(args)                src/gui.cpp:177-193
                            │ std::cout/std::cerr 临时换成 LogBuf
                            ▼
                     LogBuf::overflow 按行切分 → PostMessage(WM_APP_LOG, new string)
                                                              src/gui.cpp:67-98, 92
                            ▼
                     UI 线程 WM_APP_LOG: append_log(*p); delete p  src/gui.cpp:309-316
                     结束后 WM_APP_DONE: set_busy(false) + refresh_status  src/gui.cpp:318-328
```

- **所有按钮都走 `dispatch_cli()`**，没有第二个实现：启动=`up -b`、停止=`down`、探测=`probe`、选边=`cdn --refresh`、系统代理=`system on|off`、初始化=`init` `(src/gui.cpp:333-379)`。这是刻意的：命令行与图形界面永远同一条代码路径，不存在「GUI 特供分支」。
- 唯一不经工作线程的调用是 `refresh_status()` 里的同步 `capture({"svpn.exe","status"})` `(src/gui.cpp:139-147, 150)`，它把 cout/cerr 临时换成 `ostringstream`，在 UI 线程内联执行。
- 工作线程**不直接碰控件**，只 `PostMessage`；日志字符串的所有权在线程间通过 `new`/`delete` 明确交接 `(src/gui.cpp:91-93, 310-314)`。

---

## 4. 数据流

### 4.1 SOCKS5 请求

```
client ──TCP──► accept_loop(7899)                    src/proxy.cpp:941
                  │ wait_readable 100ms 轮询 stop    src/proxy.cpp:944
                  │ accept_one                        src/net.cpp:649
                  ▼
              spawn(core, cs, http=false) → 独立线程  src/proxy.cpp:917
                  ▼
              socks_session                          src/proxy.cpp:561
                  │ ① 方法协商：本地 auth 空 ⇒ 只用 0x00（607-615）
                  │            配了 auth ⇒ 要 0x02，走 RFC1929（582-606）
                  │ ② 只认 CONNECT，其余回 0x07（622-626）
                  │ ③ ATYP：0x01 v4 / 0x04 v6 / 0x03 域名（629-650）
                  │ ④ st.conns++ / st.hit(host)（655-656）
                  ▼
              route_dial(host, port)                 src/proxy.cpp:541
                  │ Router::decide(host)              src/rules.cpp:314
                  │   direct_hosts > proxy_hosts > 回环 >
                  │   字面量 IP 的 Valve 段 > 后缀/网段 > mode 兜底
                  ├── "direct"  ──────────────► dial_direct  src/proxy.cpp:409
                  │                              ip_map 命中就直连该 IP，失败再解析（420-426）
                  │                              否则 DNS → race_connect（src/net.cpp:215）
                  └── "upstream" 且 upstream_usable ─► dial_via  src/proxy.cpp:429
                                                 socks5（含认证）/ http CONNECT
                                                 （上游是占位符时降级直连 + WARN，549）
                  ▼
              socks_reply(0x00) → do_relay → relay_stream(idle=1h)  src/proxy.cpp:666-672
                  ▼
              st.up_bytes += u; st.down_bytes += d                  src/proxy.cpp:532-538
```

### 4.2 HTTP 代理请求

```
client ──TCP──► accept_loop(7900) → spawn(http=true) → http_session  src/proxy.cpp:767
  ① 读头：分块读到 CRLFCRLF，上限 16KB、15s；多读的字节留 leftover
     src/proxy.cpp:33-36, 772（recv_until_chunked 在 139-163）
  ② cfg.auth 非空 ⇒ 校验 Proxy-Authorization，不符回 407   src/proxy.cpp:782-793
  ③ 分支：
     ├─ CONNECT host:port
     │    split_host_port（拒 host:abc / :99999）           src/proxy.cpp:722-765, 808
     │    → route_dial → "HTTP/1.1 200 Connection Established"
     │    → leftover 先灌给上游（一个字节都不丢）            src/proxy.cpp:828-834
     │    → do_relay                                        src/proxy.cpp:836
     └─ 绝对 URI（只认 http://）
          剥 Proxy-Authorization / Proxy-Connection / Connection
          请求行改写为 origin-form（"/path"），末尾强制 Connection: close
                                                              src/proxy.cpp:841-880
          → leftover 补发 → 单次请求响应中继，字节计入统计     src/proxy.cpp:886-896
```

### 4.3 CDN 加速

选边（离线计算，写缓存）：

```
svpn cdn --refresh                                 src/main.cpp:808
  └ 域名 = kCdnDomains（8 个，可 --domain 覆盖）    src/rules_data.cpp:43-51
  ▼
refresh(domains, bench, timeout)  → 4 线程池       src/cdn.cpp:596-615
  └ rank_domain                                    src/cdn.cpp:489
      ├ DoH GET ?name=&type=A（4 家轮试；400/404/405/415 再补 POST）
      │                                             src/cdn.cpp:436-448
      ├ 合并系统解析结果（可能被污染，靠下一步淘汰）  src/cdn.cpp:513-526
      ├ tls_probe 并行竞速，最多 6 个候选           src/cdn.cpp:537-561
      └ bench_edge：前 3 名拉 1MB Range 算 MB/s     src/cdn.cpp:572-585
  ▼
save_cache → runtime/cdn-cache.json（先 .tmp 再 MoveFileEx）
                                                   src/cdn.cpp:677-712, 176-203
```

消费路径 A —— `cdn.enabled=false`（hosts 直指边缘 IP，不需要常驻）：

```
svpn hosts apply                                   src/main.cpp:849
  └ load_cache(TTL=refresh_hours)                  src/main.cpp:888-889
  └ 按 best_ip 分组成 "1.2.3.4  a.com b.com"       src/main.cpp:903-907
  └ apply_hosts → 托管块原子替换 → flush_dns       src/winsys.cpp:496, 615
  ▼
Steam 解析 a.com 直接拿到边缘 IP，svpn 可以关掉
```

消费路径 B —— `cdn.enabled=true`（SNI 透明转发，必须常驻）：

```
svpn hosts apply → 只写一条 127.0.0.1 覆盖全部 cdn.hosts   src/main.cpp:864-880
svpn up
  ├ apply_ip_map(cache → cfg.ip_map, TTL=refresh_hours)    src/main.cpp:362（cdn.cpp:739）
  └ SNI 线程 serve_sni(cfg, st, g_stop)                    src/main.cpp:414-421
       serve_sni_to: listen 127.0.0.1:cfg.cdn.sni_port     src/sni.cpp:229
         │ select 100ms 轮询 stop                          src/sni.cpp:257-274
         ▼ accept → 每连接一个 join 线程                    src/sni.cpp:281
       handle_conn                                         src/sni.cpp:121
         ① 读 TLS 记录（≤16KB，单次 10s，非 0x16 立刻放弃） src/sni.cpp:100-118
         ② parse_client_hello_sni 取 SNI                   src/tls.cpp:609
         ③ 白名单 allowed(cdn.hosts)：空表 = 全拒           src/sni.cpp:89-95, 224-226
         ④ 目标：ip_map[host] 命中就直连边缘 IP，否则系统解析逐个试
                                                            src/sni.cpp:157-169
         ⑤ 先把已读字节原样回注，再 relay_stream(idle=0)     src/sni.cpp:181-192
```

客户端视角一条线：`Steam ──TLS──► 127.0.0.1:443 (hosts) ──► serve_sni 按 SNI 命中 ip_map ──► 23.x.x.x:443 边缘 ──TLS──► Steam`，全程不解密。

`sni_port` 是**监听**端口，目标端口恒为 443 `(src/sni.cpp:41, 209-211)`——配置里没有「目标端口」这一项。

---

## 5. 契约边界

### 5.1 `svpn.hpp` 里哪些是跨模块契约

| 类别 | 内容 | 行 |
|---|---|---|
| 基础类型 | `socket_t`(=`uintptr_t`)、`kBadSock`、`kDefaultTimeoutMs` | `src/svpn.hpp:18-22` |
| 数据结构 | `Node`、`UpstreamCfg`、`Config`、`Paths`、`Stats`、`Json`、`Edge`、`DomainResult`、`Cache` | `src/svpn.hpp:25-139, 240-256` |
| 规则 | `normalize_host`/`match_suffix`/`is_ip`/`in_cidr`/`Router` | `src/svpn.hpp:166-178` |
| 配置 | `default_config`/`merge_config`/`load_config`/`save_config`/`config_to_json`/`unknown_config_keys` | `src/svpn.hpp:181-188` |
| 网络 | `net_init`/`tcp_connect*`/`resolve_system`/`send_all`/`recv_*`/`relay_stream`/`listen_on`/`accept_one` | `src/svpn.hpp:191-204` |
| TLS | `TlsConn`、`TlsProbe`、`tls_connect`、`tls_probe`、`parse_client_hello_sni` | `src/svpn.hpp:207-227` |
| 拨号 | `dial_via`、`dial_direct` | `src/svpn.hpp:230-231` |
| 服务 | `serve_socks`/`serve_http`/`serve_all`/`serve_sni` | `src/svpn.hpp:234-237, 305` |
| CDN | `doh_query`/`rank_domain`/`refresh`/`load_cache`/`save_cache`/`best_ip_map`/`apply_ip_map` | `src/svpn.hpp:258-267` |
| 系统 | `apply_hosts`/`clear_hosts`/`read_wininet`/`backup_wininet`/`apply_wininet`/`restore_wininet`/`is_admin`/`elevate` | `src/svpn.hpp:270-296` |
| 入口 | `dispatch_cli`、`gui_main` | `src/svpn.hpp:309-311` |
| 生成数据 | `kSteamSuffixes`/`kCnDirectSuffixes`/`kSteamCidrs`/`kCdnDomains`/`kDefaultBypass` | `src/svpn.hpp:155-163` |

**内部细节（外面看不到，也不该依赖）**：

| 细节 | 位置 | 为什么在模块内部 |
|---|---|---|
| `Core`（活动连接表、`abort`、`active` 计数） | `src/proxy.cpp:445-499` | 只有一个 `serve_*` 家族用 |
| 模块级 `g_ip_map` 注册表 | `src/proxy.cpp:180-194` | `dial_direct` 的签名里没有 `cfg`，而契约不可改，只好在 `serve_one` 打开监听时 `publish_ip_map(cfg.ip_map)` `(src/proxy.cpp:966)` |
| `Settings`、`LiveSet`、`SockGuard` | `src/sni.cpp:45-85` | SNI 服务私有 |
| `kProviders`、`kPaths`、`bench_edge` | `src/cdn.cpp:27-48, 309` | 选边策略细节 |
| hosts 托管块标记 | `src/winsys.cpp:29-30` | 私有常量，但**已写进用户机器**，改动等于破坏兼容 |
| `Args`（`-p`/`--port` 等解析） | `src/main.cpp:177-197` | 只服务 CLI |

### 5.2 `config.json` 的键（以 `config.cpp` 为准）

顶层**恰好 12 个**（`src/config.cpp:23-26`）+ 2 个可选高级键（`src/config.cpp:27`）：

| 键 | 类型 | 默认 | 归一化 / 备注 |
|---|---|---|---|
| `listen_host` | str | `127.0.0.1` | 空串回落回环 `(src/config.cpp:167-168)` |
| `listen_port` | int | `7899` | 1..65535，越界回落 `(src/config.cpp:169)` |
| `http_port` | int | `7900` | **0 合法** = 不起 HTTP 入口 `(src/config.cpp:170, 63-64)` |
| `mode` | str | `whitelist` | 枚举 `whitelist\|all\|blacklist`，非法回落 `(src/config.cpp:159, 171)` |
| `connect_timeout` | int(秒) | `12` | 非正 / >300 回落；**小于 1 抬到 1**（int 装不下小数）`(src/config.cpp:69-74)` |
| `auto_node` | bool | `true` | 只读入与回写，**无任何消费点**（`grep -rn auto_node src/` 命中 5 行：config.cpp 4 行读写 + svpn.hpp 字段声明） |
| `upstream.kind` | str | `socks5` | 枚举 `socks5\|http\|direct` `(src/config.cpp:160, 176)` |
| `upstream.host` | str | `NODE_HOST` | 等于占位符即「未配置」`(src/config.cpp:177, src/util.cpp:126)` |
| `upstream.port` | int | `1080` | 同上 1..65535 `(src/config.cpp:178)` |
| `upstream.username` / `.password` | str | 空 | JSON 键名是 `username`/`password`，结构体里叫 `user`/`pass` `(src/config.cpp:179-180, 251-252)` |
| `auth.username` / `.password` | str | 空 | 非空即开启本地鉴权（SOCKS5 RFC1929 + HTTP Basic）`(src/config.cpp:183-184)` |
| `browser.apply_system_proxy` | bool | `true` | `(src/config.cpp:187)` |
| `browser.use_pac` | bool | `false` | 读入但无消费点（PAC 走独立子命令）`(src/config.cpp:188)` |
| `browser.bypass` | str | `kDefaultBypass` | 写 WinINET 时用 `(src/config.cpp:189, src/main.cpp:367-368)` |
| `cdn.enabled` | bool | `false` | `true` ⇒ 起 SNI 转发 + hosts 指向 127.0.0.1 `(src/config.cpp:192)` |
| `cdn.refresh_hours` | num | `6` | ≤0 回落 6 `(src/config.cpp:193-194)` |
| `cdn.sni_port` | int | `443` | 1..65535，**不允许 0** `(src/config.cpp:195)` |
| `cdn.hosts` | list | 8 个 CDN 域名 | 也是 SNI 白名单；可写逗号/空格分隔字符串 `(src/config.cpp:196-197)` |
| `proxy_hosts` | list | `[]` | 强制走节点（精确匹配）`(src/config.cpp:199)` |
| `direct_hosts` | list | `[]` | 强制直连（精确匹配，优先级最高）`(src/config.cpp:200)` |
| `proxy_suffixes` | list | 内置 Steam 表 | **替换**而非追加；空 = 没写 `(src/config.cpp:201)` |
| `direct_suffixes` | list | 内置国内表（仅 `blacklist` 用） | 同上 `(src/config.cpp:202)` |

合并规则：`_` 开头键丢弃、未知键丢弃并记入 `unknown_config_keys()` `(src/config.cpp:295-305)`，`doctor` 会把它们报出来 `(src/main.cpp:1063-1075)`。

### 5.3 `runtime/effective.json` 的键（当前代码）

由 `_serve` 在起监听前写一次 `(src/main.cpp:386-404)`，`status` 读回 `(src/main.cpp:503-535)`，`down` 删除 `(src/main.cpp:465-468)`。

| 键 | 类型 | 何时出现 | 谁读 |
|---|---|---|---|
| `listen_host` | str | 恒有 | 目前无人读 |
| `listen_port` | int | 恒有 | `status` 覆盖 `config.json` 的值 `(src/main.cpp:508)` |
| `http_port` | int | 恒有 | 同上 `(src/main.cpp:509)` |
| `mode` | str | 恒有 | 同上 `(src/main.cpp:510)` |
| `upstream_configured` | bool | 恒有 | 决定是否读 `upstream`；为 false 时清空节点显示 `(src/main.cpp:513-534)` |
| `upstream` | str `"host:port"` | 仅 `configured()` | `status` 拼节点行 `(src/main.cpp:514-525)` |
| `upstream_kind` | str | 仅 `configured()` | `status` 拼节点行的 kind `(src/main.cpp:527-530)` |

> 本条对应 1.1.1 收尾时的工作树：`upstream_kind` 是这一轮刚加的（`src/main.cpp:398-400` 的注释说明了为什么必须落盘）。若手上二进制更早，这个键不存在。

### 5.4 运行期文件

全部落在 exe 同级的 `runtime/`，由 `Paths::resolve()` 固定 `(src/util.cpp:130-146)`：

| 文件 | 写入点 |
|---|---|
| `config.json` | `save_config` 原子替换 `(src/config.cpp:357-393)` |
| `runtime/svpn.log` | `log_write` 每次追加 `(src/util.cpp:101-112)` |
| `runtime/svpn.pid` | `_serve` 启动 / `down` 删除 `(src/main.cpp:385, 467)` |
| `runtime/effective.json` | `_serve` `(src/main.cpp:402)` |
| `runtime/stats.json` | `dumper` 每 5 秒 `(src/main.cpp:424-433, src/util.cpp:174-209)` |
| `runtime/probe.json` | `probe` 结束 `(src/main.cpp:696-701)` |
| `runtime/cdn-cache.json` | `save_cache` `(src/cdn.cpp:677-712)` |
| `runtime/svpn.pac` | `pac` `(src/main.cpp:971-975)` |
| `runtime/hosts.backup` | `apply_hosts` 首次备份 `(src/winsys.cpp:496-545)` |
| `runtime/wininet-backup.json` | `backup_wininet` `(src/winsys.cpp:655)` |

---

## 6. 错误处理与可靠性约定

| 约定 | 实现 |
|---|---|
| **跨线程不抛异常** | `conn_main` 捕获 `std::exception` 与 `...`，计 `errors` 后正常返回 `(src/proxy.cpp:900-915)`；线程池任务体同样 `(src/cdn.cpp:148-154)`；`spawn` 的 `std::thread` 构造失败用 catch 兜底 `(src/proxy.cpp:920-937)` |
| **socket 谁开的谁关** | 连接线程入口即 `SockGuard guard(cs)` `(src/proxy.cpp:901)`；远端 socket 在 `do_relay` 里 `SockGuard rg(remote)` `(src/proxy.cpp:533)` 或会话内的 `rg` `(src/proxy.cpp:866)`；SNI 侧两个 socket 各一个 guard `(src/sni.cpp:125-126, 176-177)`；`close_socket(kBadSock)` 幂等 `(src/net.cpp:430-433)` |
| **跨线程只 `shutdown` 不 `close`** | `Core::shutdown_live` 对所有活动 socket `shutdown(SD_BOTH)` `(src/proxy.cpp:482-486)`；SNI 的 `LiveSet::shutdown_all` 同策略，注释写明跨线程 close 正在 `recv` 的 socket 是未定义行为 `(src/sni.cpp:52-69)` |
| **先摘除再减计数** | `conn_main` 返回后 `del_live(cs)` 才 `finish()`，保证 `shutdown_live` 不会碰到已关句柄 `(src/proxy.cpp:921-925)` |
| **退出不无限期挂住** | accept 循环先 `abort=true`，再 `shutdown_live()`，再 `wait_idle(5000ms)` `(src/proxy.cpp:958-960, 37)` |
| **半开连接取舍** | 一侧 EOF ⇒ `shutdown(SD_SEND)` 让另一侧继续回数据 `(src/net.cpp:557-559, 574-576)`；空闲兜底 1 小时 `(src/proxy.cpp:34)`，SNI 中继 idle=0 不限 `(src/sni.cpp:192)`。因此「客户端断开但服务端还在发」的连接会一直挂着，直到系统 TCP 超时 |
| **竞速不漏 fd** | 非赢家的连接一律 `close_socket` `(src/net.cpp:256-259)`；赢家出现时 `WSASetEvent(cancel)` 让输家立刻退出 `(src/net.cpp:226-227, 171-173)` |
| **拨号兜底** | 竞速全失败后按原顺序再逐个试一遍，预算另算（总耗时 ≤ 2×timeout）`(src/net.cpp:399-408)` |
| **不半死不活** | `serve_all` 先把 SOCKS5 与 HTTP 两个监听都 bind 好再起线程，任一失败整体返回 1/2/3 `(src/proxy.cpp:1008-1032)` |
| **原子落盘** | config：写 `.tmp` → `MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)` `(src/config.cpp:366-390)`；cdn-cache 同法 `(src/cdn.cpp:176-203)`；hosts 同法且只动托管块 `(src/winsys.cpp:271-288, 29-30)` |
| **统计落盘必须 `"wb"`** | 用 `_wfopen_s(..., L"wb")` + `fwrite`；用 `ccs=UTF-8` 会把窄字节当 UTF-16 重编码成乱码 `(src/util.cpp:202-208)` |
| **`log_write` 会重开文件** | 每次调用 `_fopen(a, ccs=UTF-8)` → 写 → `fclose` `(src/util.cpp:104-110)`，靠 `g_log_mu` 串行化 `(src/util.cpp:14, 102)` |
| **`--log-to-file` 的陷阱** | 常驻体已把 stdout/stderr 重定向到同一个日志文件 `(src/main.cpp:356-360)`；此时再让 `log_write` 每行重开同一文件会和 stdout 互锁（外部 `tail` 会 `resource busy`），所以显式把 `lp.log.clear()` 后再 `log_init` `(src/main.cpp:375-383)` |
| **外部文本一律不信任** | `cdn-cache.json` 里的域名/主机名要过 `domain_ok`/`ipv4_ok` `(src/cdn.cpp:78-113)`；hosts 条目要过 `sanitize_entries` `(src/winsys.cpp:367)` |

---

## 7. 本机实测的硬事实

以下为 2026-10-25 在本机用当前构建跑出来的原文（`probe` 连跑两次结论一致）。

1. **9 个端点里 6 个直连可用**：`login.steampowered.com`、`community.akamai.steamstatic.com`、`avatars.steamstatic.com`、`steamcdn-a.akamaihd.net`、`cdn.akamai.steamstatic.com`、`help.steampowered.com`（耗时 250–1360ms）。
2. **`store.steampowered.com`、`api.steampowered.com`、`steamcommunity.com` 三条是 SNI 阻断**（`需节点 · SNI 阻断`，前两条约 4.2s 超时，`steamcommunity.com` 0ms 直接失败）；同一结论在更早一次独立测量里也是 6/3 `(_crew/reports/V1-cli-audit.md:39)`。
3. **社区 / 创意工坊必须走节点**：`steamcommunity.com` 是上面三条之一，且它同时在内置 Steam 后缀表里 `(src/rules_data.cpp:8)`，所以 `whitelist` 模式下会判 `upstream` `(src/rules.cpp:336-340)`；没配节点时 `route_dial` 记一条 WARN 后降级直连 `(src/proxy.cpp:544-557)`，结果就是连不上。
4. **没节点时只能靠 CDN 选边 + hosts 救下载类域名**：`cdn` 缓存当前有 8 个域名、2 个拿到 `best_ip`（`cdn.akamai.steamstatic.com → 23.55.179.9 / 1.12MB/s`、`store.akamai.steamstatic.com → 23.55.179.16`）；`hosts` 当前 0 条托管记录（`svpn status` 输出 `hosts    无 svpn 记录`）。hosts 救不了第 2、3 条那三个域名——它们不在 `cdn.hosts` 里，SNI 转发也不放行。

> 说明：`probe` 的结果会随时间/边缘 IP 漂移（`README.md:225` 记了「store 可能解析到异常边缘，重跑即恢复」）。上面 4 条是本机这一次的实测值，不是永久保证。

---

## 8. 已知设计取舍

| 取舍 | 事实与依据 |
|---|---|
| **`cdn.sni_port` 实际必须是 443** | hosts 条目只能表达 IP、不能带端口，客户端连的永远是 443；`doctor` 会明确报这条 `(src/main.cpp:1077-1080)`，`up` 也会拒绝与本机代理端口撞车 `(src/main.cpp:250-254)` |
| **SNI 起不来只 WARN，不阻断启动** | 443 被占时 `serve_sni` 返回 2，`cmd_serve` 记一条 WARN 继续跑 `(src/main.cpp:416-418, src/sni.cpp:230-234)`；此时 hosts 已指向 127.0.0.1 的域名会连不上 |
| **每连接一个线程，无连接数上限** | `spawn` 直接 `std::thread(...).detach()` `(src/proxy.cpp:917-926)`；`Core::active` 只用于退出时等计数 `(src/proxy.cpp:458, 495-498)`，不做限流 |
| **中继不是「谁断谁关」** | 半关闭语义 + 1h 空闲兜底 `(src/net.cpp:557-559, src/proxy.cpp:34)`，所以半开连接会挂到系统超时 |
| **只有 2 个 CDN 域名做真吞吐** | `kPaths` 里只有 `steamcdn-a.akamaihd.net` 与 `cdn.akamai.steamstatic.com` 有下载路径 `(src/cdn.cpp:39-48)`，其余走 HEAD 只判活 `(src/cdn.cpp:330-357)`，因此列表里的「实测」列显示 `-` |
| **竞速用事件而不是 `select`** | 注释写明：`select` 阻塞不可从别的线程叫醒，输家会把已成功的拨号拖满整个 timeout；`WSAEventSelect` + cancel 事件才能微秒级退出 `(src/net.cpp:108-113)` |
| **`ip_map` 只影响直连分支** | `route_dial` 只有在判定 `direct` 时才走 `dial_direct`，它才读 `ip_map` `(src/proxy.cpp:420-426, 552-557)`；走上游时域名原样交给节点做远端解析 |
| **代理侧不校验证书** | `probe`/`cdn` 用 SChannel 只取握手结果与证书 CN，不阻断；`probe` 的定位是诊断 `(README.md:267)` |
| **前台模式功能不完整** | `up` 不加 `-b` 时不启动 SNI 转发、不启动统计定时落盘 `(src/main.cpp:263-275)`；CDN SNI 模式必须用 `up -b` |
| **`auto_node` / `browser.use_pac` 是死键** | 只解析与回写，无消费点 `(src/config.cpp:173, 188)`；`README.md:220` 已如实声明 |
| **`sni_stop` 是遗留变量** | 声明于 `src/main.cpp:414`，无读取点；SNI 线程只响应进程级 `g_stop` |
