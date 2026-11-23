#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""win_console_smoke.py —— 在【真实 Win32 控制台】上驱动 termux.exe 的冒烟测试。

为什么不用 tests/conpty_drive.py
--------------------------------
conpty_drive 用 CreatePseudoConsole 把 termux.exe 挂到一个嵌套伪控制台上。这条路在
没有交互控制台的机器（GitHub Actions 的 windows runner、本仓库的开发沙箱）上有一个
已知环境问题：**子 shell 会绑到宿主控制台而不是伪控制台**，于是它一启动就退出，
所有需要活窗格的断言只能 SKIP（见 tests/win_smoke.py 里那段对照实验与说明）。

这个脚本换一条路，走的正是用户双击 termux.exe 时的路径：
  1. CreateProcess(CREATE_NO_WINDOW) —— 让 termux 拿到一个**自己的真控制台**；
  2. AttachConsole(pid) + CONOUT$/CONIN$；
  3. ReadConsoleOutputW 读屏、WriteConsoleInputW 敲键（KEY_EVENT，与 conhost 同形）。
不需要交互桌面、不需要 Windows Terminal，CI 上也能跑。

覆盖的四件事（都是运行期断言）
------------------------------
  A. shell 通了：敲 `echo SMOKE_OK_9137`，屏幕上真的出现回显与结果。
  B. 图形直通之后的光标：让窗格输出一条 iTerm2 图片序列；图片确实到达了本进程
     （先查 termux_dump.log），并且**整帧结束时宿主光标停在 shell 提示符末尾**，
     而不是图片锚点（v2.3.7 修的缺陷）。
  C. 分屏里非活动窗格的后台输出：让我先在一个窗格里跑逐秒打点的命令、再分屏把焦点
     移到新窗格；之后**不做任何输入**，屏幕上的 TICK 编号必须继续变大（v2.3.7）。
  D. UI 冒烟：帮助页 / 搜索框 / 命令面板 -> 关于页 / 设置页。

只在 Windows 上跑；别的平台【显式 SKIP 并说明】，返回 0（不是静默通过）。
连 ATTACH 都做不到的环境（例如没有控制台会话的容器）也记 SKIP 并打印原因 ——
那说明这条测试在这台机器上量不了，不能当成通过。
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.environ.get("TERMUX_SMOKE_EXE") or os.path.join(ROOT, "termux.exe")

CREATE_NO_WINDOW = 0x08000000
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
KEY_EVENT = 0x0001
LEFT_CTRL_PRESSED = 0x0008
SHIFT_PRESSED = 0x0010
STILL_ACTIVE = 259

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class COORD(ctypes.Structure):
    _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [("Left", ctypes.c_short), ("Top", ctypes.c_short),
                ("Right", ctypes.c_short), ("Bottom", ctypes.c_short)]


class CONSOLE_SCREEN_BUFFER_INFO(ctypes.Structure):
    _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD),
                ("wAttributes", wt.WORD), ("srWindow", SMALL_RECT),
                ("dwMaximumWindowSize", COORD)]


class CHAR_INFO(ctypes.Structure):
    # ★ conhost 把双宽字符同时存进主格与次格，读回来是「版版本本」这种重复。
    #   断言只用 ASCII，别拿中文去比。
    _fields_ = [("Char", ctypes.c_wchar), ("Attributes", wt.WORD)]


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [("bKeyDown", wt.BOOL), ("wRepeatCount", wt.WORD),
                ("wVirtualKeyCode", wt.WORD), ("wVirtualScanCode", wt.WORD),
                ("UnicodeChar", ctypes.c_wchar), ("dwControlKeyState", wt.DWORD)]


class EVENT_UNION(ctypes.Union):
    _fields_ = [("KeyEvent", KEY_EVENT_RECORD), ("pad", ctypes.c_byte * 16)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [("EventType", wt.WORD), ("Event", EVENT_UNION)]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


k32.CreateFileW.restype = wt.HANDLE
k32.GetConsoleScreenBufferInfo.argtypes = [wt.HANDLE, ctypes.POINTER(CONSOLE_SCREEN_BUFFER_INFO)]
k32.ReadConsoleOutputW.argtypes = [wt.HANDLE, ctypes.POINTER(CHAR_INFO), COORD, COORD,
                                   ctypes.POINTER(SMALL_RECT)]
k32.WriteConsoleInputW.argtypes = [wt.HANDLE, ctypes.POINTER(INPUT_RECORD), wt.DWORD,
                                   ctypes.POINTER(wt.DWORD)]
k32.AttachConsole.argtypes = [wt.DWORD]
k32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                               wt.BOOL, wt.DWORD, ctypes.c_void_p, wt.LPCWSTR,
                               ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)]
k32.CreateProcessW.restype = wt.BOOL


class AttachUnavailable(Exception):
    """这台机器上拿不到 termux 的控制台（不是产品缺陷）。"""


class Term:
    """一个跑在真控制台上的 termux 实例。读屏/敲键都走 conhost 的正规 API。"""

    def __init__(self, exe):
        self.exe = exe
        self.tmp = tempfile.mkdtemp(prefix="termux_wincon_")
        self._stdout = os.environ.get("TERMUX_DUMP")
        os.environ["TERMUX_DUMP"] = "1"   # B 项要拿 termux_dump.log 证明图片真的到了
        si = STARTUPINFOW()
        si.cb = ctypes.sizeof(si)
        pi = PROCESS_INFORMATION()
        cmdline = ctypes.create_unicode_buffer('"%s"' % exe)
        ok = k32.CreateProcessW(ctypes.c_wchar_p(exe), cmdline, None, None, False,
                                CREATE_NO_WINDOW, None, os.path.dirname(os.path.abspath(exe)),
                                ctypes.byref(si), ctypes.byref(pi))
        if not ok:
            shutil.rmtree(self.tmp, ignore_errors=True)
            raise OSError(ctypes.get_last_error(), "CreateProcessW 失败")
        self.pi = pi
        self.hout = None
        self.hin = None
        time.sleep(1.6)
        k32.FreeConsole()
        if not k32.AttachConsole(pi.dwProcessId):
            self._abort("AttachConsole 失败: %d" % ctypes.get_last_error())
        self.hout = k32.CreateFileW("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                    OPEN_EXISTING, 0, None)
        self.hin = k32.CreateFileW("CONIN$", GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                   OPEN_EXISTING, 0, None)
        if not self.hout or not self.hin:
            self._abort("CONOUT$/CONIN$ 打不开")

    def _abort(self, why):
        """Attach 阶段失败：把子进程与临时目录都收掉，再抛 AttachUnavailable。"""
        try:
            k32.TerminateProcess(self.pi.hProcess, 0)
            k32.WaitForSingleObject(self.pi.hProcess, 2000)
            k32.CloseHandle(self.pi.hProcess)
            k32.CloseHandle(self.pi.hThread)
        except Exception:
            pass
        k32.FreeConsole()
        self._restore_env()
        shutil.rmtree(self.tmp, ignore_errors=True)
        raise AttachUnavailable(why)

    # ---- 观测 ----
    def alive(self):
        return k32.WaitForSingleObject(self.pi.hProcess, 0) == 0x102

    def info(self):
        b = CONSOLE_SCREEN_BUFFER_INFO()
        k32.GetConsoleScreenBufferInfo(self.hout, ctypes.byref(b))
        return b

    def lines(self):
        b = self.info()
        if b.dwSize.X < 1 or b.srWindow.Bottom < b.srWindow.Top:
            return []
        cols = b.dwSize.X
        top, bottom = b.srWindow.Top, b.srWindow.Bottom
        rows = bottom - top + 1
        buf = (CHAR_INFO * (cols * rows))()
        region = SMALL_RECT(0, top, cols - 1, bottom)
        k32.ReadConsoleOutputW(self.hout, buf, COORD(cols, rows), COORD(0, 0),
                               ctypes.byref(region))
        return ["".join(buf[r * cols + c].Char for c in range(cols)).rstrip()
                for r in range(rows)]

    def screen(self):
        return "\n".join(self.lines())

    def cursor(self):
        b = self.info()
        return b.dwCursorPosition.Y, b.dwCursorPosition.X

    # ---- 输入 ----
    def key(self, ch, vk=0, ctrl=0, shift=0):
        recs = (INPUT_RECORD * 2)()
        for i, down in enumerate((True, False)):
            recs[i].EventType = KEY_EVENT
            k = recs[i].Event.KeyEvent
            k.bKeyDown = down
            k.wRepeatCount = 1
            k.wVirtualKeyCode = vk
            k.UnicodeChar = ch
            k.dwControlKeyState = ctrl | shift
        n = wt.DWORD()
        k32.WriteConsoleInputW(self.hin, recs, 2, ctypes.byref(n))
        time.sleep(0.03)

    def type(self, text):
        for c in text:
            if c == "\r":
                self.key("\r", 0x0D)
            elif c == "\x02":                       # Ctrl+B（默认前缀）
                self.key("\x02", 0x42, LEFT_CTRL_PRESSED)
            elif c == "_":
                self.key("_", 0xBD, shift=SHIFT_PRESSED)
            elif c == ":":
                self.key(":", 0xBA, shift=SHIFT_PRESSED)
            elif c == "?":
                self.key("?", 0xBF, shift=SHIFT_PRESSED)
            else:
                self.key(c, ord(c.upper()) if c.isalpha() else 0)

    def wait_for(self, needle, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.screen():
                return True
            time.sleep(0.15)
        return False

    # ---- 收尾 ----
    def _restore_env(self):
        if self._stdout is None:
            os.environ.pop("TERMUX_DUMP", None)
        else:
            os.environ["TERMUX_DUMP"] = self._stdout

    def close(self):
        try:
            for h in (self.hin, self.hout):
                if h:
                    k32.CloseHandle(h)
        except Exception:
            pass
        try:
            k32.TerminateProcess(self.pi.hProcess, 0)
            k32.WaitForSingleObject(self.pi.hProcess, 3000)
            k32.CloseHandle(self.pi.hProcess)
            k32.CloseHandle(self.pi.hThread)
        except Exception:
            pass
        k32.FreeConsole()
        self._restore_env()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def diagnostics(self):
        out = ["alive=%s" % self.alive()]
        try:
            for i, ln in enumerate(self.lines()):
                if ln.strip():
                    out.append("%2d| %s" % (i, ln))
        except Exception as ex:      # 控制台已经没了
            out.append("<读屏失败: %s>" % ex)
        return "\n".join(out)


def emit_payload(tmp, name, payload):
    """把一段字节写进一个 python 脚本（窗格里用 python 输出它）。"""
    path = os.path.join(tmp, "emit_%s.py" % name)
    with open(path, "wb") as f:
        f.write(b"import sys\nsys.stdout.buffer.write(%r)\nsys.stdout.buffer.flush()\n"
                % payload)
    return path


def py_cmd(script):
    """窗格里执行 python 脚本的命令行。sys.executable 在 MSYS2 下也是 Windows 路径；
    万一拿到的是 POSIX 风格路径，就退回 PATH 上的 python。"""
    exe = sys.executable or ""
    if not re.match(r"^[A-Za-z]:[\\/]", exe):
        exe = "python"
    return '"%s" "%s"' % (exe, script)


class Report:
    def __init__(self):
        self.fails = []
        self.skips = []

    def ck(self, name, cond, detail=""):
        if cond:
            print("  [ok]   %s" % name)
        else:
            print("  [FAIL] %s %s" % (name, detail))
            self.fails.append(name)

    def skip(self, name, why):
        print("  [SKIP] %s —— %s" % (name, why))
        self.skips.append(name)


def prompt_row(t):
    """最后一条非空行与光标所在行。"""
    lines = t.lines()
    idx = [i for i, s in enumerate(lines) if s.strip()]
    return (idx[-1] if idx else -1), lines


def check_startup(t, rep):
    if not t.wait_for(">", 12):
        print("  [FAIL] 等不到 shell 提示符")
        print(t.diagnostics())
        rep.fails.append("shell 提示符")
        return False
    rep.ck("进程起来了（真控制台）", t.alive())
    return True


def check_echo(t, rep):
    marker = "SMOKE_OK_9137"
    t.type("echo %s\r" % marker)
    # 命令行本身就把 marker 回显了一遍 ⇒ 必须等它出现【两次】（命令行 + 命令输出），
    # 只等一次会在命令还没跑完时提前通过。
    end = time.time() + 8
    while time.time() < end and t.screen().count(marker) < 2:
        time.sleep(0.15)
    seen = t.screen().count(marker)
    rep.ck("echo 回显与结果都出现", seen >= 2, "屏幕上只有 %d 处" % seen)
    end = time.time() + 4
    while time.time() < end:
        last, lines = prompt_row(t)
        if last >= 0 and lines[last].rstrip().endswith(">"):
            break
        time.sleep(0.15)
    last, lines = prompt_row(t)
    rep.ck("提示符停在最后一行", last >= 0 and lines[last].rstrip().endswith(">"),
           "最后一行=%r" % (lines[last] if last >= 0 else ""))


def check_image_cursor(t, rep, tmp):
    """B：图片之后的光标必须回到 shell 提示符末尾。

    先确认图片真的到了本进程（termux_dump.log），否则这条会【平凡通过】——
    宿主/系统把 OSC 吃掉时看起来一样正常，必须显式区分。
    """
    payload = b"\x1b]1337;File=inline=1;size=8;width=100;aGVsbG8=\x07"
    script = emit_payload(tmp, "iterm", payload)
    dump = os.path.join(os.path.dirname(os.path.abspath(t.exe)), "termux_dump.log")
    try:
        if os.path.exists(dump):
            os.remove(dump)
    except OSError:
        pass
    t.type(py_cmd(script) + "\r")
    end = time.time() + 8
    arrived = False
    while time.time() < end:
        try:
            if os.path.exists(dump) and payload in open(dump, "rb").read():
                arrived = True
                break
        except OSError:
            pass
        time.sleep(0.2)
    if not arrived:
        rep.skip("图片之后光标归位",
                 "这条 iTerm2 序列没有从窗格 ConPTY 到达 termux（OS/宿主差异），"
                 "这条量不了；渲染层的顺序另有 verify_gfx_paint.py 覆盖")
        return
    time.sleep(1.2)
    last, lines = prompt_row(t)
    row, col = t.cursor()
    prompt = lines[last].rstrip() if last >= 0 else ""
    rep.ck("图片之后光标停在提示符行", row == last,
           "cursor=(%d,%d) 末行=%d" % (row, col, last))
    rep.ck("图片之后光标停在提示符末尾（不是图片锚点）",
           col >= max(1, len(prompt) - 1),
           "cursor 列=%d 提示符长=%d" % (col, len(prompt)))


def check_ui(t, rep):
    t.type("\x02?")
    rep.ck("帮助页能打开", t.wait_for("Terminal Multiplexer", 6))
    t.key("\x1b", 0x1B)
    time.sleep(0.5)
    t.type("\x02/")
    time.sleep(0.6)
    t.type("HELLO_SEARCH")
    rep.ck("搜索框能输入关键词", t.wait_for("HELLO_SEARCH", 5))
    t.key("\x1b", 0x1B)
    time.sleep(0.5)
    t.type("\x02:")
    time.sleep(0.8)
    t.type("about\r")
    rep.ck("命令面板 -> 关于页", t.wait_for("wudream813/super-termux", 6))
    t.type("\x02x")          # 关掉关于页
    time.sleep(0.6)
    t.type("\x02S")
    rep.ck("设置页能打开", t.wait_for("Settings Panel", 6))


def check_split_background(rep):
    """C：分屏之后，非活动窗格的输出在【没有任何输入】时也要继续刷新。"""
    try:
        t = Term(EXE)
    except AttachUnavailable as ex:
        rep.skip("分屏后台窗格刷新", str(ex))
        return
    try:
        if not check_startup(t, rep):
            return
        t.type("for /l %i in (1,1,40) do @(echo TICK%i & timeout /t 1 >nul)\r")
        time.sleep(2.0)
        t.type("\x02_")          # Ctrl+B _ = 上下分屏，焦点移到新窗格
        time.sleep(3.0)          # 等新窗格的 shell 启动输出落定
        before = max([int(m) for m in re.findall(r"TICK(\d+)", t.screen())] or [0])
        time.sleep(4.5)          # 期间不做任何输入
        after = max([int(m) for m in re.findall(r"TICK(\d+)", t.screen())] or [0])
        rep.ck("后台窗格无输入也在刷新", after > before,
               "4.5 秒内最大 TICK %d -> %d" % (before, after))
    finally:
        t.close()


def main():
    # 判据文案是中文：Windows 控制台默认 cp936，遇到编码不了的字符会直接把脚本搞挂
    # （CI 里靠 PYTHONUTF8 顶着，本地不一定有）。别让「输出不了」伪装成「判据不过」。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    if os.name != "nt":
        print("[SKIP] win_console_smoke.py 只在 Windows 上跑（当前 os.name=%r）。"
              % os.name)
        print("       POSIX 侧对应的是 make check-posix / tests/posix_smoke.py。")
        return 0
    if not os.path.exists(EXE):
        print("[FAIL] 找不到 %s，先构建 termux.exe" % EXE)
        return 1

    print("=== Windows 真控制台冒烟测试（%s）===" % EXE)
    rep = Report()

    # --- 第一段：单窗格（echo / 图片光标 / UI）---
    try:
        t = Term(EXE)
    except AttachUnavailable as ex:
        rep.skip("真控制台冒烟", str(ex))
        print("\n这台机器上量不了（不是产品缺陷），按 SKIP 处理。")
        return 0
    except OSError as ex:
        print("[FAIL] 起不来: %s" % ex)
        return 1
    try:
        if check_startup(t, rep):
            check_echo(t, rep)
            check_image_cursor(t, rep, t.tmp)
            check_ui(t, rep)
    finally:
        t.close()

    # --- 第二段：分屏后台刷新（单独一个会话，免得前面 UI 的状态干扰）---
    check_split_background(rep)

    print()
    if rep.fails:
        print("%d 项失败：%s" % (len(rep.fails), "；".join(rep.fails)))
        return 1
    if rep.skips:
        print("硬断言全过，但有 %d 项被 SKIP：%s" % (len(rep.skips), "；".join(rep.skips)))
        return 0
    print("Windows 真控制台冒烟测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
