#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_port.py —— 跨平台移植的不变量检查。

这一份不是「跑一遍看看」，而是把移植过程中【真的踩过、而且编译期看不出来】的坑
逐条钉成断言。每一条后面都写了它对应的事故，改动前请先读那行注释。

用法：python3 tests/verify_port.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FAILS = []


def read(*p):
    return (ROOT.joinpath(*p)).read_text(encoding="utf-8")


NPASS = 0


def ck(name, cond, extra=""):
    global NPASS
    if cond:
        NPASS += 1
        print("  [ok]   %s" % name)
    else:
        print("  [FAIL] %s %s" % (name, extra))
        FAILS.append(name)


def strip_comments(src):
    """去掉 C 注释和字符串字面量，免得正则被注释里的字给骗了。"""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r'"(\\.|[^"\\])*"', '""', src)
    src = re.sub(r"'(\\.|[^'\\])*'", "''", src)
    return src


WINCOMPAT = read("include", "wincompat.h")
COMMON = read("include", "common.h")
PLATFORM_H = read("include", "platform.h")
PLAT_WIN = read("src", "platform_win.c")
PLAT_POSIX = read("src", "platform_posix.c")
TERM_INPUT = read("src", "term_input_posix.c")
MAIN_POSIX = read("src", "main_posix.c")
CONFIG = read("src", "config.c")
INPUT = read("src", "input.c")
PANE = read("src", "pane.c")
MAKEFILE = read("Makefile")

# ===========================================================================
print("=== 1) 平台层接口两侧都实现了 ===")
# 为什么重要：plat_* 只在链接期才暴露「某一侧忘了实现」。逐文件 -fsyntax-only
# 抓不到，make linux 也要等到链接那一步才报，很容易漏。
declared = sorted(set(re.findall(r"\b(plat_[a-z0-9_]+)\s*\(", PLATFORM_H)))
ck("platform.h 里声明了 plat_* 接口", len(declared) >= 10, "只找到 %d 个" % len(declared))
# POSIX 侧的实现分散在两个文件里：进程/控制台/剪贴板在 platform_posix.c，
# 输入字节流翻译在 term_input_posix.c（plat_console_read 就在那儿）。
POSIX_ALL = strip_comments(PLAT_POSIX) + "\n" + strip_comments(TERM_INPUT) + "\n" + strip_comments(MAIN_POSIX)
for fn in declared:
    in_win = re.search(r"\b%s\s*\(" % fn, strip_comments(PLAT_WIN)) is not None
    in_posix = re.search(r"\b%s\s*\(" % fn, POSIX_ALL) is not None
    ck("%s 两侧都有实现" % fn, in_win and in_posix,
       "win=%s posix=%s" % (in_win, in_posix))

# ===========================================================================
print("=== 2) CRITICAL_SECTION 必须可重入 ===")
# 事故：Windows 的 CRITICAL_SECTION 同一线程可以重复 Enter，引擎真的这么用
# （render_screen 拿 g_mux.cs，再经 render_split -> split_compute_rects ->
#  pane_resize_to 又拿一次）。用普通 pthread_mutex 会自死锁 —— 表现是
# 「一分屏整个界面冻住，宿主侧一个字节都不再输出」，编译期毫无征兆。
ck("InitializeCriticalSection 用了 PTHREAD_MUTEX_RECURSIVE",
   "PTHREAD_MUTEX_RECURSIVE" in WINCOMPAT)
ck("EnterCriticalSection 走的是 pthread_mutex_lock",
   "pthread_mutex_lock" in WINCOMPAT)

# ===========================================================================
print("=== 3) 默认 shell 不能写死 cmd.exe ===")
# 事故：config.c 的内置启动项、input.c 的分屏/自定义命令都硬写 cmd.exe，
# 在 POSIX 上 execvp("cmd.exe") 必然失败 —— 新窗格是空白的，且没有任何报错。
ck("platform.h 提供 TERMUX_DEFAULT_SHELL_W / _U8",
   "TERMUX_DEFAULT_SHELL_W" in PLATFORM_H and "TERMUX_DEFAULT_SHELL_U8" in PLATFORM_H)
for fname, src in (("config.c", CONFIG), ("input.c", INPUT)):
    body = strip_comments(src)
    # Windows 分支里出现 cmd.exe 是应该的；只查「不在 #ifdef _WIN32 里」的。
    outside = []
    depth_win = 0
    for line in body.split("\n"):
        st = line.strip()
        if st.startswith("#ifdef _WIN32") or st.startswith("#if defined(_WIN32"):
            depth_win += 1
        elif st.startswith("#endif"):
            depth_win = max(0, depth_win - 1)
        elif st.startswith("#else") and depth_win:
            depth_win = 0            # #else 之后就是非 Windows 分支
        elif depth_win == 0 and "cmd.exe" in line:
            outside.append(line.strip())
    ck("%s 的非 Windows 分支里没有 cmd.exe" % fname, not outside,
       "残留：%s" % outside[:2])

# ===========================================================================
print("=== 4) 构建配置 ===")
# 事故：Makefile 的 $(CC) 是 MinGW 交叉编译器。linux 目标曾经用 $(CC)，
# 于是拿着 mingw 的头去找 poll.h，报「poll.h: No such file or directory」。
posix_recipe = re.search(r"^linux:\n((?:\t.*\n)+)", MAKEFILE, re.M)
ck("Makefile 有 linux 目标", posix_recipe is not None)
if posix_recipe:
    ck("linux 目标用 $(POSIX_CC) 而不是 $(CC)",
       "$(POSIX_CC)" in posix_recipe.group(1) and "$(CC)" not in posix_recipe.group(1),
       repr(posix_recipe.group(1)[:80]))
darwin_recipe = re.search(r"^darwin:\n((?:\t.*\n)+)", MAKEFILE, re.M)
ck("darwin 目标不链 -lutil（macOS 的 forkpty 在 libSystem 里）",
   darwin_recipe is not None and "-lutil" not in darwin_recipe.group(1))
# 事故：darwin 目标在 Linux 上会【成功】编出一个 ELF Linux 二进制，只是名字叫
# termux-macos。看起来像 macOS 构建通过了，实际一次都没经过 Apple 的编译器。
ck("darwin 目标会拒绝在非 macOS 上跑（防止假绿）",
   darwin_recipe is not None and 'uname -s' in darwin_recipe.group(1)
   and "Darwin" in darwin_recipe.group(1))
ck("clean 会清掉 POSIX 产物",
   re.search(r"^clean:\n((?:\t.*\n)+)", MAKEFILE, re.M) is not None and
   "termux-linux" in re.search(r"^clean:\n((?:\t.*\n)+)", MAKEFILE, re.M).group(1))
ck("POSIX_SRC 排除了 Windows 专属文件",
   all(x not in MAKEFILE.split("POSIX_SRC =")[1].split("\n\n")[0]
       for x in ("src/main.c", "platform_win.c", "conpty_loader.c")))

# ===========================================================================
print("=== 5) macOS 专有分支 ===")
# 事故：_NSGetExecutablePath 声明在 <mach-o/dyld.h>，之前误写成 <crt_externs.h>
# （那个头给的是 _NSGetEnviron）。Xcode 15 起的 clang 把隐式函数声明当错误，
# macOS 上直接编不过 —— 而沙箱里没有 macOS，只能靠这条断言守住。
apple_block = re.search(r"#ifdef __APPLE__\n(.*?)#else", PLAT_POSIX, re.S)
ck("platform_posix.c 的 Apple 分支包含 <mach-o/dyld.h>",
   apple_block is not None and "mach-o/dyld.h" in apple_block.group(1))
ck("platform_posix.c 的 Apple 分支包含 <util.h>（forkpty）",
   apple_block is not None and "<util.h>" in apple_block.group(1))
ck("非 Apple 分支包含 <pty.h>",
   re.search(r"#else\n#include <pty\.h>", PLAT_POSIX) is not None)
ck("剪贴板在 Apple 上用 pbcopy", 'prog = "pbcopy"' in PLAT_POSIX)
# /proc 是 Linux 专有的，只能出现在非 Apple 分支里
for m in re.finditer(r"/proc/", PLAT_POSIX):
    head = PLAT_POSIX[:m.start()]
    last_apple = head.rfind("#ifdef __APPLE__")
    last_else = head.rfind("#else")
    last_endif = head.rfind("#endif")
    ck("/proc/ 出现在非 Apple 分支里",
       last_apple > last_endif and last_else > last_apple,
       "位置 %d 附近" % m.start())

# ===========================================================================
print("=== 6) 头文件分流 ===")
# 三条编译路径（MinGW 真 Windows / tests-stub 替身 harness / POSIX 移植）各拿
# 各的 Win32 定义。common.h 的 #else 分支曾经无条件 include wincompat.h，
# 结果 8 个既有 harness 全部链接失败（它们靠 -Itests/stub 截走 <windows.h>）。
# 注意 include 前是 "#    include"（预处理指令允许 # 与关键字之间有空白），
# 所以这里按正则匹配，别按字面串。
ck("common.h 用 __has_include 在替身与 wincompat 之间分流",
   re.search(r"__has_include\(<windows\.h>\)", COMMON) is not None and
   re.search(r"#\s*include\s+\"wincompat\.h\"", COMMON) is not None)
ck("NULL_HANDLE 定义在 #ifdef 之外（三条路径都要有）",
   COMMON.split("#endif")[-1].count("NULL_HANDLE") >= 1 or
   "#ifndef NULL_HANDLE" in COMMON)

# ===========================================================================
print("=== 7) POSIX 后端的关键语义 ===")
ck("plat_thread_join 会提前返回（不是无条件睡满超时）",
   "done" in PLAT_POSIX and "pthread_join" in PLAT_POSIX,
   "关 4 个窗格会白等 4×2000ms，退出一次 8 秒")
ck("close_pane 在 POSIX 上先杀进程再 join",
   re.search(r"#ifndef _WIN32.*?plat_proc_kill.*?#endif\s*\n\s*plat_thread_join",
             PANE, re.S) is not None,
   "读线程阻塞在 pty read 上，不先杀 shell 就永远等不到 EOF")
ck("termios 关掉了 OPOST（Windows 侧设了 DISABLE_NEWLINE_AUTO_RETURN）",
   "OPOST" in PLAT_POSIX)
ck("进入时打开 SGR 鼠标模式 1006",
   "1006h" in MAIN_POSIX and "1003h" in MAIN_POSIX)
ck("进入时关闭括号粘贴 2004", "?2004l" in MAIN_POSIX)
ck("退出时还原备用屏与光标",
   "?1049l" in MAIN_POSIX and "?25h" in MAIN_POSIX)
ck("不拦 SIGINT（Ctrl+C 必须原样转给 shell）",
   "SIGINT" not in MAIN_POSIX or "不拦" in MAIN_POSIX)
ck("plat_input_flush 存在（切模式后丢弃半截转义序列）",
   "void plat_input_flush(void)" in TERM_INPUT)

# ===========================================================================
# 8) 配置文件路径 / 剪贴板 / 宽字符搜索 —— 三个「编译期看不出、跑起来静默失效」
#    的真 bug。对应的运行时判据在 tests/posix_clip_config.py 与 verify_search.py，
#    这里钉住源码层面的不变量，防止有人把它们改回去。
print("=== 8) 配置路径 / 剪贴板 / 宽字符搜索 ===")

# --- 配置路径 -------------------------------------------------------------
ck("platform.h 声明 plat_user_home()", "plat_user_home(void)" in PLATFORM_H)
ck("platform.h 为两侧各定义 TERMUX_PATH_SEP",
   PLATFORM_H.count("#define TERMUX_PATH_SEP ") == 2 and
   PLATFORM_H.count("#define TERMUX_PATH_SEP_S ") == 2)
# Windows 侧必须是反斜杠（行为与移植前逐字一致），POSIX 侧必须是正斜杠。
_win_blk = PLATFORM_H.split("#ifdef _WIN32", 1)[1].split("#else", 1)[0]
_pos_blk = PLATFORM_H.split("#else", 1)[1].split("#endif", 1)[0]
ck("Windows 分支的分隔符是反斜杠", "L'\\\\'" in _win_blk)
ck("POSIX 分支的分隔符是正斜杠", "L'/'" in _pos_blk)
ck("platform_win.c 的 plat_user_home 用 USERPROFILE",
   "plat_user_home" in PLAT_WIN and "USERPROFILE" in PLAT_WIN)
ck("platform_posix.c 的 plat_user_home 用 HOME",
   "plat_user_home" in PLAT_POSIX and '"HOME"' in PLAT_POSIX)
ck("config.c 不再写死反斜杠拼 ini 路径",
   'L"%s\\' not in CONFIG and "wcsrchr(exe_path, L'\\')" not in CONFIG)
ck("config.c 不再直接读 USERPROFILE", "_wgetenv(L\"USERPROFILE\")" not in CONFIG)
ck("config.c 无条件 include platform.h（两侧都要那些宏）",
   "#include \"platform.h\"" in CONFIG.split("#ifndef _WIN32")[0])

# --- _snwprintf 的 %s 语义 ------------------------------------------------
# glibc/macOS 的 swprintf 把 %s 当窄字符，传 wchar_t* 会在第一个 0x00 字节截断
# （L"/tmp" 只剩 "/"），必须重写成 %ls。少了这段，ini 路径会变成 "//termux.ini"。
_snp = PLAT_POSIX.split("int _snwprintf(", 1)[1].split("\n}", 1)[0]
ck("_snwprintf 把裸 %s/%c 重写成 %ls/%lc",
   "L'l'" in _snp and "has_len" in _snp and "vswprintf" in _snp)
ck("_snwprintf 保留 %% 转义", 'f[i] == L\'%\'' in _snp)

# --- 剪贴板 ---------------------------------------------------------------
# 真正的复制入口是 input.c 的 OpenClipboard/GlobalAlloc/SetClipboardData，
# plat_clip_copy() 没有别的调用者。所以 SetClipboardData 必须真的把内容送出去。
_scd = PLAT_POSIX.split("void *SetClipboardData(", 1)[1].split("\n}", 1)[0]
ck("SetClipboardData 处理 CF_UNICODETEXT", "CF_UNICODETEXT" in _scd)
ck("SetClipboardData 真的调 plat_clip_copy", "plat_clip_copy(" in _scd)
ck("GlobalAlloc 真的分配内存（不是返回 NULL）", "calloc" in PLAT_POSIX.split("void *GlobalAlloc(", 1)[1].split("\n}", 1)[0])

# --- 宽字符搜索 -----------------------------------------------------------
# 宽字在缓冲里占两格（次格 UnicodeChar==0），逐物理列比较会让「中文」这类跨宽字
# 的多字词永远匹配不上。run_search 必须先把主格压成紧凑序列。
_rs = INPUT.split("static void run_search(int live)", 1)[1]
_rs = _rs[:_rs.find("\nvoid ")] if "\nvoid " in _rs else _rs
ck("run_search 用 is_wide_cp 识别宽字次格", "is_wide_cp(" in _rs)
ck("run_search 维护主格->物理列的映射", "row_x[" in _rs and "n_prim" in _rs)
ck("run_search 的高亮 end_x 会盖住宽字次格", "end_x++" in _rs)

# --- 关于页文案不能写死 Windows ------------------------------------------
# 同一页的「系统版本」已经正确显示 Linux/macOS，标题却写着「Windows 终端复用器 /
# 基于 Windows ConPTY」，自相矛盾。
ck("关于页标题走平台宏", "TERMUX_ABOUT_TITLE_U8" in PANE)
ck("关于页副标题走平台宏", "TERMUX_ABOUT_SUB_U8" in PANE)
ck("pane.c 不再写死 Windows 关于页文案",
   "Windows 终端复用器 (Terminal Multiplexer)" not in PANE and
   "基于 Windows ConPTY 的高性能" not in PANE)
_win_h = PLATFORM_H.split("#ifdef _WIN32", 1)[1].split("#else", 1)[0]
_pos_h = PLATFORM_H.split("#else", 1)[1].split("#endif", 1)[0]
ck("platform.h 的 Windows 分支保留原 Windows 文案",
   "Windows 终端复用器" in _win_h and "ConPTY" in _win_h)
ck("platform.h 的 POSIX 分支不再提 Windows/ConPTY",
   "Windows" not in _pos_h.split("TERMUX_ABOUT_TITLE_U8")[1].split("#define")[0] and
   "forkpty" in _pos_h)

# --- 命令行切分必须认引号 --------------------------------------------------
# 只按空格切会把 /bin/sh -c "echo hi; sleep 6" 撕成 6 段，子进程实际执行
# /bin/sh -c '"echo' —— 窗格一闪就没了。Windows 侧由子进程 CRT 解析引号，
# 所以这是个纯 POSIX 的行为差异。
ck("platform_posix.c 有 posix_split_cmdline", "int posix_split_cmdline(" in PLAT_POSIX)
_ck = PLAT_POSIX.split("int posix_split_cmdline(", 1)[1]
_ck = _ck[:_ck.find("\nint plat_proc_spawn")]
SQ, DQ, BS = chr(39), chr(34), chr(92)   # 避开嵌套引号地狱
ck("posix_split_cmdline 处理双引号", ("== " + SQ + DQ + SQ) in _ck)
ck("posix_split_cmdline 处理单引号", ("== " + SQ + BS + SQ + SQ) in _ck)
ck("posix_split_cmdline 处理反斜杠转义", ("== " + SQ + BS + BS + SQ) in _ck)
ck("plat_proc_spawn 调用 posix_split_cmdline（不再手撕空格）",
   "posix_split_cmdline(cmdbuf" in PLAT_POSIX)

# --- 启动目录必须展开环境变量 ----------------------------------------------
# Windows 分支用 ExpandEnvironmentStringsW；POSIX 分支原先直接把 %USERPROFILE%
# 原样交给 chdir，而设置页的提示明写着「支持 %USERPROFILE%」——功能在这边是死的。
# 更糟的是 chdir 失败被静默吞掉：注释写「目录不存在就退回 HOME」，代码里是空语句。
ck("platform_posix.c 有 posix_expand_env", "void posix_expand_env(" in PLAT_POSIX)
ck("posix_expand_env 认 %USERPROFILE%", "USERPROFILE" in PLAT_POSIX)
ck("posix_expand_env 认开头的 ~", "'~'" in PLAT_POSIX)
ck("pane.c 的 POSIX 分支调用了 posix_expand_env", "posix_expand_env(raw_dir" in PANE)
_sp = PLAT_POSIX.split("int plat_proc_spawn(", 1)[1]
_sp = _sp[:_sp.find("/* ---- 父进程 ----")]
ck("chdir 失败不再被静默吞掉（有 fprintf 报告）", "fprintf(stderr" in _sp)
ck("chdir 失败真的会退回 HOME", "chdir(h)" in _sp or "chdir(env_home())" in _sp)
ck("chdir 失败不再是空语句",
   "{ /* 目录不存在就退回 HOME */ }" not in PLAT_POSIX)

# ===========================================================================
# 自证：故意把一条断言的条件取反，必须失败。
print("=== 9) 自证 ===")
saved = len(FAILS)
ck("(自证) 故意错的断言", "这条字符串不可能出现在任何源文件里" in WINCOMPAT)
if len(FAILS) == saved + 1:
    print("  [ok]   自证有效（错的断言被抓到了）")
    del FAILS[-1]
    # 注意不要在这里动 NPASS：自证那条 ck 是【故意失败】的，本来就没自增过；
    # 而「自证有效」这行是手写 print，也没经过 ck。
else:
    print("  [FAIL] 自证无效，ck() 是死的")

print()
if FAILS:
    print("%d 项失败：%s" % (len(FAILS), "；".join(FAILS)))
    sys.exit(1)
print("移植不变量检查全部通过（%d 条断言）" % NPASS)
