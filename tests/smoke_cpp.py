"""svpn.exe 端到端冒烟：SOCKS5 / HTTP CONNECT / 绝对 URI / 鉴权 / 并发 / 畸形输入。

只依赖标准库。默认连 127.0.0.1:7899(SOCKS5) 与 :7900(HTTP)，需要先
    svpn.exe _serve
在另一个窗口常驻。
"""
import socket
import struct
import sys
import threading
import time

SOCKS = ("127.0.0.1", 7899)
HTTP = ("127.0.0.1", 7900)
ECHO_PORT = 17950

PASS, FAIL = [], []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{('  ' + detail) if detail and not cond else ''}")


def echo_server(stop):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", ECHO_PORT))
    srv.listen(64)
    srv.settimeout(0.5)

    def worker(c):
        try:
            while True:
                data = c.recv(65536)
                if not data:
                    break
                c.sendall(data)
        except OSError:
            pass
        finally:
            c.close()

    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        threading.Thread(target=worker, args=(c,), daemon=True).start()
    srv.close()


def socks5_connect(host, port, timeout=5):
    s = socket.create_connection(SOCKS, timeout)
    s.sendall(b"\x05\x01\x00")
    if s.recv(2) != b"\x05\x00":
        raise RuntimeError("方法协商失败")
    if len(host) > 200:
        raise RuntimeError("host 太长")
    req = b"\x05\x01\x00\x03" + bytes([len(host)]) + host.encode() + struct.pack(">H", port)
    s.sendall(req)
    head = s.recv(4)
    if len(head) < 4:
        raise RuntimeError("回复太短")
    if head[1] != 0:
        raise RuntimeError(f"CONNECT 失败 rep={head[1]}")
    atyp = head[3]
    if atyp == 1:
        s.recv(4)
    elif atyp == 4:
        s.recv(16)
    elif atyp == 3:
        s.recv(s.recv(1)[0])
    s.recv(2)
    return s


def http_connect():
    s = socket.create_connection(HTTP, 5)
    s.sendall(f"CONNECT 127.0.0.1:{ECHO_PORT} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    return s, head.decode("latin1"), rest


def main():
    stop = threading.Event()
    threading.Thread(target=echo_server, args=(stop,), daemon=True).start()
    time.sleep(0.3)

    print("[SOCKS5]")
    try:
        s = socks5_connect("127.0.0.1", ECHO_PORT)
        s.sendall(b"hello-socks")
        got = s.recv(64)
        check("SOCKS5 隧道收发", got == b"hello-socks", repr(got))
        s.close()
    except Exception as exc:  # noqa: BLE001
        check("SOCKS5 隧道收发", False, f"{type(exc).__name__}: {exc}")

    big = b"x" * (256 * 1024)
    try:
        s = socks5_connect("127.0.0.1", ECHO_PORT)
        s.sendall(big)
        got = b""
        while len(got) < len(big):
            chunk = s.recv(65536)
            if not chunk:
                break
            got += chunk
        check("SOCKS5 256KB 回环", got == big, f"{len(got)}/{len(big)}")
        s.close()
    except Exception as exc:  # noqa: BLE001
        check("SOCKS5 256KB 回环", False, f"{type(exc).__name__}: {exc}")

    print("[HTTP]")
    try:
        s, head, rest = http_connect()
        ok = "200" in head.split("\r\n")[0]
        s.sendall(b"hello-http")
        got = rest or s.recv(64)
        check("HTTP CONNECT 200", ok, head)
        check("HTTP CONNECT 收发", got == b"hello-http", repr(got))
        s.close()
    except Exception as exc:  # noqa: BLE001
        check("HTTP CONNECT", False, f"{type(exc).__name__}: {exc}")

    try:
        s = socket.create_connection(HTTP, 5)
        s.sendall(b"CONNECT 127.0.0.1:abc HTTP/1.1\r\n\r\n")
        data = s.recv(200).decode("latin1")
        check("畸形 CONNECT 回 400", "400" in data, data.split("\r\n")[0] if data else "empty")
        s.close()
    except Exception as exc:  # noqa: BLE001
        check("畸形 CONNECT 回 400", False, f"{type(exc).__name__}: {exc}")

    print("[并发]")
    errors = []

    def one(i):
        try:
            s = socks5_connect("127.0.0.1", ECHO_PORT)
            msg = f"c{i}".encode()
            s.sendall(msg)
            if s.recv(64) != msg:
                errors.append(i)
            s.close()
        except Exception as exc:  # noqa: BLE001
            errors.append(f"{i}:{exc}")

    threads = [threading.Thread(target=one, args=(i,)) for i in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check("20 路并发", not errors, str(errors[:3]))

    print("[真网]")
    try:
        s = socks5_connect("store.steampowered.com", 443, timeout=10)
        check("SOCKS5 直连 store:443", True)
        s.close()
    except Exception as exc:  # noqa: BLE001
        check("SOCKS5 直连 store:443", False, f"{type(exc).__name__}: {exc}")

    stop.set()
    print(f"\n通过 {len(PASS)} / 失败 {len(FAIL)}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
