#ifndef TERMUX_CONPTY_LOADER_H
#define TERMUX_CONPTY_LOADER_H

#include "common.h"   /* 提供 _WIN32_WINNT=0x0A00 + windows.h，HPCON 需要它 */

/* ConPTY 实现选择与动态加载。
 *
 * 背景见 analysis/拖动错乱-根因与三种可选行为.md §19：
 * 系统自带（kernel32 转发到 conhost.exe）的 ConPTY 在每次 resize 时会把整个
 * 缓冲区重发一遍（字节流里表现为 ESC[H 起的全屏重绘），那次重绘会覆盖掉本地
 * 滚动缓冲里的行 —— microsoft/terminal PR #4354 的原文正是这么描述的。
 * Windows Terminal / WezTerm / wtmux 都随包携带 microsoft/terminal 的
 * conpty.dll + OpenConsole.exe 来避开这套行为。
 *
 * 加载顺序（受 TERMUX_CONPTY 控制）：
 *   dll    只用 exe 同目录（其次 PATH）的 conpty.dll，找不到就失败
 *   system 只用 kernel32（= 改动前的行为）
 *   auto   先试 conpty.dll，失败回退 kernel32      <- 默认
 *
 * 另有 TERMUX_CONPTY_FLAGS=<整数>（支持 0x 前缀）覆盖 CreatePseudoConsole 的
 * dwFlags，便于在同一个二进制上试 0x1 / 0x2 / 0x08 等取值。
 * 官方 conpty.h 只定义了 PSEUDOCONSOLE_INHERIT_CURSOR(0x1) 与字宽掩码
 * 0x18（GRAPHEMES 0x08 / WCSWIDTH 0x10 / CONSOLE 0x18），默认 0。
 */

int conpty_loader_init(void);   /* 幂等，返回 1 表示可用 */

HRESULT conpty_create(COORD size, HANDLE hInput, HANDLE hOutput,
                      DWORD dwFlags, HPCON *phPC);
HRESULT conpty_resize(HPCON hPC, COORD size);
void    conpty_close(HPCON hPC);

DWORD conpty_default_flags(void);
const char *conpty_source_name(void);   /* 诊断用："conpty.dll" / "kernel32" */
int conpty_is_bundled(void);

#endif
