# R2 发布报告 — fix(netutil) DoH 绕过系统代理

状态：**已完成，无卡点**

---

## 1. 测试结果

三个受影响的测试文件，全部 OK。共 **103 个用例通过，0 失败**。

### `tests/test_doh_proxy_bypass.py`（新增）

```
test_env_proxy_is_ignored (__main__.DohProxyBypassTests.test_env_proxy_is_ignored) ... ok
test_non_200_still_raises (__main__.DohProxyBypassTests.test_non_200_still_raises) ... ok
test_opener_has_no_proxy_handler (__main__.DohProxyBypassTests.test_opener_has_no_proxy_handler) ... ok
test_system_proxy_registry_is_ignored (__main__.DohProxyBypassTests.test_system_proxy_registry_is_ignored) ... ok

----------------------------------------------------------------------
Ran 4 tests in 0.011s

OK
```

**结论：OK**（4 passed，exit 0）

### `tests/test_netutil_extra.py`

```
test_all_providers_broken_is_contained (__main__.TestDohDegrade.test_all_providers_broken_is_contained) ... ok
test_answer_not_a_list_falls_through (__main__.TestDohDegrade.test_answer_not_a_list_falls_through) ... ok
test_answer_null_falls_through (__main__.TestDohDegrade.test_answer_null_falls_through) ... ok
test_bad_answers_are_filtered (__main__.TestDohDegrade.test_bad_answers_are_filtered)
data 缺失 / 非字符串 / 不是 IP / 类型不符 → 丢弃，不抛。 ... ok
test_non_200_falls_through (__main__.TestDohDegrade.test_non_200_falls_through) ... ok
test_non_dict_payload_falls_through (__main__.TestDohDegrade.test_non_dict_payload_falls_through) ... ok
test_resolve_merged_never_raises (__main__.TestDohDegrade.test_resolve_merged_never_raises) ... ok
test_resolve_merged_partial_source_labels (__main__.TestDohDegrade.test_resolve_merged_partial_source_labels) ... ok
test_truncated_json_falls_through (__main__.TestDohDegrade.test_truncated_json_falls_through) ... ok
test_253_chars_still_queried (__main__.TestDomainBoundaries.test_253_chars_still_queried) ... ok
test_empty_names (__main__.TestDomainBoundaries.test_empty_names) ... ok
test_overlong_domain_short_circuits (__main__.TestDomainBoundaries.test_overlong_domain_short_circuits) ... ok
test_system_resolve_overlong_returns_empty (__main__.TestDomainBoundaries.test_system_resolve_overlong_returns_empty) ... ok
test_trailing_dot_is_normalized (__main__.TestDomainBoundaries.test_trailing_dot_is_normalized) ... ok
test_empty_payloads_give_empty_headers (__main__.TestHttpHeadMalformed.test_empty_payloads_give_empty_headers) ... ok
test_header_without_colon_is_ignored (__main__.TestHttpHeadMalformed.test_header_without_colon_is_ignored) ... ok
test_malformed_returns_zero (__main__.TestHttpHeadMalformed.test_malformed_returns_zero) ... ok
test_normal_status_line (__main__.TestHttpHeadMalformed.test_normal_status_line) ... ok
test_socket_error_returns_zero (__main__.TestHttpHeadMalformed.test_socket_error_returns_zero) ... ok
test_status_code_boundaries (__main__.TestHttpHeadMalformed.test_status_code_boundaries) ... ok
test_bare_ipv6_with_scheme_raises_clear_error (__main__.TestNodeUrlBoundaries.test_bare_ipv6_with_scheme_raises_clear_error) ... ok
test_bare_ipv6_without_port_still_works (__main__.TestNodeUrlBoundaries.test_bare_ipv6_without_port_still_works)
`2408::1` 是完整地址，不能因为歧义判定被误伤。 ... ok
test_bracketed_ipv6 (__main__.TestNodeUrlBoundaries.test_bracketed_ipv6) ... ok
test_existing_behaviour_kept (__main__.TestNodeUrlBoundaries.test_existing_behaviour_kept) ... ok
test_password_with_at_sign (__main__.TestNodeUrlBoundaries.test_password_with_at_sign) ... ok
test_port_edges_ok (__main__.TestNodeUrlBoundaries.test_port_edges_ok) ... ok
test_port_out_of_range_raises (__main__.TestNodeUrlBoundaries.test_port_out_of_range_raises) ... ok
test_blocked_true (__main__.TestSniBlockTriState.test_blocked_true) ... ok
test_control_ip_cache_and_refresh (__main__.TestSniBlockTriState.test_control_ip_cache_and_refresh) ... ok
test_control_ip_unreachable_clears_cache (__main__.TestSniBlockTriState.test_control_ip_unreachable_clears_cache) ... ok
test_no_control_ip_returns_none (__main__.TestSniBlockTriState.test_no_control_ip_returns_none) ... ok
test_not_blocked_false (__main__.TestSniBlockTriState.test_not_blocked_false) ... ok
test_tls_error_is_none (__main__.TestSniBlockTriState.test_tls_error_is_none) ... ok
test_open_via_direct_failure_propagates (__main__.TestSocketReclaim.test_open_via_direct_failure_propagates) ... ok
test_open_via_http_closes_on_empty_reply (__main__.TestSocketReclaim.test_open_via_http_closes_on_empty_reply) ... ok
test_open_via_http_closes_on_reject (__main__.TestSocketReclaim.test_open_via_http_closes_on_reject) ... ok
test_open_via_socks5_closes_on_protocol_error (__main__.TestSocketReclaim.test_open_via_socks5_closes_on_protocol_error) ... ok
test_tls_over_closes_on_settimeout_error (__main__.TestSocketReclaim.test_tls_over_closes_on_settimeout_error) ... ok
test_tls_over_closes_on_wrap_error (__main__.TestSocketReclaim.test_tls_over_closes_on_wrap_error) ... ok
test_tls_over_returns_wrapped_socket (__main__.TestSocketReclaim.test_tls_over_returns_wrapped_socket) ... ok
test_tls_probe_closes_on_success (__main__.TestSocketReclaim.test_tls_probe_closes_on_success) ... ok
test_tls_probe_closes_raw_on_handshake_error (__main__.TestSocketReclaim.test_tls_probe_closes_raw_on_handshake_error) ... ok

----------------------------------------------------------------------
Ran 42 tests in 0.239s

OK
```

**结论：OK**（42 passed，exit 0）

### `tests/test_netutil.py`

```
test_206_and_throughput (__main__.TestDownloadBench.test_206_and_throughput) ... ok
test_custom_size (__main__.TestDownloadBench.test_custom_size) ... ok
test_factory_exception_is_contained (__main__.TestDownloadBench.test_factory_exception_is_contained) ... ok
test_http_error_reported (__main__.TestDownloadBench.test_http_error_reported) ... ok
test_no_response (__main__.TestDownloadBench.test_no_response) ... ok
test_is_ip (__main__.TestHostHelpers.test_is_ip) ... ok
test_is_private (__main__.TestHostHelpers.test_is_private) ... ok
test_norm_host_brackets (__main__.TestHostHelpers.test_norm_host_brackets) ... ok
test_norm_host_empty (__main__.TestHostHelpers.test_norm_host_empty) ... ok
test_norm_host_lower_strip_dot (__main__.TestHostHelpers.test_norm_host_lower_strip_dot) ... ok
test_direct_uses_create_connection (__main__.TestOpenViaRouting.test_direct_uses_create_connection) ... ok
test_http_dispatch (__main__.TestOpenViaRouting.test_http_dispatch) ... ok
test_socks5_dispatch (__main__.TestOpenViaRouting.test_socks5_dispatch) ... ok
test_bad_scheme_raises (__main__.TestParseNodeUrl.test_bad_scheme_raises) ... ok
test_bare_host_port_defaults_to_socks5 (__main__.TestParseNodeUrl.test_bare_host_port_defaults_to_socks5) ... ok
test_empty_raises (__main__.TestParseNodeUrl.test_empty_raises) ... ok
test_http (__main__.TestParseNodeUrl.test_http) ... ok
test_https_folds_to_http (__main__.TestParseNodeUrl.test_https_folds_to_http) ... ok
test_ipv6_url (__main__.TestParseNodeUrl.test_ipv6_url)
方括号 IPv6 节点 URL（实装已修：不再按第一个冒号切）。 ... ok
test_missing_host_raises (__main__.TestParseNodeUrl.test_missing_host_raises) ... ok
test_missing_port_defaults (__main__.TestParseNodeUrl.test_missing_port_defaults) ... ok
test_non_numeric_port_raises (__main__.TestParseNodeUrl.test_non_numeric_port_raises) ... ok
test_password_with_at_sign (__main__.TestParseNodeUrl.test_password_with_at_sign) ... ok
test_socks5_with_auth (__main__.TestParseNodeUrl.test_socks5_with_auth) ... ok
test_socks5h_alias (__main__.TestParseNodeUrl.test_socks5h_alias) ... ok
test_socks_alias (__main__.TestParseNodeUrl.test_socks_alias) ... ok
test_user_without_password (__main__.TestParseNodeUrl.test_user_without_password) ... ok
test_cache_expires_by_ttl (__main__.TestResolve.test_cache_expires_by_ttl) ... ok
test_doh_all_fail_falls_back_to_system (__main__.TestResolve.test_doh_all_fail_falls_back_to_system) ... ok
test_doh_hit_then_cache (__main__.TestResolve.test_doh_hit_then_cache) ... ok
test_empty_name (__main__.TestResolve.test_empty_name) ... ok
test_everything_fails_returns_fail (__main__.TestResolve.test_everything_fails_returns_fail) ... ok
test_explicit_providers_respected (__main__.TestResolve.test_explicit_providers_respected) ... ok
test_literal_ip_short_circuits (__main__.TestResolve.test_literal_ip_short_circuits) ... ok
test_auth_rejected_raises (__main__.TestSocks5Connect.test_auth_rejected_raises) ... ok
test_connect_refused_raises_and_closes (__main__.TestSocks5Connect.test_connect_refused_raises_and_closes) ... ok
test_literal_ipv4_uses_atyp1 (__main__.TestSocks5Connect.test_literal_ipv4_uses_atyp1) ... ok
test_literal_ipv6_uses_atyp4 (__main__.TestSocks5Connect.test_literal_ipv6_uses_atyp4) ... ok
test_no_auth_domain_atyp3 (__main__.TestSocks5Connect.test_no_auth_domain_atyp3) ... ok
test_not_socks5_raises (__main__.TestSocks5Connect.test_not_socks5_raises) ... ok
test_reply_domain_atyp_is_drained (__main__.TestSocks5Connect.test_reply_domain_atyp_is_drained) ... ok
test_reply_ipv6_atyp_is_drained (__main__.TestSocks5Connect.test_reply_ipv6_atyp_is_drained) ... ok
test_unreachable_upstream_raises (__main__.TestSocks5Connect.test_unreachable_upstream_raises) ... ok
test_username_password (__main__.TestSocks5Connect.test_username_password) ... ok
test_dedup_and_order (__main__.TestSystemResolve.test_dedup_and_order) ... ok
test_oserror_returns_empty (__main__.TestSystemResolve.test_oserror_returns_empty) ... ok
test_resolve_merged_marks_both_sources (__main__.TestSystemResolve.test_resolve_merged_marks_both_sources) ... ok
test_resolve_merged_system_only (__main__.TestSystemResolve.test_resolve_merged_system_only) ... ok
test_skips_literal_and_empty (__main__.TestSystemResolve.test_skips_literal_and_empty) ... ok
test_ok_when_handshake_succeeds (__main__.TestTlsProbe.test_ok_when_handshake_succeeds) ... ok
test_other_oserror_is_tls (__main__.TestTlsProbe.test_other_oserror_is_tls) ... ok
test_sni_rst (__main__.TestTlsProbe.test_sni_rst) ... ok
test_tcp_probe_reset_and_ok (__main__.TestTlsProbe.test_tcp_probe_reset_and_ok) ... ok
test_tcp_reset (__main__.TestTlsProbe.test_tcp_reset) ... ok
test_tcp_timeout (__main__.TestTlsProbe.test_tcp_timeout) ... ok
test_tls_error (__main__.TestTlsProbe.test_tls_error) ... ok
test_tls_timeout (__main__.TestTlsProbe.test_tls_timeout) ... ok

----------------------------------------------------------------------
Ran 57 tests in 0.697s

OK
```

**结论：OK**（57 passed，exit 0）

---

## 2. `git show --stat HEAD` 原文

```
commit 037a78f35df8ed32b98d6e35ee66aa054b798fe6
Author: zhe-du <zhe-du@users.noreply.github.com>
Date:   Sun Oct 25 10:37:33 2026 +0800

    fix(netutil): DoH 必须绕过系统代理，否则 up --system-proxy 后解析自我锁定
    
    `up --system-proxy` 会把 WinINET 的 ProxyServer 指到 127.0.0.1:7900。
    urllib 默认读这份配置，于是每条 DoH 请求都绕回 svpn 自己的 HTTP 入口，
    在代理层等一个永远不会来的上游响应 —— 实测每个域名卡 12s，百度解析也挂。
    
    改用 `build_opener(ProxyHandler({}))` 的空 opener 发 DoH，无视环境变量与
    注册表里的代理设置。新增 tests/test_doh_proxy_bypass.py 焊死该行为。
    
    - 影响面：仅 cdn 选边的解析路径；HTTP/SOCKS5 转发逻辑未变
    - 关联离线用例全绿

 svpn/netutil.py                |  8 ++++-
 tests/test_doh_proxy_bypass.py | 79 ++++++++++++++++++++++++++++++++++++++++++
 tests/test_netutil_extra.py    | 11 +++---
 3 files changed, 92 insertions(+), 6 deletions(-)
```

---

## 3. 提交 SHA 与标题

- **完整 40 位 SHA**：`037a78f35df8ed32b98d6e35ee66aa054b798fe6`
- **标题行**：`fix(netutil): DoH 必须绕过系统代理，否则 up --system-proxy 后解析自我锁定`
- 作者：`zhe-du <zhe-du@users.noreply.github.com>`
- 变更规模：3 files changed, 92 insertions(+), 6 deletions(-)

---

## 4. 仓库 / 分支 / 工作区状态

- **仓库 URL**：`git@github.com:zhe-du/svpn.git`（SSH）；网页端 `https://github.com/zhe-du/svpn`
- **分支**：`main`
- **推送结果**：`f2bde5e..037a78f  main -> main`

`git status -sb` 原文：

```
## main...origin/main
```

> 无任何 `M` / `A` / `??` 行，工作区干净且与远端同步。

三方一致校验：

| 来源 | SHA |
|---|---|
| `git rev-parse HEAD` | `037a78f35df8ed32b98d6e35ee66aa054b798fe6` |
| `git ls-remote origin refs/heads/main` | `037a78f35df8ed32b98d6e35ee66aa054b798fe6` |
| `gh api ... .sha` | `037a78f35df8ed32b98d6e35ee66aa054b798fe6` |

**三者完全相同 → 推送确实落地，可追溯。**

---

## 5. 提交网页链接

`gh api "repos/zhe-du/svpn/commits/037a78f..." --jq '.sha, .html_url'` 输出：

```
037a78f35df8ed32b98d6e35ee66aa054b798fe6
https://github.com/zhe-du/svpn/commit/037a78f35df8ed32b98d6e35ee66aa054b798fe6
```

---

## 6. 卡点

**无。**

执行前状态与 Lead 的判断一致：HEAD 停在 `f2bde5e`，工作区确有 3 项未提交改动，说明前一个同名 agent 确实未留下任何提交。

合规自查（对照硬性约束）：

1. 未使用 `git init` / `gh repo create` / `--amend` / `push --force` — 全新单条提交直接快进推送。
2. `git add -A` 后复核 `git status --short`，暂存区仅 3 项：`M svpn/netutil.py`、`A tests/test_doh_proxy_bypass.py`、`M tests/test_netutil_extra.py`；额外用 `grep -Ei 'config\.json|^runtime/|\.log$|backup'` 扫描暂存清单，返回 `NO_FORBIDDEN_FILES`。**未提交任何配置或备份文件。**
3. 全程 `unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY`，命令导出 `PYTHONUTF8=1`。
4. `gh` 通过 `export PATH="$PATH:/d/app/Lapp/gh/bin"` 调用。
5. 提交身份为仓库局部配置 `zhe-du <zhe-du@users.noreply.github.com>`，未动全局。

未修改任何源码，仅执行测试、提交、推送与校验。

---

# 附：C++ 二进制门禁核对（应 V1-verify 要求）

## 结论：**当前 `svpn.exe` 不可发布**

> ⚠️ 本节写于 10:44，产物此后又变了两次。**以文末「10:47 更新」为准**，最终结论不变
> （仍不可发布），但对象已从 `ec6d47eb` 换成 `e634fe15`。

V1-verify 给出的两份已审计基线：

| md5 | 时间 | 大小 | 状态 |
|---|---|---|---|
| `702be7f89166b51e7b16216acb5199dd` | 10:42:03 | 1,677,824 B | 已被覆盖，磁盘上不存在 |
| `439fd9dcd000460867f3132934d144fd` | 10:40:29 | 1,677,824 B | 已被覆盖，磁盘上不存在 |

工作区现存二进制：

```
ec6d47eb4b1828f059598b538b22afe1  *svpn.exe   1,677,824 B   mtime=2026-10-25 10:43:27
```

**md5 不匹配任何一份已审计基线。** 全盘 `find` 仅此一个 `svpn*.exe`，两份基线已无副本，
无法回退到已验版本。

### 时间线证据

```
2026-10-25 10:42:03   ← 基线 A 构建（702be7f8）
2026-10-25 10:42:39   ← src/main.cpp 被修改
2026-10-25 10:43:27   ← 当前 svpn.exe 重新构建（ec6d47eb）
2026-10-25 10:45:03   ← src/main.cpp 再次被修改（审计期间仍在改）
```

即：当前 exe **既非审计版本，且已落后于当前源码**。属于 V1-verify 明确要求「别直接发」的情形。

### 补充：`svpn-cpp` 不是 git 仓库

`git status` 返回 `fatal: not a git repository`。该目录无版本控制，无法用
SHA 追溯「exe 对应哪一版源码」，也无法回滚。这本身是发版可追溯性的缺口。

## 三条门禁实测（用当前 ec6d47eb，临时目录副本）

按 V1-verify 建议复制到临时目录执行，未污染工作区。`up` 路径不含 `apply_hosts`、
未启用 `--system-proxy`，故不改 hosts、不动 WinINET。

**在单次 shell 会话内连续执行时，三条全部符合预期：**

```
=== GATE1 ===
[+] 后台已启动 pid=4072  socks5://127.0.0.1:17899  http://127.0.0.1:7900
RC1=0     pid file: 4072

=== GATE2 ===
[=] 已在运行（端口被占用）；改配置请先 svpn down
RC2=0     pid file after gate2: 4072      process alive: 1

=== GATE3 ===
[+] 已停止 pid=4072
[!] 系统代理没动过：没有备份（可能从没接管过）
RC3=0     pid file after down: MISSING    process alive: 0
listeners on 17899/7899/7900: NONE_LISTENING
```

- `up -b -p 17899` → `[+] 后台已启动`，rc=0 ✔
- 重复 `up -b` → `[=] 已在运行（端口被占用）`，rc=0，**pid 文件未被删除**，进程仍存活 ✔ ——
  即 V1-verify 所称的 P1 回归点，当前构建**已修复**
- `down` → `[+] 已停止 pid=4072`，rc=0，pid 文件清除，进程退出，三个端口全部释放 ✔

### 一个实验陷阱，建议写进审计报告

首次尝试把三条门禁**拆成多次 tool 调用**执行时，Gate3 输出的是
`[=] 没有在运行的实例`（而非 `[+] 已停止 pid=`），看似 P1 复发。
复现后确认，这是**执行环境副作用**：后台子进程在 shell 会话结束时被回收，
而 pid 文件残留在后续会话里指向不存在的进程，`down` 于是判为「无实例」。

同一序列放进单次 shell 会话内执行，三条全绿。**结论：该门禁必须在同一会话内连续跑，
跨会话拆开会产生假阳性。** 若 V1-verify 早期观察到「down 报没有实例」，
建议区分其中哪部分是代码缺陷、哪部分是该陷阱。

---

# 10:47 更新：产物第二次被覆盖，结论不变

V1-verify 在 10:46 前后补充「三份构建等价，`ec6d47eb` 可用」，但该结论在送达时**已经过期**。

## 实际状态

```
e634fe15067808ccf2ba0dad30d55d97  *svpn.exe   1,678,848 B   mtime=2026-10-25 10:46:27
```

- 不在 V1 给出的三份 hash 之内
- **大小也变了**：`1,677,824 → 1,678,848`（+1024 B），是新代码产物，不是重编副本
- `ec6d47eb` 已被 10:46:27 这次构建覆盖，磁盘上不存在

## 「源码自 10:42:39 起未改动」不成立

```
src/main.cpp   10:42:39   48,274 B
src/main.cpp   10:45:03   49,232 B   ← +958 B
```

V1 的「源码未变」结论与实测不符。完整时间线：

| 时间 | 事件 | md5 |
|---|---|---|
| 10:42:03 | 基线 A 构建 | `702be7f8…` |
| 10:42:39 | `main.cpp` 48,274 B | — |
| 10:43:27 | 第 2 次构建 | `ec6d47eb…`（现已消失） |
| 10:45:03 | `main.cpp` 49,232 B（+958 B） | — |
| 10:46:27 | 第 3 次构建 | `e634fe15…`（当前） |

**这是第三轮「审计完就被覆盖」**。只要构建在审计窗口内持续进行，任何 hash 结论在送达时都可能失效。

## 三条门禁实测（`e634fe15`，临时目录副本，单会话连跑）

```
=== GATE1 ===
[+] 后台已启动 pid=23128  socks5://127.0.0.1:17899  http://127.0.0.1:7900
RC1=0     pid file: 23128

=== GATE2 ===
[=] 已在运行（端口被占用）；改配置请先 svpn down
RC2=0     pid file after gate2: 23128（未变）   alive: 1

=== GATE3 ===
[+] 已停止 pid=23128
RC3=0     pid file after down: MISSING   alive: 0
          17899/7899/7900 LISTENING = NONE
```

- `up -b -p 17899` → 后台启动，rc=0 ✔
- 重复 `up -b` → `[=] 已在运行（端口被占用）`，rc=0，**pid 文件未被删除**，进程存活 ✔（P1 回归点未复发）
- `down` → `[+] 已停止`，rc=0，pid 文件清除，进程退出，端口全释放 ✔

**但门禁通过 ≠ 可发布。** 门禁只覆盖 CLI 生命周期，不覆盖 V1 的只读审计面（doctor 三态、cdn、node、pac 等）。仍须 V1 对 `e634fe15` 签字。

## 源码指纹（供冻结对齐）

目录无 git，「源码未变」只能靠内容指纹而非 mtime 口头确认。当前值：

```
ef09fc1c11844a04a76aae86b94c871b  src/main.cpp     49,232 B
cca903e4c3a33d973567ad502b27ef19  src/svpn.hpp
5c7aea0555ee6e2832655d31918d6f68  src/config.cpp
eabe35a7df866cfa16f6539d8fb4bb7c  src/cdn.cpp
b31a2047ce204853dde701e098874c70  src/tls.cpp
a3a41a3ba9b2d7ad730d29e242368795  src/proxy.cpp
2b573bf199fd6d32ffcd9beebc4af9f4  src/net.cpp
2c16bae9ca66595f9fb489d86f481ffd  src/winsys.cpp
```

## 建议的收口方式

1. 构建方宣布源码冻结，停止修改 `src/`；
2. 立即把 `svpn.exe` 复制为不可变副本（如 `dist/svpn-e634fe15.exe`），此后审计与打包一律针对副本；
3. V1 审副本并出具带 md5 的结论。

如此「审计对象」与「发布对象」为同一文件，物理上无法错位。

## 凭据排查（V1 提出，已核）

V1 担心模板里残留真实上游凭据。实测**不存在该问题**：

```
config.example.json:  "host": "NODE_HOST", "username": "", "password": ""
config.json:          "host": "NODE_HOST", "username": "", "password": ""
```

两者 `upstream.host` 均为 `NODE_HOST` 占位符，账号密码为空。当前工作区 `config.json` 未含明文凭据。

`config.json` 与 `runtime/` 仍**不应进入发布产物**——理由从「含密钥」变为「含本机运行时状态」：
`runtime/` 下有 `cdn-cache.json`、`probe.json`、`stats.json`、`svpn.log`、`wininet-backup.json`，
其中 `wininet-backup.json` 记录本机 WinINET 代理原始配置，`svpn.log` 含本机运行痕迹。
（V1 提到的 `hosts.backup` / `svpn.pid` 当前目录中不存在。）

---

# 10:50 更新：第三轮签字仍指向已消失产物

V1-verify 在 10:49 前后给出「最终确认：发 `ec6d47eb`」，并附独立全量复核结果（含 `effective.json` 三项检查）
全部通过。但该文件**在磁盘上不存在**，签字对象不可达。

## 三次签发、三个已消失的产物

| 时间 | md5 | V1 状态 | 磁盘现状 |
|---|---|---|---|
| 10:40:29 | `439fd9dc…` | 早期通过 | 已被覆盖 |
| 10:42:03 | `702be7f8…` | 基线 A | 已被覆盖 |
| 10:43:27 | `ec6d47eb…` | **最终签字** | 已被覆盖 |
| 10:46:27 | `e634fe15…` | 未审 | **唯一存在** |

`find . -iname "svpn*.exe"` 全盘仅 `./svpn.exe` 一个，md5 恒为 `e634fe15…`。

## `ec6d47eb` 已永久不可复现

不只是产物被覆盖——**产出它的源码同样不存在**：

```
ec6d47eb 对应  src/main.cpp  48,274 B  mtime 10:42:39
当前文件       src/main.cpp  49,232 B  mtime 10:45:03  md5 ef09fc1c…
```

该目录无版本控制（`fatal: not a git repository`），无法回滚到 48,274 B 版本。
因此「发布 `ec6d47eb`」在物理上不可执行：既无产物，亦无源码。

## 在 `e634fe15` 上复现 V1 全套检查：全绿

因 `7899/7900` 被另一残留实例（PID 9008）占用，本次改用独立端口
（`http_port=17900`），其余条件与 V1 一致。

```
GATE1  up -b -p 17899 -m blacklist
       [+] 后台已启动 pid=18980  socks5://127.0.0.1:17899  http://127.0.0.1:17900   RC=0
       pidfile=18980
       effective.json = { listen_port:17899, http_port:17900, mode:"blacklist",
                          upstream_configured:false }        eff_md5=102b8289…

GATE2  up -b
       [=] 已在运行（端口被占用）；改配置请先 svpn down                                RC=0
       pidfile=18980（未变）
       eff_md5=102b8289…（逐字未变）
       本目录实例数 = 1

status 进程 pid=18980 / 17899 监听中 / 17900 监听中 / blacklist

GATE3  down
       [+] 已停止 pid=18980                                                            RC=0
       pidfile=MISSING   alive=0   listeners(17899/17900)=NONE
```

V1 列举的各项均一致：`effective.json` 内容正确、二次 `up` 后逐字未变、单实例、
`down` 三清理（pid / effective.json / 端口）。P3（`status` 节点行不读 `effective.json`）
同样复现，不阻断发布。

**结论：`e634fe15` 与本轮签字对象 `ec6d47eb` 在该检查集下行为一致。**

## 环境风险：残留实例会让门禁恒真

机器上存在 `PID 9008` 的 `svpn.exe` 占用 `7899` + `7900`（非本次测试产生，
本次所有实例均已 `down` 并释放端口）。影响：

> 任何使用默认 `http_port=7900` 的 `up`，都会因端口被占直接返回
> `[=] 已在运行（端口被占用）` 且 rc=0。

这意味着「第二次 `up` 应报已在运行」这条断言在该实例存在时**恒为真**，
即便被测二进制已损坏。任何在此窗口内完成的审计结论都应复核其有效性。

建议：审计与发版前先确认 `7899/7900` 无监听，或统一改独立端口。

## 待决事项

1. **发布哪一份**：`e634fe15`（唯一存在，已过门禁，待 V1 签字）；`ec6d47eb` 已不可行。
2. `svpn-cpp` 是否纳入版本控制——三轮错位的根因，无 VCS 则无法追溯产物与源码的对应关系。
3. 打包归属与产物清单（`runtime/`、`config.json` 排除项）。

在 1 明确前，**不产出任何 C++ 发布物**。

---

# 10:52 发版完成：`e634fe15`

V1-verify 改签 `e634fe15067808ccf2ba0dad30d55d97`（与磁盘现存产物一致）后执行发布。

## 产物

| 项 | 值 |
|---|---|
| 目录 | `dist/svpn-1.1.0-win64/` |
| 压缩包 | `dist/svpn-1.1.0-win64.zip` |
| 包 MD5 | `8884716eee3f8735504d89195b1ff312` |
| 包大小 | 700,379 bytes |
| exe MD5 | `e634fe15067808ccf2ba0dad30d55d97`（与 V1 签字一致） |
| exe 大小 | 1,678,848 bytes |

包内 7 个文件：

```
      2084  config.example.json
      7340  docs/BUILD.md
      1063  LICENSE
      3971  MANIFEST.txt
     22219  README.md
       823  run.cmd
   1678848  svpn.exe
```

`zipfile.testzip()` → **integrity OK**。

## 发布前校验

**校验 1 —— 哈希**
```
$ md5sum svpn.exe
e634fe15067808ccf2ba0dad30d55d97 *svpn.exe
预期 e634fe15067808ccf2ba0dad30d55d97 → MD5_OK
```

**校验 2 —— 门禁（按 V1 建议使用动态空闲端口，避开本机残留实例占用的 7899/7900）**
```
GATE1  up -b -m blacklist
       [+] 后台已启动 pid=27476  socks5://127.0.0.1:64166  http://127.0.0.1:64167   rc=0
       pidfile=27476
       effective.json md5=9d43b8391b2b6788faed79a4f3097e20

GATE2  up -b
       [=] 已在运行（端口被占用）；改配置请先 svpn down                                rc=0
       pidfile=27476（未变）
       effective.json md5=9d43b839…（逐字未变）

GATE3  down
       [+] 已停止 pid=27476                                                           rc=0
       pidfile=MISSING   alive=0   listeners(64166/64167)=NONE
```

**校验 3 —— 包内容**
泄露扫描（`config.json` / `runtime/` / `*.log` / `*.pid` / `effective.json` /
`hosts.backup` / `wininet-backup.json`）→ **CLEAN**，零命中。
敏感信息扫描（本机路径、用户名、token）→ 仅命中 `config.example.json` 中两处
空口令字段 `"password": ""`，无真实凭据。

## 交付物端到端验证（解压后重跑）

不以工作区 exe 为准，而是解压 zip 后在全新目录完整走一遍：

```
解压后 svpn.exe md5 = e634fe15067808ccf2ba0dad30d55d97   与签字一致

svpn.exe --help     rc=0
svpn.exe init       [+] 已写入 ...\config.json            rc=0
                    [i] SOCKS5 127.0.0.1:7899 / HTTP 127.0.0.1:7900
svpn.exe up -b -m blacklist
                    [+] 后台已启动 pid=16296               rc=0
svpn.exe status     进程 运行中 pid=16296 / 64200 监听中 / 64201 监听中 / blacklist
svpn.exe down       [+] 已停止 pid=16296                  rc=0
                    alive=0   ports=NONE
```

**结论：交付物自身可用**，非仅"构建目录里的那个 exe 可用"。
`init` 能自动生成 `config.json`，故包内不含配置文件不影响开箱使用。

## 包内附带 MANIFEST.txt

写入 exe 哈希、14 个源文件的 md5 与大小、门禁实测记录、已知 P3、包内容说明。
使用者可用 `certutil -hashfile svpn.exe MD5` 自行校验。

## 遗留（不阻断发布）

1. **`svpn-cpp` 无版本控制** — 三轮「签字对象已被覆盖」的根因。建议发版后立即
   `git init` 并把当前源码与 `dist/` 入库，否则本包与源码的对应关系仅靠
   `MANIFEST.txt` 维持，无密码学或 VCS 保证。
2. **`dist/` 未加入 `.gitignore`** — 若后续纳入 git，需先补一行，避免把二进制提交进库。
3. **P3**：`status` 节点行在 `--node` 场景下可能显示「未配置」，仅显示问题。
4. 机器上曾有 `PID 9008` 残留实例占用 7899/7900，本次已绕开；后续验收建议继续用动态端口。
