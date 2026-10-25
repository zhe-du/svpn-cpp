"""按窗口类名找 svpn GUI，置前并截图存 BMP（不依赖 PIL）。"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowDC.restype = wt.HDC
user32.GetWindowDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]

gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p,
                            ctypes.c_void_p, wt.UINT]

hwnd = user32.FindWindowW("SvpnGuiWindow", None)
if not hwnd:
    print("WINDOW_NOT_FOUND")
    sys.exit(1)
print("hwnd=", hwnd)
user32.ShowWindow(hwnd, 9)          # SW_RESTORE
user32.SetForegroundWindow(hwnd)
user32.UpdateWindow(hwnd)

r = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(r))
w, h = r.right - r.left, r.bottom - r.top
print("rect=", r.left, r.top, w, h)

hdc = user32.GetWindowDC(hwnd)
mem = gdi32.CreateCompatibleDC(hdc)
bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
gdi32.SelectObject(mem, bmp)
gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)   # SRCCOPY

class BIH(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]

bih = BIH()
bih.biSize = ctypes.sizeof(BIH)
bih.biWidth = w
bih.biHeight = h
bih.biPlanes = 1
bih.biBitCount = 24
bih.biCompression = 0
stride = (w * 3 + 3) & ~3
buf = ctypes.create_string_buffer(stride * h)
gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0)

out = sys.argv[1] if len(sys.argv) > 1 else "gui.bmp"
with open(out, "wb") as f:
    f.write(b"BM" + struct.pack("<IHHI", 14 + 40 + len(buf), 0, 0, 14 + 40))
    f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(buf), 2835, 2835, 0, 0))
    f.write(buf.raw)
print("saved", out)
