/* ---------------------------------------------------------------------------
 * wincompat.h —— POSIX（Linux / macOS）上的 Win32 类型与少量 API 兼容层。
 *
 * 为什么需要它：整个引擎（screen / vt / render / input / split / keymap / theme /
 * utf8 / framediff / config / cliphtml / loghist，1.3 万行）是按 Windows 控制台的
 * 数据结构写的 —— INPUT_RECORD / KEY_EVENT_RECORD / MOUSE_EVENT_RECORD / CHAR_INFO
 * / COORD / VK_* 虚拟键码 / CRITICAL_SECTION。这一层让这些文件在 POSIX 上【原样】
 * 编译，不用改一行；真正平台相关的东西（进程、控制台 I/O、剪贴板）在
 * include/platform.h + src/platform_posix.c 里。
 *
 * 与 tests/stub/windows.h 的关系：那份是单元测试用的最小替身（还带 g_stub_* 那套
 * 假的控制台状态），故意保持独立、不动它，免得牵动 8 个既有 harness。两边重叠的
 * 是 VK_* 与鼠标/修饰键位定义 —— 由 verify_wincompat.py 逐条比对，防止漂移。
 * ------------------------------------------------------------------------- */
#ifndef WIN_TERMUX_WINCOMPAT_H
#define WIN_TERMUX_WINCOMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

/* ---- 基础类型 ---- */
typedef unsigned short WORD;
typedef unsigned int   DWORD;
typedef unsigned long long DWORD64;
typedef unsigned long long ULONGLONG;
typedef unsigned char  BYTE;
typedef int            BOOL;
typedef wchar_t        WCHAR;
typedef short          SHORT;
typedef unsigned int   UINT;
typedef int            LONG;
typedef long           LONGLONG;
typedef unsigned int   LCID;

/* 句柄统一成 intptr_t：POSIX 侧往里塞 fd / pid / pthread_t（编码见 platform.h）。
 * 这样 types.h 里 Pane 的 `HANDLE pipe_in, pipe_out, process, thread, read_thread`
 * 一个字都不用改。 */
typedef intptr_t HANDLE;
typedef void    *HPCON;
typedef void    *HGLOBAL;
typedef void    *HWND;
typedef void    *HINSTANCE;
typedef void    *HKEY__;
typedef HKEY__  *HKEY;

#define MAX_PATH 4096
#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define NULL_HANDLE ((HANDLE)0)
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT  258
#define INFINITE      0xFFFFFFFFu

#define CP_UTF8 65001
#define CP_ACP  0

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp  wcscasecmp
#define _wcsnicmp wcsncasecmp

#ifndef __stdcall
#define __stdcall
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

/* ---- 原子 / 时钟 / 睡眠 ---- */
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ull + (ULONGLONG)ts.tv_nsec / 1000000ull;
}
static inline void Sleep(DWORD ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static inline LONG InterlockedExchange(volatile LONG *dst, LONG v) {
    return __atomic_exchange_n(dst, v, __ATOMIC_SEQ_CST);
}

/* ---- 临界区：真的用 pthread mutex（POSIX 侧有 pane 读线程）。
 *      ★ 必须是【可重入】的：Windows 的 CRITICAL_SECTION 同一线程可以重复
 *      Enter，而引擎里真的这么用 —— render_screen() 先拿 g_mux.cs，
 *      再经 render_split -> split_compute_rects -> pane_resize_to 又拿一次。
 *      用普通 pthread_mutex 会在这里自死锁（实测：一分屏整个渲染线程卡死，
 *      宿主侧一个字节都不再输出）。 ---- */
typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->m, &attr);
    pthread_mutexattr_destroy(&attr);
}
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_destroy(&c->m); }
static inline void EnterCriticalSection(CRITICAL_SECTION *c)   { pthread_mutex_lock(&c->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c)   { pthread_mutex_unlock(&c->m); }

/* ---- 控制台几何 / 字符单元 ---- */
typedef struct _COORD { short X; short Y; } COORD;
typedef struct _SMALL_RECT { short Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct _CHAR_INFO {
    union { WCHAR UnicodeChar; char AsciiChar; } Char;
    WORD Attributes;
} CHAR_INFO;

/* ---- 颜色属性位（render.c 用来算 SGR） ---- */
#define FOREGROUND_BLUE      0x0001
#define FOREGROUND_GREEN     0x0002
#define FOREGROUND_RED       0x0004
#define FOREGROUND_INTENSITY 0x0008
#define BACKGROUND_BLUE      0x0010
#define BACKGROUND_GREEN     0x0020
#define BACKGROUND_RED       0x0040
#define BACKGROUND_INTENSITY 0x0080
#define COMMON_LVB_REVERSE_VIDEO 0x4000
#define COMMON_LVB_UNDERSCORE    0x8000

/* ---- 修饰键状态位 ---- */
#define RIGHT_ALT_PRESSED   0x0001
#define LEFT_ALT_PRESSED    0x0002
#define RIGHT_CTRL_PRESSED  0x0004
#define LEFT_CTRL_PRESSED   0x0008
#define SHIFT_PRESSED       0x0010
#define NUMLOCK_ON          0x0020
#define SCROLLLOCK_ON       0x0040
#define CAPSLOCK_ON         0x0080
#define ENHANCED_KEY        0x0100

/* ---- 鼠标事件 ---- */
#define FROM_LEFT_1ST_BUTTON_PRESSED 0x0001
#define RIGHTMOST_BUTTON_PRESSED     0x0002
#define FROM_LEFT_2ND_BUTTON_PRESSED 0x0004
#define MOUSE_MOVED   0x0001
#define DOUBLE_CLICK  0x0002
#define MOUSE_WHEELED 0x0004
#define MOUSE_HWHEELED 0x0008

typedef struct _MOUSE_EVENT_RECORD {
    COORD dwMousePosition;
    DWORD dwButtonState;
    DWORD dwControlKeyState;
    DWORD dwEventFlags;
} MOUSE_EVENT_RECORD;

/* ---- 键盘事件 ---- */
typedef struct _KEY_EVENT_RECORD {
    BOOL  bKeyDown;
    WORD  wRepeatCount;
    WORD  wVirtualKeyCode;
    WORD  wVirtualScanCode;
    union { WCHAR UnicodeChar; char AsciiChar; } uChar;
    DWORD dwControlKeyState;
} KEY_EVENT_RECORD;

#define KEY_EVENT                1
#define MOUSE_EVENT              2
#define WINDOW_BUFFER_SIZE_EVENT 4
#define FOCUS_EVENT              16

typedef struct _INPUT_RECORD {
    WORD EventType;
    union {
        KEY_EVENT_RECORD    KeyEvent;
        MOUSE_EVENT_RECORD  MouseEvent;
        struct { COORD dwSize; } WindowBufferSizeEvent;
    } Event;
} INPUT_RECORD;

/* ---- 控制台模式位（POSIX 侧只用来记状态，实际由 termios 负责） ---- */
#define ENABLE_PROCESSED_INPUT        0x0001
#define ENABLE_LINE_INPUT             0x0002
#define ENABLE_ECHO_INPUT             0x0004
#define ENABLE_WINDOW_INPUT           0x0008
#define ENABLE_MOUSE_INPUT            0x0010
#define ENABLE_INSERT_MODE            0x0020
#define ENABLE_QUICK_EDIT_MODE        0x0040
#define ENABLE_EXTENDED_FLAGS         0x0080
#define ENABLE_PROCESSED_OUTPUT       0x0001
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define DISABLE_NEWLINE_AUTO_RETURN   0x0008
#define STD_INPUT_HANDLE  (-10)
#define STD_OUTPUT_HANDLE (-11)
#define STD_ERROR_HANDLE  (-12)

/* ---- 虚拟键码（keymap.c 用到的全套） ---- */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12
#define VK_PAUSE   0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE  0x1B
#define VK_SPACE   0x20
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_PRINT   0x2A
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E
#define VK_LWIN    0x5B
#define VK_RWIN    0x5C
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD     0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE  0x6F
#define VK_F1      0x70
#define VK_F2      0x71
#define VK_F3      0x72
#define VK_F4      0x73
#define VK_F5      0x74
#define VK_F6      0x75
#define VK_F7      0x76
#define VK_F8      0x77
#define VK_F9      0x78
#define VK_F10     0x79
#define VK_F11     0x7A
#define VK_F12     0x7B
#define VK_F13     0x7C
#define VK_F14     0x7D
#define VK_F15     0x7E
#define VK_F16     0x7F
#define VK_F17     0x80
#define VK_F18     0x81
#define VK_F19     0x82
#define VK_F20     0x83
#define VK_F21     0x84
#define VK_F22     0x85
#define VK_F23     0x86
#define VK_F24     0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL  0x91
#define VK_OEM_1   0xBA   /* ; : */
#define VK_OEM_PLUS 0xBB  /* = + */
#define VK_OEM_COMMA 0xBC /* , < */
#define VK_OEM_MINUS 0xBD /* - _ */
#define VK_OEM_PERIOD 0xBE/* . > */
#define VK_OEM_2   0xBF   /* / ? */
#define VK_OEM_3   0xC0   /* ` ~ */
#define VK_OEM_4   0xDB   /* [ { */
#define VK_OEM_5   0xDC   /* \ | */
#define VK_OEM_6   0xDD   /* ] } */
#define VK_OEM_7   0xDE   /* ' " */
#define VK_PACKET  0xE7

/* ---- 字宽宏 ---- */
#define HIWORD(x) (((unsigned int)(x) >> 16) & 0xFFFF)
#define LOWORD(x) ((unsigned int)(x) & 0xFFFF)
#define MAKEWORD(lo, hi) ((WORD)(((BYTE)(lo)) | ((WORD)((BYTE)(hi)) << 8)))

/* ---- UTF-8 <-> UTF-16。Windows 版由系统 API 做；这里自己实现，语义一致：
 *      返回写入的字符数（MultiByteToWideChar 含结尾 NUL，WideCharToMultiByte
 *      在 cbMultiByte>0 时含结尾 NUL），失败返回 0。 ---- */
int wc_MultiByteToWideChar(unsigned int cp, unsigned int flags, const char *src,
                           int srclen, WCHAR *dst, int dstlen);
int wc_WideCharToMultiByte(unsigned int cp, unsigned int flags, const WCHAR *src,
                           int srclen, char *dst, int dstlen,
                           const char *defchar, int *useddef);
#define MultiByteToWideChar  wc_MultiByteToWideChar
#define WideCharToMultiByte  wc_WideCharToMultiByte

/* ---- config.c 用到的 Win32 文件 / 环境 API。实现在 src/platform_posix.c。
 *      签名逐条照 Windows 抄，config.c 因此不用改。 ---- */
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define SW_SHOWNORMAL 1
DWORD   GetModuleFileNameW(void *hModule, WCHAR *buf, DWORD n);
FILE   *_wfopen(const WCHAR *path, const WCHAR *mode);
WCHAR  *_wgetenv(const WCHAR *name);
DWORD   GetFileAttributesW(const WCHAR *path);
int     _snwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, ...);
void   *ShellExecuteW(void *hwnd, const WCHAR *op, const WCHAR *file,
                      const WCHAR *params, const WCHAR *dir, int showcmd);

/* ---- 窗口标题：POSIX 上改标题就是 OSC 0 序列，而 render.c 的 update_host_title()
 *      本来就同时发了 `\x1b]0;...\x07`，所以 SetConsoleTitle* 在这儿是空操作。
 *      GetConsoleTitleW 查不到宿主终端的标题，返回 0 —— 调用方本来就有
 *      「空标题就不恢复」的分支（main.c / render.c 都判了 g_orig_title[0]）。 ---- */
static inline int SetConsoleTitleA(const char *t) { (void)t; return 1; }
static inline int SetConsoleTitleW(const WCHAR *t) { (void)t; return 1; }
static inline unsigned long GetConsoleTitleW(void *buf, unsigned long n) {
    (void)buf; (void)n; return 0;
}

/* ---- 剪贴板：POSIX 侧走 OSC 52 / pbcopy / xclip，见 src/platform_posix.c ---- */
#define CF_UNICODETEXT 13
#define CF_TEXT        1
#define GMEM_MOVEABLE  0x0002
#define GHND           0x0042
int   OpenClipboard(void *hwnd);
int   CloseClipboard(void);
int   EmptyClipboard(void);
void *GlobalAlloc(unsigned int flags, size_t bytes);
void *GlobalLock(void *h);
int   GlobalUnlock(void *h);
unsigned int RegisterClipboardFormatA(const char *name);
void *SetClipboardData(unsigned int fmt, void *h);

#endif /* WIN_TERMUX_WINCOMPAT_H */
