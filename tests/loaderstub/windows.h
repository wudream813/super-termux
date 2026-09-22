/* tests/loaderstub/windows.h —— 只给 src/conpty_loader.c 用的最小 Win32 替身。
 *
 * 目的：让 conpty_loader_init() 的【决策逻辑】能在 Linux 上真跑一遍。
 * 被替身的只有 4 个 Win32 调用（LoadLibraryW / FreeLibrary / GetProcAddress /
 * GetModuleFileNameW），其余全是加载器自己的代码。
 *
 * include/common.h 用尖括号引 <windows.h>/<shellapi.h>/<process.h>，
 * 所以 -Itests/loaderstub 排前面就能遮蔽它们（引号形式的 "common.h"
 * 会先在同目录找，遮蔽不了，故替身必须叫 windows.h）。 */
#ifndef LOADERSTUB_WINDOWS_H
#define LOADERSTUB_WINDOWS_H

#include <wchar.h>
#include <stddef.h>

typedef int HRESULT;
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *HPCON;
typedef wchar_t WCHAR;

typedef struct {
    short X;
    short Y;
} COORD;

#define WINAPI
#define MAX_PATH 260
#define ERROR_PROC_NOT_FOUND 127
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x))

/* 测试钩子：由 test_conpty_loader.c 实现，控制替身的行为。 */
extern int stub_dll_present;      /* exe 同目录/PATH 上有没有 conpty.dll */
extern int stub_symbols_complete; /* dll 里三个符号是否齐全 */
extern int stub_kernel32_ok;      /* kernel32 能不能取到符号 */
extern int stub_free_called;      /* FreeLibrary 被调过几次 */

/* ★★★ 这 8 个名字必须【重映射】，否则在真 Windows 上链接必炸。
 *
 * 本文件是给 src/conpty_loader.c 用的 Win32 替身，tests/test_conpty_loader.c
 * 里定义了同名函数来拦截调用、验证 loader 的决策逻辑。Linux 上没有
 * libkernel32，所以一直没事。但在真 Windows / MSYS2 上，libkernel32.a 里
 * 这 8 个符号【全都已经有定义】（实测 nm 每个都是 1 个 T）：
 *     LoadLibraryW FreeLibrary GetProcAddress GetModuleFileNameW
 *     GetModuleHandleW CreatePseudoConsole ResizePseudoConsole ClosePseudoConsole
 * 于是链接期直接：
 *     multiple definition of `GetProcAddress';
 *     libkernel32.a(libkernel32s00735.o): first defined here
 * （CI 的 windows 作业第三轮就是挂在这里，2026-09-22。）
 *
 * 重映射之后：
 *   - src/conpty_loader.c 里写的仍然是 GetProcAddress(...)，一个字都不用改；
 *   - tests/test_conpty_loader.c 定义的是映射后的名字，不和 kernel32 撞；
 *   - 正式构建（make all）不带 -Itests/loaderstub，走真 <windows.h>，
 *     调的是真 kernel32，完全不受影响。
 * ★ 本地交叉编译器【复现不了】这个链接错 —— Debian 的 mingw-w64 默认库集合
 *   和 MSYS2 不同，不会那么早把 libkernel32.a 的目标文件拉进来。所以
 *   make crosscheck-win-harness 用 tools/check_win32_symbol_clash.py 做
 *   【符号级】比对，不依赖链接行为。那个检查验红过。 */
#define LoadLibraryW         termux_stub_LoadLibraryW
#define FreeLibrary          termux_stub_FreeLibrary
#define GetProcAddress       termux_stub_GetProcAddress
#define GetModuleFileNameW   termux_stub_GetModuleFileNameW
#define GetModuleHandleW     termux_stub_GetModuleHandleW
#define CreatePseudoConsole  termux_stub_CreatePseudoConsole
#define ResizePseudoConsole  termux_stub_ResizePseudoConsole
#define ClosePseudoConsole   termux_stub_ClosePseudoConsole

HRESULT CreatePseudoConsole(COORD size, HANDLE in, HANDLE out, DWORD flags, HPCON *ph);
HRESULT ResizePseudoConsole(HPCON h, COORD size);
void ClosePseudoConsole(HPCON h);

HMODULE LoadLibraryW(const WCHAR *name);
BOOL FreeLibrary(HMODULE h);
void *GetProcAddress(HMODULE h, const char *name);
DWORD GetModuleFileNameW(void *module, WCHAR *buf, DWORD cap);
HMODULE GetModuleHandleW(const WCHAR *name);

#endif
