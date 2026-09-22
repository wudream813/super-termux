#ifndef WIN_TERMUX_COMMON_H
#define WIN_TERMUX_COMMON_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006   // Win10 1809 (RS5) - ConPTY requirement
#endif
#define UNICODE
#define _UNICODE

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#else
/* 非 Windows（Linux / macOS）。这里有三条编译路径，靠「找不找得到 windows.h」
 * 来分流：
 *   1. 单元测试 / 回归 harness：用 -Itests/stub 或 -Itests/loaderstub 提供一份
 *      最小 windows.h 替身（还带 g_stub_* 那套假控制台状态）。这些 harness 在
 *      移植之前就存在，必须继续原样编过 —— 而它们本来就是靠
 *      「#include <windows.h> 被 -I 目录截走」生效的，所以优先走这条。
 *   2. 真正的移植构建（make linux / make darwin）：只有 -Iinclude，找不到
 *      windows.h，落到移植兼容层 wincompat.h。
 * 一份引擎源码，三种 Win32 定义各拿各的，互不干扰。 */
#if defined(__has_include)
#  if __has_include(<windows.h>)
#    include <windows.h>
#  else
#    include "wincompat.h"
#  endif
#else
#  include "wincompat.h"
#endif
#endif

/* 空句柄常量：三条路径都要有。pane.c / platform_*.c 用它判空，不用裸 NULL ——
 * POSIX 侧 HANDLE 是 intptr_t，跟指针常量比会报符号比较警告。 */
#ifndef NULL_HANDLE
#define NULL_HANDLE ((HANDLE)0)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* 定义在 src/globals.c。TERMUX_DUMP 下的诊断打点（见那里的注释）。
 * 声明放在 common.h 是因为 main.c / input.c / globals.c 都 include 它。 */
extern int g_dump_enabled;
void dump_mark(const char *fmt, ...);
#include <wctype.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#ifndef TERMUX_VERSION
/* v2.0.0：正式支持 Windows / Linux / macOS 三个系统。
 * 1.x 是 Windows-only（ConPTY 后端）；2.0 起引擎代码三系统共用，只有平台层分家：
 *   Windows : main.c + platform_win.c + conpty_loader.c     (ConPTY + CreateProcessW)
 *   POSIX   : main_posix.c + platform_posix.c + term_input_posix.c   (forkpty)
 * 接口见 include/platform.h。 */
#define TERMUX_VERSION "2.0.4"

/* 平台副标题。帮助页 / 关于页那几行共用 UI 代码里要用，所以跟 TERMUX_VERSION
 * 放一起（render.c 只 include 了 common.h，没有 platform.h）。
 * ★ 原来在共用的 render.c 里写死 "Windows Terminal Multiplexer (Win10 1809+)"，
 *   Linux/macOS 上照样这么显示 —— 和之前修掉的关于页文案是同一类问题。 */
#ifdef _WIN32
#define TERMUX_HELP_PLATFORM_U8  "Windows Terminal Multiplexer (Win10 1809+)"
#elif defined(__APPLE__)
#define TERMUX_HELP_PLATFORM_U8  "macOS Terminal Multiplexer (forkpty)"
#else
#define TERMUX_HELP_PLATFORM_U8  "Linux Terminal Multiplexer (forkpty)"
#endif
#endif

#define MAX_PANES         16
#define SCROLL_BUF_LINES  10000
#define READ_BUF_SIZE     32768
#define MAX_CHOOSER_ITEMS 9
#define MAX_SEARCH_MATCHES 2048
#define PALETTE_STACK_MAX 8

#define RGB565_WHITE 0xFFFF
#define RGB565_BLACK 0x0000

static inline WORD rgb565(int r, int g, int b) {
    return (WORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void rgb565_split(WORD v, int *r, int *g, int *b) {
    *r = (v >> 11) & 0x1F; *r = (*r << 3) | (*r >> 2);
    *g = (v >> 5) & 0x3F;  *g = (*g << 2) | (*g >> 4);
    *b = v & 0x1F;         *b = (*b << 3) | (*b >> 2);
}

#endif // WIN_TERMUX_COMMON_H
