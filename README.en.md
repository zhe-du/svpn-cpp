# svpn (C++) — local split routing for Steam

> 中文版见 [README.md](README.md)

In one sentence: it runs a **Steam-only proxy** on your machine (SOCKS5 + HTTP), routes Steam traffic to a node you choose by destination domain, and adds a CDN edge picker that pins downloads and updates to whichever Akamai edge is fastest right now. **It works with no node configured.**

Windows 10/11 x64, **two statically linked executables with no third-party runtime DLLs**: `svpn.exe` (console: CLI plus `svpn.exe gui`) and `svpn-gui.exe` (window subsystem, so double-clicking does not flash a console), each about 1.7 MB. No Python, no .NET, no redistributables — copy and run.

This is the native Windows rewrite of [svpn](https://github.com/zhe-du/svpn) (the Python version): same command names, same option semantics, and `config.json` plus `runtime/cdn-cache.json` are compatible in both directions.

The release package ships `SHA256SUMS.txt` and `RELEASE.txt`. Verify with `certutil -hashfile svpn.exe SHA256` and compare against the `svpn.exe` line there; if they differ, do not run it. Rebuilding the same source yields different hashes — that is expected. Build steps are in [docs/BUILD.md](docs/BUILD.md).

## What it does not do

- **It is not a VPN and has no TUN adapter.** Only SOCKS5 / HTTP listeners (default `127.0.0.1:7899` and `:7900`). Programs that ignore proxy settings cannot use it.
- **It ships no servers and no nodes.** `upstream` is the placeholder `NODE_HOST:1080` out of the box, so without your input it is pure direct-connect. It decides *which path Steam traffic takes*, not *how to leave the country*.
- **It does not proxy UDP.** The SOCKS5 side implements CONNECT only (`cmd != 0x01` is rejected), so QUIC/HTTP3 and UDP game traffic are out of scope.
- **It does no TLS interception.** No root certificate, no certificate store changes. hosts and SNI forwarding are plain forwarding; TLS stays end-to-end.
- **It installs no service, adds no autostart entry, injects into no process, loads no driver.**

---

## Quick start (4 commands)

```bat
svpn.exe init         :: create config.json (skipped if present; --force overwrites)
svpn.exe up -b        :: start SOCKS5 127.0.0.1:7899 + HTTP 127.0.0.1:7900 in the background
                      :: then point browser/downloader at socks5://127.0.0.1:7899 or http://127.0.0.1:7900
svpn.exe down         :: stop the process and restore the system proxy
```

To also point the **system proxy** (WinINET — browsers and the Steam client follow it) at this machine:

```bat
svpn.exe up -b --system-proxy   :: take over while starting (backed up first, restored by down)
svpn.exe system on              :: or, with the service already running, take over now
svpn.exe system show            :: inspect ProxyEnable / ProxyServer / AutoConfigURL
svpn.exe system off             :: restore from the backup (down also restores it)
```

Output prefixes: `[+]` success, `[i]` information, `[!]` needs your attention, `[=` no action needed.

You can also double-click **`run.cmd`**: it runs `init` when `config.json` is missing, then `up -b`, waits two seconds, prints one `status`, and holds the window open with `pause`.

---

## Subcommands (14)

No arguments prints usage and returns `2`. `-h` / `--help` / `help` anywhere prints usage and returns `0`. `svpn.exe -v` (or `--version`) prints the version.

| Command | Options | What it does | Exit code |
|---|---|---|---|
| `init` | `--force` | Write `config.json` from the default template; skip if it exists | `0` / `1` (write failed) |
| `up` | `-b` `--background`<br>`-p` `--port <n>`<br>`-m` `--mode <whitelist\|all\|blacklist>`<br>`--node <socks5://…\|http://…>`<br>`--system-proxy` | Start the local proxy. Without `-b` it stays in the foreground (Ctrl+C to quit); with `-b` it spawns a background process and writes `runtime/svpn.pid` | `0` started or already running / `1` port conflict, start failure / `2` bad arguments |
| `_serve` | `--log-to-file`<br>`--system-proxy`<br>`--listen-port` `--mode` `--node` | **Internal.** The resident process spawned by `up -b`, receiving the parent's overrides. Do not call it directly | `0` / `1` |
| `down` | `--keep-system`<br>`--clear-hosts` | Kill the PID in `runtime/svpn.pid`; restore the system proxy by default; `--clear-hosts` also removes the managed hosts block (triggers UAC) | always `0` |
| `status` | — | Process, both ports, node, mode (**ports / mode / node are the values actually in effect**), system proxy, CDN cache age, hosts entry count, last stats | `0` |
| `probe` | `--node <url>` / `--node none`<br>`--timeout <seconds>` (default 6)<br>`--json`<br>`--lenient` | TCP + TLS probing of 9 Steam endpoints, direct and through the node | `0` all reachable / `1` something "currently unavailable" |
| `node` | `scan`<br>`set <url>`<br>`clear` | Scan common local proxy ports / store a node (Steam connectivity tested first) / clear back to pure direct | `0` / `1` unusable / `2` bad arguments |
| `cdn` | `--refresh`<br>`--no-bench`<br>`--domain <domain>`<br>`--timeout <seconds>` (default 8) | DoH resolution + TLS handshake race (+ a 1MB throughput probe); results go to `runtime/cdn-cache.json` | `0` |
| `hosts` | `apply` / `clear` (**omitted means `apply`**)<br>`--no-elevate`<br>`--timeout <seconds>` (default 8) | Add or remove the marked managed block in the hosts file; requests elevation when not admin | `0` / `1` |
| `system` | `on` / `off` / `show` | Take over / restore / inspect the WinINET proxy (writes `HKCU` only) | `0` / `1` / `2` |
| `gui` | — (`--gui` is a synonym) | Open the graphical interface; every button calls the same `dispatch_cli()`, so behaviour matches the CLI exactly | `0` |
| `pac` | `--apply` | Generate `runtime/svpn.pac`; `--apply` also writes it into `AutoConfigURL` | `0` / `1` |
| `steam` | `--system`<br>`--exe <path>` | Launch Steam (`--system` points the system proxy here first); returns `2` when `steam.exe` is not found, which is normal | `0` / `1` / `2` |
| `doctor` | — | Health check: `config.json` exists **and parses**, unknown keys, port occupancy, stale CDN cache, node reachability, admin rights | `0` clean / `1` problems |

`svpn-gui.exe` is the same program built for the window subsystem (no console window on double-click); `svpn.exe gui` is equivalent.

Details (checked against the current build, not inferred):

- **`hosts` with no subcommand equals `hosts apply`**, which edits `C:\Windows\System32\drivers\etc\hosts` directly (UAC prompt when not admin), **with no second confirmation**. Undo with `hosts clear`.
- **`up -b` forwards its overrides to the background process**: `-p/--port`, `-m/--mode`, `--node` are passed down as `--listen-port` / `--mode` / `--node`, so `up -b -p 17899` really listens on 17899 (the parent waits until that port is ready). The resident process writes the **effective** port/mode/node into `runtime/effective.json`, which `status` and `down` read — so `status` reports what is running, not stale `config.json` values.
- **Overrides are not written back to `config.json`**: `-p/-m/--node` apply to this run only; the next bare `up` returns to the file's values. For a permanent change edit `config.json` or use `node set` (which persists).
- **`up -b --system-proxy` really takes over the system proxy**: back up WinINET, set `ProxyServer = 127.0.0.1:<listen_port>`, print `[+] system proxy now points at 127.0.0.1:<port>`; `down` / `system off` restore from the backup.
- **`-h` / `--help` / `help` anywhere only prints usage** (rc=0) and never runs the command — `svpn.exe probe -h` does not probe.
- **`up` does no node auto-discovery.** `auto_node` is parsed but unused for decisions; run `svpn.exe node scan`.
- `probe --node` accepts `socks5://…` / `http://…` (parsed like `node set`) or the literal `none` (direct only); it **affects this probe only and is not persisted**. Use `svpn.exe node set` for a lasting change.
- **`pac` always writes `runtime/svpn.pac`**; only `--apply` also changes `AutoConfigURL`. **Unrecognised flags are silently ignored**, so the legacy `pac --write` does nothing extra and equals `pac` (no longer in the help text).
- When already running, `up` reports "already running" and returns `0` — it **does not restart**. `down` then `up` to apply new settings.
- Every command that writes hosts (`hosts` / `hosts apply` / `hosts clear` / `down --clear-hosts`) raises one UAC prompt when not admin. If you decline, run it from an elevated terminal with `--no-elevate`.

### `probe`: the 9 endpoints and its exit codes

`store.steampowered.com` / `login.steampowered.com` / `api.steampowered.com` / `steamcommunity.com` / `community.akamai.steamstatic.com` / `avatars.steamstatic.com` / `steamcdn-a.akamaihd.net` / `cdn.akamai.steamstatic.com` / `help.steampowered.com` (all on 443).

Each row has exactly one of four verdicts:

| Verdict | Meaning |
|---|---|
| `direct OK` | Direct TLS handshake succeeded |
| `needs node · OK` | Direct failed, but the configured upstream works |
| `needs node · SNI blocked` | Direct failed and **no node is configured** — only says "you need a node", not a diagnosis |
| `unreachable · check` | A node is configured, and neither direct nor the node works |

The summary line has four numbers; three are exclusive buckets and the fourth is derived:

```
[i] direct 7 / needs node 2 / dead 0 / currently unavailable 2
```

- `direct` — works over a direct connection;
- `needs node` — requires a node, covering **two cases**: a node is configured and works, or no node is configured at all (direct failure can then only be "needs node");
- `dead` — a node is configured but neither direct nor the node works;
- `currently unavailable = needs node + dead`, i.e. "how many endpoints this configuration cannot reach right now", not a fourth bucket.

Exit codes: `0` = everything reachable; `1` = at least one endpoint "currently unavailable". **`--lenient` judges by dead endpoints only** (`dead > 0` returns 1), so with no node configured `--lenient` returns `0` — exactly how it differs from the default, and what CI wants when community/avatar endpoints may be unreachable.

> The `--json` contract is **JSON on stdout and nothing else** (so `svpn.exe probe --json | jq .` works); the table, the `[i] node:` note and the summary line all move to **stderr**. Add `2>&1` to see both. `runtime/probe.json` holds the same pure JSON.

---

## Configuration (`config.json`)

`svpn.exe init` writes `config.json` **next to the executable**. It has **exactly 12 top-level keys**, matching the Python version key for key:

| Key | Default | Notes |
|---|---|---|
| `listen_host` | `127.0.0.1` | Loopback only; empty falls back to loopback. Configure `auth` before changing to `0.0.0.0` |
| `listen_port` | `7899` | SOCKS5 port |
| `http_port` | `7900` | HTTP port (CONNECT + absolute-URI); `0` disables the HTTP listener |
| `mode` | `whitelist` | `whitelist`: only the Steam/Valve list goes through the node; `all`: everything; `blacklist`: domestic sites direct, the rest through the node |
| `connect_timeout` | `12` | Connect and handshake timeout in seconds; non-positive or >300 falls back to the default |
| `auto_node` | `true` | Reserved key (unused for decisions in this version) |
| `upstream` | `{kind:"socks5", host:"NODE_HOST", port:1080, username:"", password:""}` | `kind` is `socks5` / `http` / `direct`; `host` equal to `NODE_HOST` means "not configured" |
| `auth` | empty | Local authentication: SOCKS5 username/password (RFC 1929) + HTTP Basic; empty means no auth |
| `browser` | `{apply_system_proxy:true, use_pac:false, bypass:"<local>;…;*.cn"}` | WinINET takeover toggle, PAC toggle, bypass list |
| `cdn` | `{enabled:false, refresh_hours:6, sni_port:443, hosts:[8 domains]}` | The two CDN strategies + edge-cache TTL + SNI allowlist |
| `proxy_hosts` | `[]` | Domains **forced through the node** (exact match, beats the built-in rules) |
| `direct_hosts` | `[]` | Domains **forced direct** (exact match, highest priority; on conflict with `proxy_hosts`, direct wins) |

Normalisation rules (the `merge_config()` contract):

- Top-level keys starting with `_` (such as `_note`) or not in the list above are **dropped**, with one debug log line and no error;
- Nested objects merge by sub-key (`{"cdn":{"refresh_hours":2}}` touches only that item); unknown nested sub-keys are dropped too;
- Numeric keys given `null` / `""` / `"abc"` / `NaN` / an out-of-range value **fall back to the default** without throwing;
- Boolean keys accept the strings `true/false/1/0/yes/no/on/off` (case and whitespace insensitive);
- Lists (`proxy_hosts` / `direct_hosts` / `cdn.hosts`) may also be written as a comma- or space-separated string and are normalised into an array.

### Two optional advanced keys (not in the template, active only when present)

| Key | Default | Notes |
|---|---|---|
| `proxy_suffixes` | built-in Steam suffix list | **Replaces** the "goes through the node" suffixes (routing decisions and the generated PAC follow it) |
| `direct_suffixes` | built-in domestic direct list (only used by the default policy in `blacklist` mode) | **Replaces** the "direct" suffixes |

- They **replace, not append**: `["corp.example.com"]` means "only this one goes through the node";
- Empty or `[]` is the same as absent and falls back to the built-in list;
- Under `mode: blacklist`, an empty `direct_suffixes` falls back to the built-in domestic list (otherwise "domestic goes direct" would be an empty promise);
- `init --force` rewrites `config.json` from the template and drops them; run `down` then `up` after editing.

**Priority** (first match wins):

```
direct_hosts (exact) > proxy_hosts (exact) > loopback / local names > Valve CIDRs for literal IPs
> *_suffixes > built-in Steam suffixes/CIDRs > mode fallback
```

### Two ways to accelerate CDN (`cdn.enabled`)

| | `false` (default) | `true` (transparent SNI forwarding) |
|---|---|---|
| What `hosts apply` writes | Each CDN domain → that domain's best measured edge IP | All `cdn.hosts` → `127.0.0.1` |
| Who does the work | The OS resolves straight to the edge | The SNI forwarder inside the resident `svpn.exe up`: it reads the SNI from the ClientHello and, on a cache hit, connects to the best IP, otherwise uses the upstream |
| Needs a resident process | No — close it once hosts is written | **Yes.** Stop `up` and these domains become unreachable |
| Occupies 443 | No | Yes, `cdn.sni_port` (in practice it must be 443; hosts entries have no port field) |
| When the IP changes | Manually `cdn --refresh` + `hosts apply` | Cache-only refresh, no hosts rewrite |

SNI mode's boundaries (enforced in code): binds to loopback only, allows only the `cdn.hosts` allowlist (an empty list rejects everything), drops any first packet that is not TLS, caps header reads at 16KB / 10 seconds, and forwards the ClientHello untouched with no man-in-the-middle.

---

## Runtime files (all next to the executable)

| Path | Contents |
|---|---|
| `config.json` | Configuration (upstream credentials in plaintext — do not commit) |
| `runtime/svpn.log` | Run log (in background mode both stdout and stderr go here) |
| `runtime/svpn.pid` | Background process PID, used by `down` / `status` / `doctor` |
| `runtime/effective.json` | Port/mode/node **actually in effect** (the result of `up -b -p` style overrides); read by `status` / `down`, deleted by `down` |
| `runtime/stats.json` | Connection count, bytes up/down, top domains |
| `runtime/probe.json` | Structured `probe` results |
| `runtime/cdn-cache.json` | Edge cache, **byte-for-byte compatible with the Python version**; either side reads the other's |
| `runtime/svpn.pac` | Proxy auto-config generated by `pac` |
| `runtime/hosts.backup` | Original hosts content before the write (contains your whole hosts file) |
| `runtime/wininet-backup.json` | System proxy settings captured before takeover |

Everything except `config.json` lives under `runtime/`, and that directory is in `.gitignore`.

---

## Relationship to the Python version

- **The Python version is the behavioural baseline.** Routing rules, CDN edge selection, probe verdicts and hosts/WinINET handling were rewritten function by function against it.
- **The two can swap `config.json`**: same 12 keys plus the optional `proxy_suffixes` / `direct_suffixes`, same normalisation rules (dirty values fall back, `_` keys dropped, lists may be strings). `runtime/cdn-cache.json` is mutually readable too.
- **The C++ build needs no Python runtime** and reuses nothing from the Python version outside `runtime/`; the Python version does not depend on this exe either.
- Running both: they default to the **same ports** (7899/7900) and the **same managed hosts block**, so only one can run at a time.

---

## Known differences and limitations (the honest list)

1. **The `auto_node` key is read but unused**: parsed and written back, but there is no "scan local ports and persist" logic. Use `node scan` / `node set` explicitly.
2. **Half-open connections wait for the system TCP timeout** (same as the Python version): the relay waits until both directions finish naturally, so one side closing does not cancel the other. There is an internal 1-hour idle cap.
3. **Only two CDN domains get a real throughput measurement**: the bench column only has values for `steamcdn-a.akamaihd.net` and `cdn.akamai.steamstatic.com` (a 1MB Range download); the rest only do a HEAD request, so `-` is by design and does not mean "measured at 0". `--no-bench` skips measurement entirely, leaving handshake latency only.
4. **`cdn.enabled: true` occupies 443** (`cdn.sni_port`). If something like Watt Toolkit already holds it, startup logs a single `WARN SNI forwarder not started (port busy?)` and keeps running — at which point the domains already pointed at 127.0.0.1 in hosts become unreachable. Free 443, or set `cdn.enabled` back to `false` and use "hosts points straight at the best IP".
5. **`probe`'s table has no certificate CN column**: SChannel can already retrieve the peer CN via `SECPKG_ATTR_REMOTE_CERT_CONTEXT` (`TlsProbe::cert_cn`), but the CLI does not print it; the Python version uses `CERT_NONE`, so that field is always empty there.
6. **`probe` occasionally reports one `unreachable · check`**: `store` may resolve to an odd edge; rerun and it recovers. The proxy dialer does failover across candidate IPs.
7. **`node scan` only scans the local machine**: a fixed list of 16 common proxy ports (7890/7891/7897/1080/1081/10808/10809/10810/8889/8080/8118/9910/20171/20172/2080/2081). It reads no environment variables and no system proxy settings.
8. **Only `socks5://` and `http://` upstreams are supported**: `ss://` / `vmess://` / `trojan://` need sing-box, xray or similar to expose a local socks5 endpoint first, then `node set`.
9. **Writing hosts requires administrator rights**; a declined UAC prompt means running an elevated terminal with `--no-elevate`. Also **`hosts` with no subcommand is `apply`** (edits the real hosts file with no second confirmation).
10. **Windows only**: it depends on the WinINET registry, the hosts file and SChannel. macOS / Linux are not supported.

---

## Building

Toolchain, the full command, why `-static` / `-municode` are needed, and a table of common link errors are in **[docs/BUILD.md](docs/BUILD.md)**.

Short version: MinGW-w64 `g++ -std=c++20 -O2 -static -municode` plus `-lws2_32 -lcrypt32 -lsecur32 -lwinhttp -ladvapi32 -luser32 -lshell32 -liphlpapi -lole32 -luuid -ldnsapi -lwininet`, or just run `build.cmd`, which produces both binaries in one pass (`-mconsole` and `-mwindows -DSVPN_GUI_SUBSYSTEM`).

Sources: 14 `.cpp` files plus 1 `.hpp`: `json` / `util` / `rules_data` / `config` / `rules` / `net` / `tls` / `proxy` / `cdn` / `winsys` / `pac` / `sni` / `gui` / `main`. TLS goes through SChannel; the concurrency model is **threads + blocking sockets + `select` for bidirectional relaying**, with no asyncio.

---

## Statements

### Purpose

It does one thing: **local split routing for Steam traffic**. Store, login and downloads go through the upstream node you specify or the best CDN edge; domestic sites connect directly. Three small extras come along: CDN edge selection (writing hosts), endpoint probing (`probe`), and PAC generation.

It is not a VPN, has no TUN, ships no nodes, does not proxy UDP, and performs no TLS interception (see "What it does not do" above).

### Data and privacy

- **It does use the network**, and only for these things: DoH lookups of A records for CDN domains (`doh.pub` / `dns.alidns.com` / `1.12.12.12` / `223.5.5.5`), TLS handshakes and a 1MB throughput test against candidate edge IPs, `probe` connecting to Steam domains, and `node scan` connecting to common local proxy ports.
- **It uploads no data.** No account, no telemetry, no central server; every result stays in the local `runtime/`.
- **It writes local files**: `runtime/svpn.log`, `stats.json`, `probe.json`, `cdn-cache.json`, `svpn.pac`.
- **Two backup files contain system information** and are not shipped with the repository: `runtime/hosts.backup` (the original hosts content, including your entire hosts file) and `runtime/wininet-backup.json` (system proxy settings from before takeover, which may contain a corporate intranet proxy address).
- **Upstream credentials in `config.json` are plaintext** (dictated by SOCKS5/HTTP authentication; the local proxy protocol itself offers no better storage). The file is in `.gitignore` — do not commit it, do not sync it to cloud storage, and read before pasting `node set` output: that line contains the full URL including credentials.

### Security

- **Writing hosts requires one administrator authorisation (UAC).** Only the marked managed block is added or removed (`# >>> steam-vpn managed block (do not edit) >>>` … `# <<< steam-vpn managed block <<<`); bytes outside the block are **preserved verbatim** (including BOM and encoding — the file is handled as bytes and never decoded). It backs up before writing, writes `hosts.tmp` first, then replaces atomically.
- **Changing the system proxy needs no administrator rights** (it writes `HKCU`). It only restores when a backup exists; with no backup it says so explicitly rather than guessing defaults and clearing your settings.
- **It installs no service, adds no autostart entry, injects into no process, loads no driver.** `up` starts an ordinary background process; `down` ends it.
- **It listens on loopback by default.** Setting `listen_host` to `0.0.0.0` turns it into an open proxy — if you really want to share it, configure `auth.username/password` first (SOCKS5 RFC 1929 + HTTP Basic).
- **SNI forwarding is not an open proxy**: loopback only, `cdn.hosts` allowlist only, an empty allowlist rejects everything, and non-TLS first packets are dropped.
- **Only valid hosts entries are accepted**: the IP must be valid and hostnames may not contain spaces, `#` or a forged marker; `cdn-cache.json` is a replaceable file and none of its strings are trusted.
- **Probing does not validate certificates**: to distinguish "handshake reset" from "certificate error", probing skips chain validation. It is diagnostic only and never participates in forwarding.

### Compatibility

- **Windows 10 / 11 x64.** It depends on the WinINET registry, `System32\drivers\etc\hosts` and SChannel; macOS and Linux are not supported.
- **Single statically linked exe, zero DLL dependencies** (no `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll`), and no Python or .NET runtime required.
- **`config.json` and `runtime/cdn-cache.json` are bidirectionally compatible with the Python version**, so the two can be swapped (only one at a time — ports and the managed hosts block are shared).
- **TCP only**; half-open connections wait for the system TCP timeout (see limitation 2).
- Upstreams are limited to `socks5://` and `http://`; `kind: "direct"` means no upstream.

### Disclaimer

The software is provided "as is", without warranty of any kind, express or implied. The author accepts no liability for direct or indirect damages arising from the use of, or inability to use, this tool — including but not limited to sites becoming unreachable because of a mistaken hosts write, a system proxy left unrestored, or download acceleration that does not help.

The tool provides no network egress of its own and performs no circumvention; which upstream you use and what you access are your decisions and your responsibility. When accessing networks across borders, comply with the laws of your jurisdiction.

### License

MIT, see [LICENSE](LICENSE).

---

Version 1.1.1 · Windows x64 · single static exe, no DLL dependencies
