#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""conpty_drive.py —— 在真 ConPTY 里驱动 termux.exe 的测试驱动（Windows 版 drive.py）。

为什么要有它
------------
`tests/drive.py` 是 POSIX 的：`pty.openpty()` / `fcntl` / `termios` / `TIOCSCTTY`
在 Windows 上一个都不存在。所以移植完成后 Windows 侧**一个运行时测试都没有** ——
CI 的 windows 作业只做构建 + 静态检查。而 Windows 恰恰是最初「resize 丢历史」
那个 bug 的老家，九轮盲改全败就是因为没有 Windows 上的自动化复现手段。

接口刻意和 drive.Term 对齐（cols/rows、send、resize、frame、grid、nframes、
quit、close），这样同一套断言两边都能跑。读帧的那几个方法直接继承 drive.Term：
它们只是解析 `render_dump.log`（TERMUX_DUMP=1 时程序自己写的），完全平台无关。

实现要点
--------
- `CreatePseudoConsole`（kernel32，Win10 1809+）拿一个 HPCON。
- 子进程必须通过 `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` (0x00020016) 属性列表
  挂到这个 HPCON 上，并且 `dwCreationFlags` 带 `EXTENDED_STARTUPINFO_PRESENT`
  (0x00080000)、用 `STARTUPINFOEXW`。少了任何一样，子进程会挂到调用者的
  控制台上去，测试就读不到东西了。
- ConPTY 的输出端必须【持续读走】。不读的话管道缓冲填满，子进程 write 阻塞，
  表现成「卡死」——和真 bug 长得一模一样，很容易误判。这里用后台线程一直抽干。

只在 Windows 上能用。在别的平台 import 不会炸（方便静态检查），但实例化会
抛 RuntimeError 并说明原因。
"""
import io
import os
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402  为了继承读 render_dump.log 的那几个方法

IS_WINDOWS = (os.name == "nt")

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
    EXTENDED_STARTUPINFO_PRESENT = 0x00080000
    # ★ 传宽字符环境块必须带这个标志，否则 Windows 按 ANSI 解析那块内存，
    #   轻则环境变量全乱，重则 CreateProcessW 直接 ERROR_INVALID_PARAMETER。
    CREATE_UNICODE_ENVIRONMENT = 0x00000400
    INFINITE = 0xFFFFFFFF
    WAIT_TIMEOUT = 0x00000102        # 258：对象仍活跃
    STILL_ACTIVE = 259               # GetExitCodeProcess 用它表示"还在跑"

    class COORD(ctypes.Structure):
        _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("lpReserved", wintypes.LPWSTR),
            ("lpDesktop", wintypes.LPWSTR),
            ("lpTitle", wintypes.LPWSTR),
            ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
            ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
            ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
            ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
            ("hStdInput", wintypes.HANDLE),
            ("hStdOutput", wintypes.HANDLE),
            ("hStdError", wintypes.HANDLE),
        ]

    class STARTUPINFOEXW(ctypes.Structure):
        _fields_ = [("StartupInfo", STARTUPINFOW),
                    ("lpAttributeList", ctypes.c_void_p)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                    ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]

    _k32.CreatePipe.argtypes = [ctypes.POINTER(wintypes.HANDLE),
                                ctypes.POINTER(wintypes.HANDLE),
                                ctypes.c_void_p, wintypes.DWORD]
    _k32.CreatePseudoConsole.argtypes = [COORD, wintypes.HANDLE, wintypes.HANDLE,
                                         wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p)]
    _k32.CreatePseudoConsole.restype = ctypes.c_long          # HRESULT
    _k32.ResizePseudoConsole.argtypes = [ctypes.c_void_p, COORD]
    _k32.ResizePseudoConsole.restype = ctypes.c_long
    _k32.ClosePseudoConsole.argtypes = [ctypes.c_void_p]
    _k32.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD,
                                                       wintypes.DWORD,
                                                       ctypes.POINTER(ctypes.c_size_t)]
    _k32.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD,
                                               ctypes.c_void_p, ctypes.c_void_p,
                                               ctypes.c_size_t, ctypes.c_void_p,
                                               ctypes.POINTER(ctypes.c_size_t)]
    _k32.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
    # ★ 下面这几个也必须显式声明 argtypes。不设的话 ctypes 按默认 int 传参，
    #   x64 上 HANDLE（64 位）会被截断成 32 位 —— 症状是 ReadFile/WriteFile
    #   报 ERROR_INVALID_HANDLE，看起来像 ConPTY 挂了，其实是自己的声明错了。
    _k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.ReadFile.restype = wintypes.BOOL
    _k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    _k32.WriteFile.restype = wintypes.BOOL
    _k32.CloseHandle.argtypes = [wintypes.HANDLE]
    _k32.CloseHandle.restype = wintypes.BOOL
    _k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _k32.WaitForSingleObject.restype = wintypes.DWORD
    _k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _k32.TerminateProcess.restype = wintypes.BOOL
    _k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    _k32.GetExitCodeProcess.restype = wintypes.BOOL
    _k32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR,
                                    ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL,
                                    wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                    ctypes.POINTER(STARTUPINFOEXW),
                                    ctypes.POINTER(PROCESS_INFORMATION)]


def _fail(msg):
    raise RuntimeError(msg)


def _env_block(env):
    """把 dict 拼成 Windows 的 Unicode 环境块："K=V\\0K2=V2\\0\\0"。"""
    items = ["%s=%s" % (k, v) for k, v in env.items() if v is not None]
    return ctypes.create_unicode_buffer("\0".join(items) + "\0\0")


class Term(drive.Term):
    """ConPTY 版的 drive.Term。

    继承自 drive.Term 只是为了复用 frame()/nframes()/frame_sizes()/grid()
    —— 它们只读 render_dump.log，不含任何平台调用。__init__ 完全重写，
    不调用基类的（基类那个是 pty 的）。
    """

    def __init__(self, cols=100, rows=24, dump=True, env=None):
        if not IS_WINDOWS:
            _fail("conpty_drive 只能在 Windows 上用（当前 os.name=%r）。"
                  "POSIX 请用 tests/drive.py。" % os.name)
        self.cols, self.rows = cols, rows
        self.tmp = tempfile.mkdtemp(prefix="termux_conpty_")

        e = dict(os.environ)
        e["TERM"] = "xterm-256color"
        if dump:
            e["TERMUX_DUMP"] = "1"
        if env:
            e.update(env)

        # 两对管道：in_read/in_write 给 ConPTY 当输入，out_read/out_write 当输出
        self._in_read = wintypes.HANDLE()
        self._in_write = wintypes.HANDLE()
        self._out_read = wintypes.HANDLE()
        self._out_write = wintypes.HANDLE()
        if not _k32.CreatePipe(ctypes.byref(self._in_read), ctypes.byref(self._in_write),
                               None, 0):
            _fail("CreatePipe(输入) 失败: %d" % ctypes.get_last_error())
        if not _k32.CreatePipe(ctypes.byref(self._out_read), ctypes.byref(self._out_write),
                               None, 0):
            _fail("CreatePipe(输出) 失败: %d" % ctypes.get_last_error())

        self._hpc = ctypes.c_void_p()
        hr = _k32.CreatePseudoConsole(COORD(cols, rows), self._in_read,
                                      self._out_write, 0, ctypes.byref(self._hpc))
        if hr != 0:
            _fail("CreatePseudoConsole 失败 HRESULT=0x%08x —— 需要 Windows 10 1809+" % (hr & 0xFFFFFFFF))

        # 属性列表：把子进程挂到 HPCON 上
        size = ctypes.c_size_t(0)
        _k32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
        self._attr = (ctypes.c_byte * size.value)()
        if not _k32.InitializeProcThreadAttributeList(self._attr, 1, 0, ctypes.byref(size)):
            _fail("InitializeProcThreadAttributeList 失败: %d" % ctypes.get_last_error())
        # ★★★ lpValue 必须传 HPCON 的【值本身】，不是 ctypes.byref(self._hpc)。
        #   这和 UpdateProcThreadAttribute 其它属性的惯例【相反】——文档写的是
        #   「lpValue: A pointer to the attribute value」，但 PSEUDOCONSOLE 这一项
        #   微软自己的示例（echocon）传的就是 hPC 本身。传 byref 的话，属性列表里
        #   存的"句柄"会变成【那个变量的地址】，CreateProcessW 拿到无效 HPCON，
        #   直接返回 ERROR_INVALID_PARAMETER (87)。
        #   CI 的 windows 作业第五轮就是这么挂的（2026-09-22）：
        #       RuntimeError: CreateProcessW 失败: 87
        #   对照来源：github.com/13angs/switchboard/pull/75，作者也踩过同一个坑，
        #   并确认微软示例是传值。
        if not _k32.UpdateProcThreadAttribute(
                self._attr, 0,
                ctypes.c_void_p(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE),
                self._hpc, ctypes.sizeof(self._hpc), None, None):
            _fail("UpdateProcThreadAttribute 失败: %d" % ctypes.get_last_error())

        si = STARTUPINFOEXW()
        si.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
        si.lpAttributeList = ctypes.cast(self._attr, ctypes.c_void_p)

        pi = PROCESS_INFORMATION()
        # ★ lpCommandLine 必须是【可写】缓冲，CreateProcessW 会就地改写它
        cmdline = ctypes.create_unicode_buffer('"%s"' % EXE)
        ok = _k32.CreateProcessW(
            EXE, cmdline, None, None, False,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            _env_block(e), self.tmp,
            ctypes.byref(si), ctypes.byref(pi))
        if not ok:
            _fail("CreateProcessW 失败: %d（EXE=%s）" % (ctypes.get_last_error(), EXE))
        self._hproc = pi.hProcess
        self._hthread = pi.hThread
        self.pid = pi.dwProcessId

        _k32.DeleteProcThreadAttributeList(self._attr)
        _k32.CloseHandle(self._hthread)
        # ConPTY 已经复制了这两个端，父进程可以关掉自己那份
        _k32.CloseHandle(self._in_read)
        _k32.CloseHandle(self._out_write)

        self.raw = b""
        self._stop = threading.Event()
        # ★ 必须一直把 ConPTY 的输出抽干，否则缓冲满了子进程 write 会阻塞，
        #   看起来和「卡死」一模一样 —— 极易误判成真 bug。
        self._pump = threading.Thread(target=self._drain_loop, daemon=True)
        self._pump.start()

    # ---- IO ----
    def _drain_loop(self):
        buf = ctypes.create_string_buffer(65536)
        got = wintypes.DWORD(0)
        while not self._stop.is_set():
            if _k32.ReadFile(self._out_read, buf, 65536, ctypes.byref(got), None):
                if got.value:
                    self.raw += buf.raw[:got.value]
            else:
                err = ctypes.get_last_error()
                if err in (109, 232, 6):      # BROKEN_PIPE / NO_DATA / INVALID_HANDLE
                    break
                time.sleep(0.01)

    def drain(self, t):
        """对齐 drive.Term 的接口：等 t 秒（后台线程一直在收）。"""
        time.sleep(t)

    def send(self, data, wait=0.6):
        if isinstance(data, str):
            data = data.encode()
        written = wintypes.DWORD(0)
        _k32.WriteFile(self._in_write, data, len(data), ctypes.byref(written), None)
        self.drain(wait)
        return written.value

    def set_winsize(self, cols, rows):
        self.cols, self.rows = cols, rows
        hr = _k32.ResizePseudoConsole(self._hpc, COORD(cols, rows))
        if hr != 0:
            raise RuntimeError("ResizePseudoConsole 失败 HRESULT=0x%08x" % (hr & 0xFFFFFFFF))

    def resize(self, cols, rows, wait=0.8):
        self.set_winsize(cols, rows)
        self.drain(wait)

    def alive(self):
        # ★ 原来写成 == 259 是错的：259 是 STILL_ACTIVE，那是 GetExitCodeProcess
        #   的返回值；WaitForSingleObject 在对象【仍活跃】时返回的是
        #   WAIT_TIMEOUT = 0x102 = 258。写 259 的话这个函数【永远返回 False】，
        #   于是「进程起来了」这条断言必然失败（CI 第六轮就是这么挂的）。
        return _k32.WaitForSingleObject(self._hproc, 0) == WAIT_TIMEOUT

    def exit_code(self):
        """子进程退出码；还在跑则返回 None。用来在断言失败时说清楚是怎么死的。"""
        code = wintypes.DWORD(0)
        if not _k32.GetExitCodeProcess(self._hproc, ctypes.byref(code)):
            return None
        return None if code.value == STILL_ACTIVE else code.value

    def diagnostics(self):
        """断言失败时打印这些，免得只能靠猜（这里没有 Windows，改一轮要等 4 分钟）。"""
        d = os.path.join(self.tmp, "render_dump.log")
        exists = os.path.exists(d)
        size = os.path.getsize(d) if exists else -1
        # ★ 必须把【已读到的原始字节】打出来：CI 第七轮只看到「已读到 16 字节 +
        #   exit_code=1」，而 main() 里 return 1 的两处（"no console attached" /
        #   "cannot query console buffer"）都写 stderr —— 不看字节根本没法区分
        #   是启动就失败、还是渲染了一半崩掉。
        # ★ mouse_dump.log 由 src/main.c:197-203 写出，位置正好在【那两个 return 1 之后】
        #   （main.c:168 "no console attached" / :174 "cannot query console buffer"）。
        #   所以它存不存在能直接二分出"死在哪一段"，而且内容里带 host=%dx%d，
        #   能看到应用在 ConPTY 下【自以为】拿到的终端尺寸。
        md = os.path.join(self.tmp, "mouse_dump.log")
        md_txt = ""
        if os.path.exists(md):
            try:
                md_txt = io.open(md, "r", encoding="utf-8", errors="replace").read()[-900:]
            except OSError:
                md_txt = "<读不了>"
        # ★ 末帧内容是最有用的一条：断言失败时光知道"没找到某个字面量"没法判断
        #   是【画面没变】还是【画面变了但文案不同】。CI 第十一轮就是这样卡住的：
        #   「关于页出现」失败但「版本号=2.0.2」通过，看着自相矛盾 —— 其实是因为
        #   帮助页头部本来就有 "版本 v2.0.2 | ..."，而关于页压根没打开（帧数一直是 2）。
        # ★ 这两个文件是【现成的】诊断源，之前一直没读：
        #   termux_dump.log   src/globals.c:97  dump_pane_bytes() —— 窗格读线程
        #                     【实际收到】的字节。cmd.exe 的输出有没有真的进到
        #                     termux，看这个就知道，不用猜。
        #   conpty_source.log src/conpty_loader.c:78 —— 加载的是捆绑的 conpty.dll
        #                     还是系统自带的，以及 flags。
        #   另外把 tmp 下所有 *.log 的大小列出来，避免再有"文件其实存在但没人看"。
        logs = {}
        try:
            for nm in sorted(os.listdir(self.tmp)):
                if nm.endswith(".log"):
                    fp = os.path.join(self.tmp, nm)
                    logs[nm] = os.path.getsize(fp)
        except OSError:
            pass

        def _head(nm, n):
            fp = os.path.join(self.tmp, nm)
            if not os.path.exists(fp):
                return "<无此文件>"
            try:
                return io.open(fp, "r", encoding="utf-8", errors="replace").read()[:n]
            except OSError as ex:
                return "<读不了: %s>" % ex

        try:
            last = self.frame()
        except Exception as ex:      # frame() 在文件缺失时会抛
            last = "<取不到: %s>" % ex
        return ("alive=%s exit_code=%s 已读到输出=%d 字节  render_dump.log存在=%s 大小=%s  帧数=%s"
                "\n         mouse_dump.log存在=%s 内容=%r"
                "\n         末帧(前700)=%r"
                "\n         已读字节(前300)=%r"
                "\n         tmp下的log=%s"
                "\n         conpty_source=%r"
                "\n         窗格收到的字节(termux_dump 前500)=%r"
                % (self.alive(), self.exit_code(), len(self.raw),
                   exists, size, self.nframes(),
                   os.path.exists(md), md_txt, last[:700], self.raw[:300],
                   logs, _head("conpty_source.log", 200),
                   _head("termux_dump.log", 500)))

    def quit(self, timeout=5):
        _k32.TerminateProcess(self._hproc, 0)
        _k32.WaitForSingleObject(self._hproc, int(timeout * 1000))

    def close(self):
        self._stop.set()
        try:
            _k32.ClosePseudoConsole(self._hpc)
        except Exception:
            pass
        for h in (self._in_write, self._out_read, self._hproc):
            try:
                _k32.CloseHandle(h)
            except Exception:
                pass

    # ---- 这几个在 Windows 上没有对应物，显式说明而不是静默返回假数据 ----
    def fds(self):
        raise NotImplementedError("Windows 上没有 /proc/<pid>/fd；fd 泄漏检查请用 POSIX")

    def fd_targets(self):
        raise NotImplementedError("同上")

    def kids(self):
        raise NotImplementedError("残留子进程检查请用 POSIX 侧的 kids()")


# termux.exe 的路径：优先 TERMUX_SMOKE_EXE，否则仓库根目录的 termux.exe
EXE = os.environ.get("TERMUX_SMOKE_EXE") or os.path.join(drive.ROOT, "termux.exe")
PREFIX = drive.PREFIX
