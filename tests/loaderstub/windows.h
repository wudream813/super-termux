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

HRESULT CreatePseudoConsole(COORD size, HANDLE in, HANDLE out, DWORD flags, HPCON *ph);
HRESULT ResizePseudoConsole(HPCON h, COORD size);
void ClosePseudoConsole(HPCON h);

HMODULE LoadLibraryW(const WCHAR *name);
BOOL FreeLibrary(HMODULE h);
void *GetProcAddress(HMODULE h, const char *name);
DWORD GetModuleFileNameW(void *module, WCHAR *buf, DWORD cap);
HMODULE GetModuleHandleW(const WCHAR *name);

#endif
