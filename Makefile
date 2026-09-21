CC = x86_64-w64-mingw32-gcc
CXX = x86_64-w64-mingw32-g++
CFLAGS = -O2 -s -Wall -Wextra -Iinclude
CXXFLAGS = -O2 -s -Wall -Wextra -Iinclude
LDFLAGS = -luser32 -lshell32

SRC = src/config.c src/cliphtml.c src/conpty_loader.c src/framediff.c src/input.c src/keymap.c src/loghist.c src/main.c src/globals.c src/pane.c src/platform_win.c src/render.c src/screen.c src/split.c src/theme.c src/utf8.c src/vt.c
TARGET = termux.exe
TARGET_CPP = termux_cpp.exe

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

# ---- 2026-09-19 §19 两个对照实现 ----
# Plan A：捆绑 microsoft/terminal 的 conpty.dll + OpenConsole.exe（找不到才回退
#         kernel32）。目的是让 conhost 不再在每次 resize 时整屏重发。
planA:
	$(CC) $(CFLAGS) -DTERMUX_CONPTY_DEFAULT_DLL $(SRC) -o termux_v17A.exe $(LDFLAGS)

# Plan B：仍用 kernel32（= 改动前），但关掉「重绘后把提示符顶回底行」的底部重锚定，
#         改成 Windows Terminal 的立场：让 conhost 的重绘照实落地。
planB:
	$(CC) $(CFLAGS) -DTERMUX_NO_REANCHOR $(SRC) -o termux_v17B.exe $(LDFLAGS)

plans: planA planB

# ---- 2026-09-20 v19：修「左窗格历史被截断」----
# conhost 的 resize 整屏重绘在真机上是【两趟】的（uploads/render_dump.log +
# termux_dump.log pane0 偏移 5884 / 6076）：
#   趟 1: ESC[?25l ESC[H <几行内容> ESC[<rows>;1H ESC[?25h ESC[?25l
#   趟 2: ESC[H <同样几行 + ESC[K> <一串 ESC[K CR LF 空行> ESC[<r>;<c>H ESC[?25h
# 旧实现在趟 1 就把 resize_repaint_pending 和快照一起吃掉（那趟光标停在最后一行、
# tail==0、本来就无事可做），趟 2 于是拿不到「重绘前的可见行」，只能眼睁睁看着它
# 补的空行把 24 行 dir 列表抹平。现在趟 1 保留 pending + 快照（封顶只让过一趟），
# 趟 2 才做底部重锚定；另外重绘顶行若只是本地完整记录的后半截（conhost 的 reflow
# 不跨它自己 scrollback/viewport 边界合并），保留本地那行、不删前缀。
# 与 termux.exe 同一份源码、同一个默认（重锚定开）；TERMUX_REANCHOR=0 可切回
# 旧的 Plan B 行为做对照。
v19:
	$(CC) $(CFLAGS) $(SRC) -o termux_v19.exe $(LDFLAGS)

# ---- 2026-09-20 v20：修「窗格 <=6 列时滚动条没了」（= v19 + 滚动条门槛）----
# 渲染侧原来写死 cols >= 10（分屏窗格路径 render_split_pane、整屏路径 render_screen
# 各一处），窗格窄于 10 列时整条滚动条不画；而 src/input.c 的鼠标命中测试没有同样
# 的门槛 —— 窄窗格里能拖一条看不见的滚动条。现在收口成 render_sb_cols_ok()
# （SB_MIN_COLS = 2），渲染两点 + 命中一点共用；判据见 verify_scrollbar.py。
v20:
	$(CC) $(CFLAGS) $(SRC) -o termux_v20.exe $(LDFLAGS)

# ---- 2026-09-20 v21：三条一起 ----
#  (23) 滚动条盖住刚敲的字：光标停在右缘列时（VT 延迟换行）那一格让给内容，
#       见 render_sb_spare_row / verify_scrollbar.py 第 1b 节；
#  (24) 搜索跟随新输出重算：pane 读线程 search_mark_dirty()，主循环渲染前
#       search_refresh_live()，并用 search_relocate_cur() 把停留项找回来，
#       见 verify_search_refresh.py；
#  (25) 太矮不该能上下切分：SPLIT_MIN_ROWS 2 -> 3（门槛 5 行 -> 7 行），
#       见 verify_split.py 的 bug #25 段。
v21:
	$(CC) $(CFLAGS) $(SRC) -o termux_v21.exe $(LDFLAGS)

# ---- 2026-09-20 v22：中文输入法下「Ctrl+B 然后 _」没反应 ----
# 输入法把 Shift+标点 变成 VK_PACKET + 全角字符，只按虚拟键码匹配的绑定抓不到。
# 修法：spec_match 统一把全角 ASCII(U+FF01..U+FF5E) 折成半角，并给 _ 补一条纯字符
# 绑定（CHR() 不看 vk、不要求 shift）。判据在 tests/test_config.c（make unittest）。
v22:
	$(CC) $(CFLAGS) $(SRC) -o termux_v22.exe $(LDFLAGS)

# ---- 2026-09-20 v23：v22 的全部 + 嵌套分屏下拖分隔条动错条（bug #27）----
# 「先左右分 -> 左半上下分三格 -> 中间格再左右分」之后拖中间格的右侧分隔线，动的
# 却是里层那条，而且跳一下。两个病因：(1) split_resize_set_frac() 取锚点向上第一个
# 方向匹配祖先，嵌套下那是【里层】节点；(2) 百分比分母用「锚点宽+1+屏幕右邻宽」，
# 与 frac 的实际解释（被改节点的整棵子树跨度）不一致。
# 修法：split_drag_pick_node() 按「a/b 子树分界线 == 用户抓的那条线」挑节点，
# split_drag_pct() 按该节点自己的跨度算百分比。判据在 verify_split.py（含两个自证）。
v23:
	$(CC) $(CFLAGS) $(SRC) -o termux_v23.exe $(LDFLAGS)

# ---- 2026-09-20 v24：v23 的全部 + 滚动条拖动的鼠标捕获(#28) + 最小尺寸闸(#29) ----
# #28 滚动条拖动只有 g_sb_dragging/g_sb_grab_offset，没记是哪个 pane；拖动中鼠标
#     划过另一个窗格 -> handle_split_mouse 切了焦点 -> 后续 move 去拖那个窗格的条。
#     修法：拖动期间分屏层不切焦点（鼠标捕获）+ 新增 g_sb_drag_pane。
# #29 frac 只夹 5..95，窗格小的时候 5% 就是 0 行/列。修法：split_drag_pct 按最小
#     尺寸夹百分比 + layout_rec 里 clamp_side() 兜底（覆盖键盘 resize / 窗口拖窄）。
# 判据：tests/verify_sb_drag.sh（新，含自证）+ verify_split.py（+24 条，含自证 3/4）。
v24:
	$(CC) $(CFLAGS) $(SRC) -o termux_v24.exe $(LDFLAGS)

# src/conpty_loader.c 的决策逻辑单元测试（Linux 上跑，替身见 tests/loaderstub）
verify-loader:
	sh tests/verify_conpty_loader.sh

cpp: $(TARGET_CPP)

$(TARGET_CPP): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET_CPP) $(LDFLAGS)

test:
	python3 verify_all.py

# 主题 / 键位模块不依赖 Win32 API，可用 tests/stub 的 windows.h 替身在本机跑
unittest:
	gcc -O1 -Wall -Wextra -Werror -Itests/stub -Iinclude src/theme.c src/keymap.c tests/test_config.c -o /tmp/termux_test_config -lm
	/tmp/termux_test_config

# ===========================================================================
# POSIX（Linux / macOS）构建。引擎代码与 Windows 完全共用，只有 main / 进程 /
# 控制台 / 剪贴板换成 POSIX 后端：
#   src/main.c + src/platform_win.c + src/conpty_loader.c   (Windows)
#   src/main_posix.c + src/platform_posix.c + src/term_input_posix.c   (POSIX)
# ===========================================================================
POSIX_SRC = src/config.c src/cliphtml.c src/framediff.c src/globals.c src/input.c \
            src/keymap.c src/loghist.c src/main_posix.c src/pane.c src/platform_posix.c \
            src/render.c src/screen.c src/split.c src/term_input_posix.c src/theme.c \
            src/utf8.c src/vt.c
# 注意：$(CC) 是 MinGW 交叉编译器（给 Windows 版用的）。POSIX 构建必须用本机
# 编译器，否则会拿着 mingw 的头去找 poll.h / termios.h。用 ?= 以便 CI 里覆盖。
POSIX_CC     ?= cc
POSIX_CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Iinclude
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  POSIX_LDFLAGS  = -lpthread
  POSIX_TARGET   = termux-macos
  # forkpty 在 macOS 的 libSystem 里，没有 -lutil 这个库；链接它直接报错。
  POSIX_TEST_LIBS = -lpthread
else
  POSIX_LDFLAGS  = -lpthread -lutil
  POSIX_TARGET   = termux-linux
  POSIX_TEST_LIBS = -lpthread -lutil
endif

# posix-build：按当前系统建出对应的 POSIX 二进制。CI 的 macOS 作业不能走 linux
# 目标 —— 它把 -lutil 写死了，Apple 上没有这个库。
ifeq ($(UNAME_S),Darwin)
posix-build: darwin
else
posix-build: linux
endif

linux:
	$(POSIX_CC) $(POSIX_CFLAGS) $(POSIX_SRC) -o termux-linux -lpthread -lutil

# darwin 必须在 macOS 上跑：它需要 Apple 的 SDK（forkpty 在 libSystem 里，
# 没有 -lutil）。链接标志写死，不跟 $(POSIX_LDFLAGS) 走，免得在 Linux 上
# 误用 -lutil 编出一个链接不过的东西还以为验证过了。
# ★ 必须在 macOS 上跑，这里主动拦一道。不拦的话在 Linux 上它会【成功】编出
#   一个 ELF Linux 二进制、只是名字叫 termux-macos —— 看起来像 macOS 构建通过了，
#   实际上一次都没经过 Apple 的编译器。这种假绿比编不过危险得多。
darwin:
	@case "$$(uname -s)" in \
	  Darwin) ;; \
	  *) echo "darwin 目标只能在 macOS 上跑：它需要 Apple SDK（forkpty 在 libSystem、" >&2; \
	     echo "_NSGetExecutablePath 在 <mach-o/dyld.h>）。当前系统是 $$(uname -s)，" >&2; \
	     echo "继续只会产出一个改名叫 termux-macos 的 Linux 二进制。" >&2; exit 1 ;; \
	esac
	$(POSIX_CC) $(POSIX_CFLAGS) $(POSIX_SRC) -o termux-macos -lpthread

posix: linux

# ---- POSIX 侧的两个测试 ----
# unittest-posix-input：把终端字节流喂给 plat_console_read()，逐条核对翻译出来的
#   INPUT_RECORD（键位 / 修饰键 / SGR 鼠标 / 滚轮 delta / UTF-8 / OSC 吞掉）。
# smoke-posix：真起一个 pty 把 ./termux-linux 跑起来，按剧本操作，用
#   tools/frame2txt.py 解码渲染帧断言界面。移植期两个编译期看不出的 bug
#   （CRITICAL_SECTION 不可重入导致分屏自死锁、默认 shell 写死 cmd.exe）
#   都是它抓出来的。
unittest-posix-input:
	$(POSIX_CC) -O1 -Wall -Iinclude tests/test_term_input_posix.c src/term_input_posix.c -o /tmp/termux_tti
	/tmp/termux_tti

smoke-posix: posix-build
	TERMUX_SMOKE_EXE=$(CURDIR)/$(POSIX_TARGET) python3 tests/posix_smoke.py

# verify-port：移植不变量静态检查（62 条）。把移植期真的踩过、编译期又看不出
# 来的坑钉成断言：CRITICAL_SECTION 可重入、默认 shell 不写死 cmd.exe、
# linux 目标不用 MinGW 的 $(CC)、macOS 用 <mach-o/dyld.h>、plat_* 两侧都实现…
verify-port:
	python3 tests/verify_port.py

# clip-posix：真 pty 验证两条「编译期看不出、跑起来静默失效」的路径 ——
#   ① 复制模式必须真的发出 OSC 52（复制入口是 input.c 的 SetClipboardData，
#      plat_clip_copy 没有别的调用者，替身一旦是空实现就什么都不发生且不报错）；
#   ② termux.ini 必须落在二进制同目录，而不是启动时的 cwd（分隔符 + glibc
#      swprintf 把 %s 当窄字符，两个坑叠起来会让路径变成 "//termux.ini"）。
clip-posix: posix-build
	TERMUX_SMOKE_EXE=$(CURDIR)/$(POSIX_TARGET) python3 tests/posix_clip_config.py

# unittest-posix-write：plat_write_fd 的背压回归。pty master 是 O_NONBLOCK，子进程
#   一时不读时 write 返回 EAGAIN；旧版直接 return -1，那段输入静默丢掉（实测丢 41%）。
unittest-posix-write:
	$(POSIX_CC) -O1 -Wall -Iinclude tests/test_write_backpressure.c src/platform_posix.c \
	   src/term_input_posix.c -o /tmp/termux_wb $(POSIX_TEST_LIBS)
	/tmp/termux_wb

# unittest-posix-cmdline：posix_split_cmdline 的引号解析回归。旧实现只按空格切，
#   /bin/sh -c "echo hi; sleep 6" 会被撕成 6 段，子进程实际跑 /bin/sh -c '"echo'
#   —— 窗格一闪就没了。Windows 侧 CreateProcessW 交整条串给子进程 CRT，那边一直好的。
unittest-posix-cmdline:
	$(POSIX_CC) -O1 -Wall -Iinclude tests/test_split_cmdline.c src/platform_posix.c \
	   src/term_input_posix.c -o /tmp/termux_sc $(POSIX_TEST_LIBS)
	/tmp/termux_sc

# unittest-posix-env：posix_expand_env 的回归。Windows 侧用 ExpandEnvironmentStringsW，
#   POSIX 分支原先什么都不做，设置页承诺的「支持 %USERPROFILE%」在 Linux/macOS 上是死的；
#   而且 chdir 失败被静默吞掉（注释写「退回 HOME」，代码里是空语句）。
unittest-posix-env:
	$(POSIX_CC) -O1 -Wall -Iinclude tests/test_expand_env.c src/platform_posix.c \
	   src/term_input_posix.c -o /tmp/termux_ee $(POSIX_TEST_LIBS)
	/tmp/termux_ee

# POSIX 侧一把梭：编译 + 不变量检查 + 三个 POSIX 单测 + 真 pty 冒烟 + 剪贴板/配置
check-posix: posix-build verify-port unittest-posix-input unittest-posix-write unittest-posix-cmdline unittest-posix-env smoke-posix clip-posix
	@echo "POSIX 检查全部通过"


clean:
	rm -f $(TARGET) $(TARGET_CPP) termux-linux termux-macos *.o

.PHONY: all cpp test unittest posix-build clean planA planB plans v19 v20 v21 v22 v23 v24 linux darwin posix verify-loader unittest-posix-input unittest-posix-write unittest-posix-cmdline unittest-posix-env smoke-posix clip-posix check-posix verify-port
