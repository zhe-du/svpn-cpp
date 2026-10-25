"""svpn.exe CLI 回归测试（把 V1 的审计结论固化成可执行断言）。

只依赖标准库（PAC 那一组额外用 node，缺 node 就 SKIP）。

设计约束（踩过的坑，别改回去）：
1. **必须在一条命令里跑完 up→up→status→down**：本环境每条 shell 命令结束会回收进程树，
   跨命令的 up 会「人间蒸发」，看起来像「重复 up 换了 pid」。
2. **绝不在工程根目录直接跑 exe**：lead 在反复重编译，会 Permission denied 或拿到半个二进制。
   一律复制到 temp 目录再跑；`Paths::resolve()` 跟着 exe 目录走，config/runtime 都落在 temp 里。
3. **绝不碰真实 hosts**：`hosts clear` 用 `SVPN_HOSTS_FILE` 环境变量指到临时文件
   （winsys.cpp 里专门留的测试缝）。不用 `--apply`、不用 `system on/off`，只跑只读的 `system show`。
4. 二进制会变：开跑先打 md5 + mtime，结论要带版本。

用法：
    python tests/test_cli.py            # 跑全部
    python tests/test_cli.py --no-net   # 跳过 probe（会连真实端点）
"""

import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXE = os.path.join(ROOT, "svpn.exe")

# 用独立端口，避免和 lead 正在跑的 7899/7900 撞车；实际值在 main() 里动态挑空闲端口，
# 免得上一轮残留的实例/端口占用把 doctor 与 up 的结论污染成假通过或假失败。
SOCKS_PORT = 17899
HTTP_PORT = 17900


def pick_free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port

PASS, FAIL, HARD, SKIP = [], [], [], []
_tmpdirs = []


def check(name, cond, detail="", contract=False):
    """contract=True 表示「契约要求、但当前实现可能还没做到」的用例。"""
    if cond:
        PASS.append(name)
        print(f"  PASS  {name}")
    else:
        FAIL.append(name)
        if contract:
            print(f"  FAIL  {name}   [契约未满足]  {detail}".rstrip())
        else:
            HARD.append(name)
            print(f"  FAIL  {name}   {detail}".rstrip())


def skip(name, why):
    SKIP.append(name)
    print(f"  SKIP  {name}   ({why})")


# ---------------------------------------------------------------- 进程/文件工具
def run_full(cwd, args, timeout=60, env_extra=None):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    exe = os.path.join(cwd, "svpn_t1.exe")
    if not os.path.exists(exe):
        exe = os.path.join(cwd, "svpn_t1b.exe")
    try:
        p = subprocess.run([exe] + args, cwd=cwd, capture_output=True, timeout=timeout, env=env)
        return (p.returncode,
                p.stdout.decode("utf-8", "replace"),
                p.stderr.decode("utf-8", "replace"))
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode("utf-8", "replace")
        return -9, out + "\n<TIMEOUT>", ""


def run(cwd, args, timeout=60, env_extra=None):
    rc, out, err = run_full(cwd, args, timeout, env_extra)
    return rc, out + err


def make_workspace(name, exe_name, config=None):
    d = tempfile.mkdtemp(prefix=f"svpn_t1_{name}_")
    _tmpdirs.append(d)
    shutil.copy2(SRC_EXE, os.path.join(d, exe_name))
    if config is not None:
        with open(os.path.join(d, "config.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(config, f, ensure_ascii=False, indent=2)
    return d


def base_config(**over):
    cfg = {
        "listen_host": "127.0.0.1",
        "listen_port": SOCKS_PORT,
        "http_port": HTTP_PORT,
        "mode": "whitelist",
        "connect_timeout": 5,
        "auto_node": True,
        "upstream": {"kind": "socks5", "host": "NODE_HOST", "port": 1080,
                     "username": "", "password": ""},
        "auth": {"username": "", "password": ""},
        "cdn": {"enabled": False, "refresh_hours": 6, "sni_port": 443, "hosts": []},
        "proxy_hosts": [],
        "direct_hosts": [],
    }
    cfg.update(over)
    return cfg


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def connect_ex_code(port, host="127.0.0.1"):
    """阻塞 socket 上做 connect_ex。

    注意：**不能** settimeout 之后再 connect_ex —— settimeout 会把 socket 切成非阻塞模式，
    connect_ex 立刻返回 10035(WSAEWOULDBLOCK)，看起来像「端口还开着」。loopback 上
    拒绝连接是瞬时的，不需要超时。
    """
    s = socket.socket()
    s.settimeout(None)
    try:
        return s.connect_ex((host, port))
    finally:
        s.close()


def port_open(port, host="127.0.0.1"):
    return connect_ex_code(port, host) == 0


def port_closed(port, host="127.0.0.1"):
    return connect_ex_code(port, host) in (10061, 10054)


def wait_closed(port, deadline=8.0):
    end = time.time() + deadline
    while time.time() < end:
        if port_closed(port):
            return True
        time.sleep(0.1)
    code = connect_ex_code(port)
    print(f"    (端口 {port} 最终 connect_ex={code})")
    return False


def wait_pidfile(path, deadline=5.0):
    """等 pidfile 出现并写出数字。

    `up -b` 返回「后台已启动」的时机与写 pidfile 之间有一个很小的窗口，
    直接读文件偶尔会读到空/不存在（V1 踩过）。轮询到数字为止。
    """
    end = time.time() + deadline
    while time.time() < end:
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    txt = f.read().strip()
            except OSError:
                txt = ""
            if txt.isdigit():
                return txt
        time.sleep(0.05)
    return None


def task_count(image):
    """按映像名数进程：只有我们的 svpn_t1.exe 会被数到，不会误伤 lead 的 svpn.exe。"""
    try:
        p = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"],
                           capture_output=True, timeout=20)
        text = p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
        return len(re.findall(rf"\b{re.escape(image)}\b", text)), text
    except (OSError, subprocess.TimeoutExpired) as exc:
        return -1, str(exc)


# ---------------------------------------------------------------- A. 用法/退出码
def test_usage(ws):
    print("\n[A] 用法与退出码")
    rc, out = run(ws, [])
    check("无参数 → rc=2", rc == 2, f"rc={rc}")
    check("无参数 → 打印用法", "用法" in out and "命令" in out, out.strip()[:80])

    rc, out = run(ws, ["nosuchcmd"])
    check("未知命令 → rc=2", rc == 2, f"rc={rc}")
    check("未知命令 → 报未知命令", "未知命令" in out, out.strip()[:80])

    # V1 审计：--help/-h/help 当前走「未知命令」rc=2，契约要求 rc=0 且打印用法
    for flag in ("--help", "-h", "help"):
        rc, out = run(ws, [flag])
        check(f"{flag} → rc=0 且打印用法（契约）",
              rc == 0 and "用法" in out, f"rc={rc} out={out.strip()[:60]!r}", contract=True)

    # probe 自己的 -h：旧版会忽略它、真的跑一遍 probe（联网 + 十几秒）
    rc, out = run(ws, ["probe", "-h"], timeout=120)
    check("probe -h → rc=0 且打印用法（不该真跑 probe）",
          rc == 0 and "用法" in out, f"rc={rc} out={out.strip()[:80]!r}")

    # ⑨ 文档契约：help 里不能再出现 `pac --write`（--write 是静默忽略的空开关）
    rc, out = run(ws, ["--help"])
    check("help 文案不出现 pac --write（防文档把空开关写回去）",
          "--write" not in out, [ln for ln in out.splitlines() if "--write" in ln][:2])
    check("help 的 pac 行提到 --apply",
          any("pac" in ln and "--apply" in ln for ln in out.splitlines()), out.strip()[:120])


# ---------------------------------------------------------------- B. system（只读）
def test_system(ws):
    print("\n[B] system（只读，不 on/off）")
    rc, out = run(ws, ["system", "show"])
    check("system show → rc=0", rc == 0, f"rc={rc}")
    check("system show → 含 ProxyEnable", "ProxyEnable" in out, out.strip()[:80])
    check("system show → 含 ProxyServer/ProxyOverride",
          "ProxyServer" in out and "ProxyOverride" in out, out.strip()[:120])

    rc, out = run(ws, ["system"])
    check("system（无子命令）→ rc=0", rc == 0, f"rc={rc}")
    check("system（无子命令）→ 等同 show", "ProxyEnable" in out, out.strip()[:80])


# ---------------------------------------------------------------- C. node
def test_node(ws):
    print("\n[C] node set / node clear")
    cfg_path = os.path.join(ws, "config.json")

    rc, out = run(ws, ["node", "clear"])
    check("node clear → rc=0", rc == 0, f"rc={rc}")
    check("node clear → 输出「已清空节点」", "已清空节点" in out, out.strip()[:100])
    cfg = read_json(cfg_path) or {}
    up = cfg.get("upstream") or {}
    check("node clear → config upstream.host == NODE_HOST",
          up.get("host") == "NODE_HOST", f"host={up.get('host')!r}")
    check("node clear → config upstream.port 回到 1080", up.get("port") == 1080,
          f"port={up.get('port')!r}")

    rc, out = run(ws, ["node", "set"])
    check("node set（无 URL）→ rc=2", rc == 2, f"rc={rc}")
    check("node set（无 URL）→ 打印用法", "用法" in out, out.strip()[:100])

    rc, out = run(ws, ["node", "set", "ftp://x:1"])
    check("node set 非法 scheme → rc=2", rc == 2, f"rc={rc}")


# ---------------------------------------------------------------- D. hosts clear
HOSTS_BEGIN = "# >>> steam-vpn managed block (do not edit) >>>"
HOSTS_END = "# <<< steam-vpn managed block <<<"


def real_hosts_digest():
    """真实 hosts 的指纹（读不到返回 None，不报错）。"""
    path = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                        "System32", "drivers", "etc", "hosts")
    try:
        with open(path, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()
    except OSError:
        return None


def test_hosts_clear(ws):
    print("\n[D] hosts clear --no-elevate（SVPN_HOSTS_FILE 测缝，不碰真实 hosts）")
    fake = os.path.join(ws, "fake_hosts")
    bom = b"\xef\xbb\xbf"
    user_lines = [b"# user comment\r\n", b"127.0.0.1 localhost\r\n", b"10.0.0.1 nas.lan\r\n"]
    block = (HOSTS_BEGIN + "\r\n"
             + "23.45.64.1      cdn.akamai.steamstatic.com\r\n"
             + "23.45.64.2      steamcdn-a.akamaihd.net\r\n"
             + HOSTS_END + "\r\n")
    with open(fake, "wb") as f:
        f.write(bom + b"".join(user_lines) + b"\r\n" + block.encode())

    before_real = real_hosts_digest()

    rc, out = run(ws, ["hosts", "clear", "--no-elevate"], env_extra={"SVPN_HOSTS_FILE": fake})
    check("hosts clear --no-elevate → rc=0", rc == 0, f"rc={rc} out={out.strip()[:100]}")
    check("hosts clear → 输出「hosts 已清理」", "hosts 已清理" in out, out.strip()[:100])

    with open(fake, "rb") as f:
        after = f.read()
    text = after.decode("utf-8", "replace")
    check("托管块 begin 标记被移除", HOSTS_BEGIN not in text)
    check("托管块 end 标记被移除", HOSTS_END not in text)
    check("托管块里的 2 条记录被移除", "23.45.64.1" not in text and "23.45.64.2" not in text)
    check("块外用户行逐行保留", all(line in after for line in user_lines), repr(after[:80]))
    check("块外字节未变（去掉托管块与尾部分隔空行后逐字节相等）",
          after == bom + b"".join(user_lines), repr(after))
    check("BOM 被保留", after.startswith(bom))

    if before_real is None:
        skip("真实 hosts 未被改动", "无权限读取真实 hosts")
    else:
        check("真实 hosts 未被改动", real_hosts_digest() == before_real)


# ---------------------------------------------------------------- E. doctor
def test_doctor():
    print("\n[E] doctor")
    d = make_workspace("doctor", "svpn_t1b.exe")   # 故意不写 config.json
    rc, out = run(d, ["doctor"])
    check("doctor 缺 config.json → rc=1", rc == 1, f"rc={rc} out={out.strip()[:120]}")
    check("doctor 缺 config.json → 提示跑 svpn init", "config.json" in out, out.strip()[:120])

    # 产品口径（V1 已与实现确认，不用裁决）：
    #   未知键 → 报问题 rc≠0 且点名；缺键 → 容忍 rc=0。
    # 断言按「行」匹配而不是整串子串，且不依赖措辞（702be7f8 是「未知键」，
    # 别的构建写成「无法识别的键」也不影响）——只看有没有一行 [!] 同时点名 config.json 与键名。
    d2 = make_workspace("doctor_broken", "svpn_t1b.exe")
    # 带上独立端口，避免被 lead 正在跑的 7899/7900 污染成「端口冲突」而假通过
    with open(os.path.join(d2, "config.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write('{"broken": true, "listen_port": %d, "http_port": %d}' % (SOCKS_PORT, HTTP_PORT))
    rc, out = run(d2, ["doctor"], timeout=90)
    check("未知键 config → doctor rc≠0", rc != 0, f"rc={rc} out={out.strip()[:160]}")
    named = [ln for ln in out.splitlines()
             if ln.lstrip().startswith("[!]") and "config.json" in ln and "broken" in ln]
    check("未知键 config → 有一行 [!] 同时点名 config.json 与 broken",
          bool(named), out.strip()[:200])

    # 另一半契约：缺键必须容忍（防止有人把「未知键报错」收紧成「缺键也报错」）
    d4 = make_workspace("doctor_partial", "svpn_t1b.exe")
    with open(os.path.join(d4, "config.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write('{"listen_port": %d, "http_port": %d}' % (SOCKS_PORT, HTTP_PORT))
    rc, out = run(d4, ["doctor"], timeout=90)
    check("只写部分已知键（缺键）→ doctor rc=0", rc == 0, f"rc={rc} out={out.strip()[:200]}")
    check("只写部分已知键 → 没发现问题", "没发现问题" in out, out.strip()[:200])

    # 例外：`_` 前缀的键（如 _note）本来就是说明文字，必须容忍、不算未知键
    d5 = make_workspace("doctor_note", "svpn_t1b.exe")
    with open(os.path.join(d5, "config.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write('{"_note": "说明文字", "listen_port": %d, "http_port": %d}'
                % (SOCKS_PORT, HTTP_PORT))
    rc, out = run(d5, ["doctor"], timeout=90)
    check("_note 等下划线键被容忍 → doctor rc=0", rc == 0, f"rc={rc} out={out.strip()[:200]}")
    check("下划线键不被当成未知键点名", "_note" not in out, out.strip()[:200])

    # 真语法错误（JSON 都解析不了）：doctor 必须 rc=1
    d3 = make_workspace("doctor_syntax", "svpn_t1b.exe")
    with open(os.path.join(d3, "config.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write('{"listen_port": 1234,,}')
    if port_open(7899) or port_open(7900):
        skip("真语法错误 config → doctor rc=1", "7899/7900 被占用，rc 结论会被端口检查污染")
    else:
        rc, out = run(d3, ["doctor"], timeout=90)
        check("真语法错误 config → doctor rc=1", rc == 1, f"rc={rc} out={out.strip()[:200]}")
        bad = [ln for ln in out.splitlines()
               if ln.lstrip().startswith("[!]") and "config.json" in ln and "解析失败" in ln]
        check("真语法错误 config → 有一行 [!] 说明 config.json 解析失败",
              bool(bad), out.strip()[:200])


# ---------------------------------------------------------------- F. probe --json
def test_probe(ws):
    print("\n[F] probe --json")
    rc, stdout, stderr = run_full(
        ws, ["probe", "--json", "--lenient", "--node", "none", "--timeout", "1"], timeout=90)
    check("probe --lenient → rc ∈ {0,1}", rc in (0, 1), f"rc={rc}")

    # 契约：--json 时 **stdout 整体**必须是 JSON（人要看的表格/提示请走 stderr）
    table_in_stdout = "域名" in stdout and "耗时" in stdout
    check("probe --json → 表格不混进 stdout", not table_in_stdout,
          stdout.strip().splitlines()[:2] if stdout.strip() else "")
    try:
        doc = json.loads(stdout)
        ok_parse = True
        detail = ""
    except ValueError as exc:
        ok_parse = False
        doc = None
        tail = stdout.strip().splitlines()[-3:]
        detail = f"{exc}; stdout 末尾 3 行: {tail}"
    check("probe --json → stdout 整体可 json.loads（契约）", ok_parse, detail, contract=True)

    if ok_parse:
        check("probe --json → 含 rows 数组", isinstance(doc.get("rows"), list), str(list(doc)[:5]))
        check("probe --json → rows 有 9 个端点", len(doc.get("rows", [])) == 9,
              str(len(doc.get("rows", []))))
        check("probe --json → 含 ok/unusable 字段",
              "ok" in doc and "unusable" in doc, str(list(doc)[:6]))
    else:
        # 退一步：从 stdout 里截出那段 JSON，确认序列化本身没问题
        m = re.search(r'"rows"\s*:', stdout)
        if m:
            start = stdout.rfind("{", 0, m.start())
            try:
                part = json.loads(stdout[start:stdout.rfind("}") + 1])
                check("probe --json → 混在 stdout 里的 JSON 片段本身合法",
                      isinstance(part.get("rows"), list) and len(part["rows"]) == 9, "")
            except ValueError as exc:
                check("probe --json → 混在 stdout 里的 JSON 片段本身合法", False, str(exc))
        else:
            check("probe --json → 输出里能找到 rows 字段", False, stdout[:120])


# ---------------------------------------------------------------- G. PAC
PAC_JS = r"""
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
function isPlainHostName(h) { return h.indexOf('.') === -1; }
function shExpMatch(s, p) {
  const re = new RegExp('^' + p.replace(/[.+^${}()|[\]\\]/g, '\\$&')
    .replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
  return re.test(s);
}
let fn;
try {
  fn = new Function('isPlainHostName', 'shExpMatch', src + '\nreturn FindProxyForURL;')(
    isPlainHostName, shExpMatch);
} catch (e) {
  console.log('SYNTAX_ERROR ' + e.message);
  process.exit(2);
}
const hosts = JSON.parse(process.argv[3]);
const out = {};
for (const h of hosts) out[h] = fn('https://' + h + '/', h);
console.log(JSON.stringify(out));
"""


def test_pac(ws):
    print("\n[G] pac（语法 + 分流判定）")
    rc, out = run(ws, ["pac"])
    check("pac → rc=0", rc == 0, f"rc={rc} out={out.strip()[:120]}")
    pac_path = os.path.join(ws, "runtime", "svpn.pac")
    check("pac 文件已生成 runtime/svpn.pac", os.path.exists(pac_path), pac_path)
    if not os.path.exists(pac_path):
        return

    with open(pac_path, "r", encoding="utf-8") as f:
        src = f.read()
    check("PAC 里代理串是 SOCKS5 127.0.0.1:<端口>",
          f'"SOCKS5 127.0.0.1:{SOCKS_PORT}"' in src.replace("'", '"'),
          src.splitlines()[1] if len(src.splitlines()) > 1 else "")

    node = shutil.which("node")
    if not node:
        skip("PAC 在 node 里执行", "没有 node")
        return
    hosts = ["store.steampowered.com", "www.baidu.com", "example.org",
             "notsteam.example.com", "steampowered.com.evil.cn"]
    js = os.path.join(ws, "pac_check.js")
    with open(js, "w", encoding="utf-8") as f:
        f.write(PAC_JS)
    try:
        p = subprocess.run([node, js, pac_path, json.dumps(hosts)],
                           capture_output=True, timeout=60)
        text = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
    except (OSError, subprocess.TimeoutExpired) as exc:
        check("PAC 在 node 里执行", False, str(exc))
        return

    if "SYNTAX_ERROR" in text:
        check("PAC 是合法 JS（new Function 不抛）", False, text.strip())
        return
    check("PAC 是合法 JS（new Function 不抛）", p.returncode == 0, err.strip()[:120])
    try:
        got = json.loads(text)
    except ValueError:
        check("PAC 判定结果可解析", False, text[:120])
        return

    print("    判定: " + json.dumps(got, ensure_ascii=False))
    st = got.get("store.steampowered.com", "")
    check("store.steampowered.com → SOCKS5…; DIRECT",
          "SOCKS5" in st and "DIRECT" in st, st)
    for h in ("www.baidu.com", "example.org", "notsteam.example.com",
              "steampowered.com.evil.cn"):
        check(f"{h} → DIRECT", got.get(h) == "DIRECT", got.get(h, ""))


# ---------------------------------------------------------------- H. up 覆盖 + ⑫ pid
def test_up_override(ws):
    print("\n[H] up -p/-m 覆盖 + ⑫ 失败的 up 不得抢删在跑实例的 pid")
    alt = pick_free_port()
    while alt in (SOCKS_PORT, HTTP_PORT):
        alt = pick_free_port()
    pid_path = os.path.join(ws, "runtime", "svpn.pid")
    eff_path = os.path.join(ws, "runtime", "effective.json")

    rc, out = run(ws, ["up", "-b", "-p", str(alt), "-m", "blacklist"], timeout=90)
    check(f"up -b -p {alt} -m blacklist → rc=0", rc == 0, f"rc={rc} out={out.strip()[:160]}")
    check("up -b -p → 输出「后台已启动」", "后台已启动" in out, out.strip()[:160])
    pid_a = wait_pidfile(pid_path)
    if pid_a is None:
        check("pidfile 已生成且内容为数字", False, pid_path)
        return
    check("pidfile 已生成且内容为数字", True, pid_a)
    check(f"-p 覆盖的端口 {alt} 已监听", port_open(alt), f"connect_ex={connect_ex_code(alt)}")

    eff = read_json(eff_path) or {}
    check("effective.json 记下覆盖端口", eff.get("listen_port") == alt, str(eff)[:120])
    check("effective.json 记下覆盖模式", eff.get("mode") == "blacklist", str(eff)[:120])
    with open(eff_path, "rb") as f:
        eff_before = f.read()

    rc, out = run(ws, ["status"])
    check("status 用 effective.json 而非 config.json 报端口",
          f":{alt}" in out and "监听中" in out, out.strip()[:200])
    check("status 报 模式 blacklist（来自 -m 覆盖）", "blacklist" in out, out.strip()[:200])

    # ---- ⑫ P1：第二个 up 起不来，但绝不能动在跑实例的 pid 与状态 ----
    # 这里刻意让 config 的 listen_port 是空闲的、只有 http_port 被占，重放旧版那个 bug：
    # 只查 SOCKS5 端口 → 先「成功」→ 子进程 bind HTTP 端口失败 → 把 pid 记录带走。
    rc2, out2 = run(ws, ["up", "-b"], timeout=90)
    check("⑫ 二次 up -b（端口冲突）→ rc=0", rc2 == 0, f"rc={rc2} out={out2.strip()[:160]}")
    check("⑫ 二次 up -b → 输出「已在运行」", "已在运行" in out2, out2.strip()[:160])
    check("⑫ pidfile 没被删掉", os.path.exists(pid_path), pid_path)
    pid_b = "<被删了>"
    if os.path.exists(pid_path):
        with open(pid_path, "r", encoding="utf-8") as f:
            pid_b = f.read().strip()
    check("⑫ pidfile 内容仍是 A", pid_b == pid_a, f"{pid_a} → {pid_b}")
    eff2 = read_json(eff_path) or {}
    check("⑫ effective.json 没被失败实例的参数覆盖",
          eff2.get("listen_port") == alt and eff2.get("mode") == "blacklist", str(eff2)[:120])
    with open(eff_path, "rb") as f:
        eff_after = f.read()
    check("⑫ effective.json 逐字节未变", eff_after == eff_before,
          f"{len(eff_before)}B → {len(eff_after)}B")
    n, _raw = task_count("svpn_t1.exe")
    check("⑫ 仍然只有 1 个实例", n == 1, f"count={n}")

    rc3, out3 = run(ws, ["down"], timeout=60)
    check("down 能按 pid 正常停掉 A", rc3 == 0 and "已停止" in out3 and pid_a in out3,
          out3.strip()[:160])
    for port, label in ((alt, "-p 覆盖端口"), (SOCKS_PORT, "config 端口"), (HTTP_PORT, "http 端口")):
        check(f"down 后{label} {port} 连接被拒（10061）", wait_closed(port),
              f"connect_ex={connect_ex_code(port)}")
    check("down 后 pidfile 被清理", not os.path.exists(pid_path))
    check("down 后 effective.json 被清理", not os.path.exists(eff_path))


# ---------------------------------------------------------------- I. up/down 生命周期
def test_lifecycle(ws):
    print("\n[I] up -b → up -b → status → down（必须同一条命令内跑完）")
    pid_path = os.path.join(ws, "runtime", "svpn.pid")

    rc, out = run(ws, ["up", "-b"], timeout=90)
    check("首次 up -b → rc=0", rc == 0, f"rc={rc} out={out.strip()[:160]}")
    check("首次 up -b → 输出「后台已启动」", "后台已启动" in out, out.strip()[:160])
    pid1 = wait_pidfile(pid_path)
    if pid1 is None:
        check("pidfile 已生成", False, pid_path)
        return
    check("pidfile 内容为数字", pid1.isdigit(), repr(pid1))
    check(f"端口 {SOCKS_PORT} 已监听", port_open(SOCKS_PORT), f"connect_ex={connect_ex_code(SOCKS_PORT)}")
    check(f"端口 {HTTP_PORT} 已监听", port_open(HTTP_PORT), f"connect_ex={connect_ex_code(HTTP_PORT)}")

    rc2, out2 = run(ws, ["up", "-b"], timeout=90)
    check("二次 up -b → rc=0", rc2 == 0, f"rc={rc2}")
    check("二次 up -b → 输出「已在运行」", "已在运行" in out2, out2.strip()[:160])
    with open(pid_path, "r", encoding="utf-8") as f:
        pid2 = f.read().strip()
    check("二次 up -b 不换 pid", pid2 == pid1, f"{pid1} → {pid2}")

    rc3, out3 = run(ws, ["status"])
    check("status → rc=0", rc3 == 0, f"rc={rc3}")
    check("status → 显示运行中且 pid 一致", f"pid={pid1}" in out3, out3.strip()[:200])

    n, raw = task_count("svpn_t1.exe")
    check("只有 1 个 svpn 进程在跑", n == 1, f"count={n} raw={raw.strip()[:200]}")

    rc4, out4 = run(ws, ["down"], timeout=60)
    check("down → rc=0", rc4 == 0, f"rc={rc4} out={out4.strip()[:160]}")
    check("down → 输出「已停止」", "已停止" in out4, out4.strip()[:160])
    check(f"down 后 {SOCKS_PORT} 连接被拒（10061）", wait_closed(SOCKS_PORT), "端口仍开着")
    check(f"down 后 {HTTP_PORT} 连接被拒（10061）", wait_closed(HTTP_PORT), "端口仍开着")

    rc5, out5 = run(ws, ["down"], timeout=60)
    check("二次 down → rc=0", rc5 == 0, f"rc={rc5}")
    check("二次 down → 输出「没有在运行的实例」", "没有在运行的实例" in out5, out5.strip()[:160])


# ---------------------------------------------------------------- J. probe --lenient
PROBE_SUM = re.compile(r"直连可用 (\d+) / 需节点 (\d+) / 死端点 (\d+) / 当前不可用 (\d+)")


def test_probe_lenient(ws):
    """④ 不写死 rc，只验不变量：D == B + C、rc 与桶一致、lenient 不比 strict 更严。

    用 `--node none` + `--timeout 1`：所有直连都会失败，按 main.cpp 的分桶，
    直连失败且没配节点会记进「需节点」而不是「死端点」，所以 C 恒为 0 ——
    这正是 `--lenient` 的意义所在（strict 会因为 D>0 返回 1，lenient 必须返回 0）。
    """
    print("\n[J] probe --lenient 的语义不变量（④）")
    rc_s, out_s = run(ws, ["probe", "--node", "none", "--timeout", "1"], timeout=120)
    rc_l, out_l = run(ws, ["probe", "--lenient", "--node", "none", "--timeout", "1"], timeout=120)
    ms = PROBE_SUM.search(out_s)
    ml = PROBE_SUM.search(out_l)
    if not ms or not ml:
        check("probe 能解析出四个桶的计数", False, (out_s[-200:] + " ||| " + out_l[-200:]))
        return
    a_s, b_s, c_s, d_s = (int(x) for x in ms.groups())
    a_l, b_l, c_l, d_l = (int(x) for x in ml.groups())
    print(f"    strict : 直连 {a_s} / 需节点 {b_s} / 死 {c_s} / 不可用 {d_s}  rc={rc_s}")
    print(f"    lenient: 直连 {a_l} / 需节点 {b_l} / 死 {c_l} / 不可用 {d_l}  rc={rc_l}")

    check("每轮都有 9 个端点计入桶",
          a_s + b_s + c_s == 9 and a_l + b_l + c_l == 9,
          f"strict={a_s + b_s + c_s} lenient={a_l + b_l + c_l}")
    check("D（当前不可用）== B（需节点）+ C（死端点）",
          d_s == b_s + c_s and d_l == b_l + c_l,
          f"strict {d_s} vs {b_s}+{c_s}; lenient {d_l} vs {b_l}+{c_l}")
    check("strict rc == (D>0)", rc_s == (1 if d_s else 0), f"rc={rc_s} D={d_s}")
    check("lenient rc == (C>0)", rc_l == (1 if c_l else 0), f"rc={rc_l} C={c_l}")
    check("lenient 不比 strict 更严（rc_l <= rc_s）", rc_l <= rc_s, f"{rc_l} vs {rc_s}")
    check("--node none 下死端点恒为 0（直连失败记「需节点」）",
          c_s == 0 and c_l == 0, f"strict C={c_s} lenient C={c_l}")


# ---------------------------------------------------------------- K. cdn 实测列
def test_cdn_tput(ws):
    """⑧ 只验「实测」列的格式，不追速度：--no-bench 必须显示 '-'。"""
    print("\n[K] cdn 实测列格式（⑧）")
    domain = "cdn.akamai.steamstatic.com"
    rc, out = run(ws, ["cdn", "--refresh", "--no-bench", "--domain", domain, "--timeout", "4"],
                  timeout=240)
    row = next((ln for ln in out.splitlines() if domain in ln and "候选" not in ln), None)
    if row is None or rc != 0:
        skip("cdn --no-bench 的实测列是 '-'",
             f"rc={rc}，没拿到 {domain} 的行（DoH/网络不通时不判失败）")
        return
    toks = row.split()
    check("cdn 行至少 5 列（域名/最优IP/握手/实测/候选）", len(toks) >= 5, row)
    if len(toks) < 5:
        return
    tput = toks[-2]
    check("--no-bench 时实测列是 '-'（不是 0.00MB/s）", tput == "-", f"实测={tput!r} 整行={row}")
    check("实测列格式合法（'-' 或 N.NNMB/s）",
          tput == "-" or re.fullmatch(r"\d+\.\d{2}MB/s", tput), tput)


# ---------------------------------------------------------------- L. down 兜底提示
def test_down_port_hint(ws):
    """⑩ 不造「pid 属于别人」的场景（有误杀风险），改测用户可见的兜底提示。"""
    print("\n[L] down 兜底提示：没 pidfile 但端口被占（⑩）")
    pid_path = os.path.join(ws, "runtime", "svpn.pid")
    try:
        os.remove(pid_path)
    except OSError:
        pass

    hold = socket.socket()
    hold.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        hold.bind(("127.0.0.1", SOCKS_PORT))
        # backlog 必须给足：前置探针自己会占掉一个 SYN 名额，listen(1) 时
        # 后续 C++ 侧 300ms 的探测会连不进来，down 就会误判成「端口是空的」。
        hold.listen(64)
        check("前置条件：测试进程已占住端口", port_open(SOCKS_PORT),
              f"connect_ex={connect_ex_code(SOCKS_PORT)}")
        rc, out = run(ws, ["down"], timeout=60)
        check("down 无 pidfile → 报「没有在运行的实例」", "没有在运行的实例" in out,
              out.strip()[:200])
        check(f"down 无 pidfile 但 {SOCKS_PORT} 被占 → 提示仍被占用",
              "仍被占用" in out and str(SOCKS_PORT) in out, out.strip()[:240])
        check("down 此时仍返回 rc=0", rc == 0, f"rc={rc}")
    finally:
        hold.close()


# ---------------------------------------------------------------- main
def main():
    global SOCKS_PORT, HTTP_PORT
    no_net = "--no-net" in sys.argv
    if not os.path.exists(SRC_EXE):
        print(f"FATAL 找不到 {SRC_EXE}")
        return 2

    SOCKS_PORT = pick_free_port()
    while True:
        HTTP_PORT = pick_free_port()
        if HTTP_PORT != SOCKS_PORT:
            break

    st = os.stat(SRC_EXE)
    with open(SRC_EXE, "rb") as f:
        digest = hashlib.md5(f.read()).hexdigest()
    print("svpn CLI 回归（T1）")
    print(f"  被测二进制: {SRC_EXE}")
    print(f"  md5={digest}  size={st.st_size}  mtime={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}")
    print(f"  端口: SOCKS {SOCKS_PORT} / HTTP {HTTP_PORT}（避开 7899/7900）")

    ws = make_workspace("main", "svpn_t1.exe", base_config())
    print(f"  工作目录: {ws}")

    try:
        test_usage(ws)
        test_system(ws)
        test_node(ws)
        test_hosts_clear(ws)
        test_doctor()
        if no_net:
            skip("probe --json（整组）", "--no-net")
            skip("probe --lenient 不变量（整组）", "--no-net")
        else:
            test_probe(ws)
            test_probe_lenient(ws)
        test_pac(ws)
        if no_net:
            skip("cdn 实测列格式（整组）", "--no-net")
        else:
            test_cdn_tput(ws)
        test_up_override(ws)
        test_lifecycle(ws)
        test_down_port_hint(ws)
    finally:
        # 兜底：中途炸了也别留下常驻进程
        try:
            run(ws, ["down"], timeout=30)
        except Exception:  # noqa: BLE001
            pass
        for d in _tmpdirs:
            shutil.rmtree(d, ignore_errors=True)

    print(f"\nPASS {len(PASS)} / FAIL {len(FAIL)} / SKIP {len(SKIP)}"
          f"（其中硬失败 {len(HARD)}、契约待修 {len(FAIL) - len(HARD)}）")
    if HARD:
        print("硬失败:")
        for n in HARD:
            print(f"  - {n}")
    if len(FAIL) - len(HARD):
        print("契约待修（src 修好后会自动转绿）:")
        for n in FAIL:
            if n not in HARD:
                print(f"  - {n}")
    if SKIP:
        print("跳过:")
        for n in SKIP:
            print(f"  - {n}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
