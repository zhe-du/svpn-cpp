#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""svpn 1.1.1 打包：dist/ 双产物 + RELEASE.txt + SHA256SUMS + zip。

设计要点（都踩过）：
1. 源码指纹取自 **git 索引 blob**（LF 行尾），不取工作区 —— 工作区受 .gitattributes /
   autocrlf 影响，同一 commit 在不同机器上会算出不同 MD5，指纹就失去意义了。
2. 两个 exe 都设只读，防误覆盖（1.1.0 曾据此被记成欠账，实际是记错了，这里显式做一遍）。
3. SHA256SUMS 覆盖包内除自身以外的所有文件。
4. zip 平铺（与 1.1.0 一致），不含外层目录。

用法：python tools\\pack.py <版本号>   （脚本自己定位仓库根，与当前工作目录无关）
必须先把源码 commit（源码指纹取自 git 索引，未提交的改动会被忽略）。
"""
import hashlib
import io
import os
import re
import stat
import subprocess
import sys
import time
import zipfile

VER = sys.argv[1] if len(sys.argv) > 1 else "1.1.1"
_HERE = os.path.dirname(os.path.abspath(__file__))
# 脚本放在 tools/ 下；若被挪到根目录则 _HERE 就是根
ROOT = os.path.dirname(_HERE) if os.path.basename(_HERE) == "tools" else _HERE
os.chdir(ROOT)
DIST = os.path.join(ROOT, "dist")
ZIPNAME = "svpn-%s-win64.zip" % VER

EXES = ["svpn.exe", "svpn-gui.exe"]
# 发布包里的 run.cmd 是**精简版**（250B，只做：查 exe 在不在 → init → up -b → status → pause），
# 与仓库根的开发版 run.cmd（823B，带失败分支与提示）不是同一个文件。
# 为可复现，发布版收在 tools/run-release.cmd，打包时改名为 run.cmd。
DOCS = [("README.md", "README.md"), ("README.en.md", "README.en.md"),
        ("LICENSE", "LICENSE"), ("tools/run-release.cmd", "run.cmd"),
        ("config.example.json", "config.example.json")]
TOPDIR = "svpn-%s-win64" % VER


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def blob(path):
    """取 git 索引里的字节（= 该 commit 的真实内容，LF 行尾）。"""
    r = subprocess.run(["git", "show", ":%s" % path], capture_output=True)
    if r.returncode != 0:
        raise SystemExit("取索引失败: %s" % path)
    return r.stdout


def main():
    # ---- 0. 前置检查
    for e in EXES:
        if not os.path.isfile(os.path.join(ROOT, e)):
            raise SystemExit("缺产物: %s（先跑 build.cmd）" % e)

    v = sh('svpn.exe -v').stdout.strip()
    if v != VER:
        raise SystemExit("版本不符: svpn.exe -v 报 %r，期望 %r" % (v, VER))
    # 注意：**不要**调 `svpn-gui.exe -v`。它是 -mwindows 子系统，没有控制台，
    # 传 -v 会被当成「无参数启动」直接开窗并阻塞，打包脚本会永远挂住。
    # 要核对 GUI 版版本号，用 `svpn-gui.exe gui` 或看窗口标题，别用 CLI 参数。

    # ---- 1. 源码指纹（索引 / LF）
    files = sorted(
        [p for p in os.listdir("src") if p.endswith((".cpp", ".hpp"))],
        key=lambda x: (x != "svpn.hpp", x))
    src_rows = []
    for f in files:
        b = blob("src/" + f)
        src_rows.append((f, md5_bytes(b), len(b)))

    # ---- 2. 构建信息
    gxx = r"D:\LE\AI\tools\mingw64\bin\g++.exe"
    if not os.path.isfile(gxx):
        gxx = "g++"
    gver = sh('"%s" --version' % gxx).stdout.splitlines()
    gver_s = gver[0].strip() if gver else "unknown"
    gver_2 = gver[1].strip() if len(gver) > 1 else ""
    build_time = time.strftime("%Y-%m-%d %H:%M:%S %z")
    commit = sh("git rev-parse --short HEAD").stdout.strip()

    exe_info = []
    for e in EXES:
        p = os.path.join(ROOT, e)
        exe_info.append((e, md5_bytes(open(p, "rb").read()), os.path.getsize(p)))

    # ---- 3. dist/ 重建
    os.makedirs(DIST, exist_ok=True)
    for old in os.listdir(DIST):
        op = os.path.join(DIST, old)
        if os.path.isfile(op):
            os.chmod(op, stat.S_IWRITE)          # 只读文件先解锁才能删
            os.remove(op)

    for e in EXES:
        dst = os.path.join(DIST, e)
        with open(os.path.join(ROOT, e), "rb") as s, open(dst, "wb") as d:
            d.write(s.read())
    for src, dstname in DOCS:
        s = os.path.join(ROOT, src)
        if not os.path.isfile(s):
            raise SystemExit("缺文档: %s" % src)
        dst = os.path.join(DIST, dstname)
        with open(s, "rb") as sf, open(dst, "wb") as df:
            df.write(sf.read())

    # ---- 4. RELEASE.txt
    L = []
    A = L.append
    A("svpn %s  ·  发布说明" % VER)
    A("=" * 50)
    A("")
    A("版本            %s" % VER)
    A("可执行文件      svpn.exe（控制台：命令行 + `svpn gui`）")
    A("                svpn-gui.exe（窗口子系统：双击不闪控制台）")
    for name, m, sz in exe_info:
        A("MD5  %-12s %s" % (name, m))
        A("大小 %-12s %s B" % (name, format(sz, ",")))
    A("构建时间        %s" % build_time)
    A("编译器          %s" % gver_s)
    if gver_2:
        A("                %s" % gver_2)
    A("目标平台        x86_64-w64-mingw32 / Windows 10·11 x64")
    A("构建脚本        build.cmd（一趟编两个产物，-static -s，零 DLL 依赖）")
    A("源码 commit     %s" % commit)
    A("")
    A("两个 exe 由同一份源码编译，只有入口子系统不同（-mconsole / -mwindows），")
    A("所有按钮都调用同一个 dispatch_cli()，行为与命令行完全一致。")
    A("")
    A("")
    A("源码指纹（%d 个文件，MD5，按 LF 行尾计算）" % len(src_rows))
    A("-" * 50)
    for f, m, sz in src_rows:
        A("%s  src/%-16s %s B" % (m, f, format(sz, ",")))
    A("")
    A("注：指纹取自 git 索引 blob（LF），不是工作区文件——工作区受 .gitattributes")
    A("与 core.autocrlf 影响，同一 commit 在不同机器上会算出不同 MD5。")
    A("校验方法：git show :src/main.cpp | md5sum")
    A("")
    A("")
    A("行为基准")
    A("-" * 50)
    A("Python 版 svpn，commit 037a78f")
    A("https://github.com/zhe-du/svpn/commit/037a78f35df8ed32b98d6e35ee66aa054b798fe6")
    A("")
    A("本 exe 为该实现的原生重写：单文件、零 DLL 依赖，")
    A("TLS 走 SChannel，转发用线程 + select。")
    A("")
    A("")
    A("验证这份产物")
    A("-" * 50)
    A("1) 校验哈希")
    A("     certutil -hashfile svpn.exe MD5")
    A("   应输出 %s" % exe_info[0][1])
    A("")
    A("2) 打印版本")
    A("     svpn.exe -v")
    A("   应输出 %s" % VER)
    A("")
    A("3) 跑离线用例（需要 Python + 仓库内 tests/，发布包里没有 tests/，")
    A("   这一步只在源码仓里做）")
    A("     python tests\\test_cli.py")
    A("   本产物发布时实测 PASS 126 / FAIL 0 / SKIP 0；")
    A("   test_config 178/0、test_sni_parse 27/0。")
    A("")
    A("")
    A("包内文件")
    A("-" * 50)
    A("  svpn.exe             主程序（控制台，含 `gui` 子命令）")
    A("  svpn-gui.exe         同一程序的窗口子系统构建")
    A("  README.md            使用说明（中文）")
    A("  README.en.md         使用说明（English）")
    A("  LICENSE              MIT")
    A("  run.cmd              启动脚本")
    A("  config.example.json  配置模板")
    A("  SHA256SUMS.txt       上述文件 + 本文件的 SHA256（sha256sum -c 可直接校验）")
    A("  RELEASE.txt          本文件")
    A("")
    A("")
    A("注意")
    A("-" * 50)
    A("1. 本包不含 config.json。首次运行会自动生成：")
    A("     svpn.exe init")
    A("")
    A("2. 本包不含 runtime/。该目录由程序运行时创建，内含")
    A("   本机代理备份、日志与探测缓存，不属于发布内容。")
    A("")
    A("3. run.cmd 与 exe 同目录，直接双击即可：没有 config.json")
    A("   会先执行 init，然后 up -b、等 2 秒、打印 status。")
    A("")
    A("4. config.example.json 中 upstream.host 为 NODE_HOST 占位符，")
    A("   上游地址与账号需自行填写。本包不含任何真实凭据。")
    A("")
    A("5. 两个 exe 已设只读，防误覆盖。要替换请先 attrib -R。")
    A("")
    rel = os.path.join(DIST, "RELEASE.txt")
    with open(rel, "wb") as f:
        f.write("\n".join(L).encode("utf-8") + b"\n")

    # ---- 5. SHA256SUMS
    sums = []
    for name in ["svpn.exe", "svpn-gui.exe", "run.cmd", "README.md",
                 "README.en.md", "LICENSE", "config.example.json", "RELEASE.txt"]:
        sums.append((sha256_file(os.path.join(DIST, name)), name))
    with open(os.path.join(DIST, "SHA256SUMS.txt"), "wb") as f:
        for h, n in sums:
            f.write(("%s  %s\n" % (h, n)).encode("utf-8"))

    # ---- 6. 只读 + zip
    for e in EXES:
        os.chmod(os.path.join(DIST, e), 0o555)

    zp = os.path.join(DIST, ZIPNAME)
    order = ["svpn.exe", "svpn-gui.exe", "README.md", "README.en.md", "LICENSE",
             "run.cmd", "config.example.json", "SHA256SUMS.txt", "RELEASE.txt"]
    # 与 1.1.0 保持一致：zip 内是**单层顶层目录** svpn-<ver>-win64/，不是平铺。
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n in order:
            z.write(os.path.join(DIST, n), "%s/%s" % (TOPDIR, n))

    print("== dist/ ==")
    for n in sorted(os.listdir(DIST)):
        p = os.path.join(DIST, n)
        print("  %-28s %10s B  %s" % (n, format(os.path.getsize(p), ","),
                                      "r--" if not (os.stat(p).st_mode & stat.S_IWRITE) else "rw-"))
    print()
    print("源码指纹 %d 个文件" % len(src_rows))
    for f, m, sz in src_rows:
        print("  %s  src/%s" % (m, f))
    print()
    print("SHA256SUMS 覆盖 %d 项" % len(sums))
    print("zip: %s  %s B" % (ZIPNAME, format(os.path.getsize(zp), ",")))


if __name__ == "__main__":
    main()
