/* 仅供 Linux 侧单元测试使用的 windows.h 最小替身。
 * 目标是让不依赖 Win32 API 的纯逻辑模块（theme.c / keymap.c）能在
 * CI 的 ubuntu runner 上原生编译并直接跑断言，而不是靠 Python 复刻一遍逻辑。 */
#ifndef WIN_TERMUX_TEST_STUB_WINDOWS_H
#define WIN_TERMUX_TEST_STUB_WINDOWS_H

#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <stdio.h>   /* FILE：下面的 _wfopen 替身要用 */

typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef unsigned long long DWORD64;
typedef unsigned char BYTE;
typedef int BOOL;
typedef wchar_t WCHAR;
typedef void *HANDLE;
typedef void *HPCON;
typedef unsigned int UINT;
typedef long LONG;

#define MAX_PATH 260
#define TRUE 1
#define FALSE 0

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp

/* Win32 调用约定在原生 Linux 编译下置空（部分头里有 __stdcall 函数声明）。 */
#ifndef __stdcall
#define __stdcall
#endif
#ifndef WINAPI
#define WINAPI
#endif

/* 控制台单元（仅 loghist/screen 等纯逻辑模块需要；真实定义见 Win32 wincon.h）。 */
typedef struct _CHAR_INFO {
    union {
        WCHAR UnicodeChar;
        char  AsciiChar;
    } Char;
    WORD Attributes;
} CHAR_INFO;

typedef struct _COORD {
    short X;
    short Y;
} COORD;

typedef struct _SMALL_RECT {
    short Left, Top, Right, Bottom;
} SMALL_RECT;

/* CRITICAL_SECTION 替身：纯逻辑测试不真正用临界区（Enter/Leave 为空宏）。 */
typedef struct { int dummy; } CRITICAL_SECTION;
#define InitializeCriticalSection(p) (void)(p)
#define DeleteCriticalSection(p)     (void)(p)
#define EnterCriticalSection(p)      (void)(p)
#define LeaveCriticalSection(p)      (void)(p)

/* 鼠标事件记录替身（只列访问到的字段）。dwMousePosition 用 COORD。 */
typedef struct _MOUSE_EVENT_RECORD {
    COORD dwMousePosition;
    DWORD dwButtonState;
    DWORD dwControlKeyState;
    DWORD dwEventFlags;
} MOUSE_EVENT_RECORD;

#define FROM_LEFT_1ST_BUTTON_PRESSED  0x0001
#define FROM_LEFT_2ND_BUTTON_PRESSED  0x0004
#define RIGHTMOST_BUTTON_PRESSED      0x0002
#define DOUBLE_CLICK                  0x0002
#define MOUSE_WHEELED                 0x0004
#define MOUSE_HWHEELED                0x0008

#define COMMON_LVB_UNDERSCORE  0x8000
#define COMMON_LVB_REVERSE_VIDEO 0x4000
#define FOREGROUND_INTENSITY   0x0008
#define BACKGROUND_INTENSITY   0x0080
#define FOREGROUND_RED 0x0004
#define FOREGROUND_GREEN 0x0002
#define FOREGROUND_BLUE 0x0001
#define BACKGROUND_RED 0x0040
#define BACKGROUND_GREEN 0x0020
#define BACKGROUND_BLUE 0x0010

/* 控制键状态位 */
#define RIGHT_ALT_PRESSED   0x0001
#define LEFT_ALT_PRESSED    0x0002
#define RIGHT_CTRL_PRESSED  0x0004
#define LEFT_CTRL_PRESSED   0x0008
#define SHIFT_PRESSED       0x0010

/* 虚拟键码（只列出 keymap 用到的） */
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12
#define VK_ESCAPE  0x1B
#define VK_CAPITAL 0x14
#define VK_SPACE   0x20
#define VK_NUMLOCK 0x90
#define VK_SCROLL  0x91
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E
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
#define VK_ADD     0x6B
#define VK_F1      0x70
#define VK_F2      0x71
#define VK_F5      0x74
#define VK_F24     0x87
#define VK_OEM_1   0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_PACKET  0xE7   /* 输入法注入的字符：vk 无意义，只有 UnicodeChar 可信 */
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2   0xBF
#define VK_OEM_3   0xC0
#define VK_OEM_4   0xDB
#define VK_OEM_5   0xDC
#define VK_OEM_6   0xDD
#define VK_OEM_7   0xDE
/* input.c / keymap.c 用到而 stub 原先缺的虚拟键码（值取自 Win32 头）。 */
#define VK_F3      0x72
#define VK_F4      0x73
#define VK_F6      0x75
#define VK_F7      0x76
#define VK_F8      0x77
#define VK_F9      0x78
#define VK_F10     0x79
#define VK_F11     0x7A
#define VK_F12     0x7B
#define VK_LWIN    0x5B
#define VK_RWIN    0x5C

/* --- 2026-09-15 追加：让 src/render.c 能在 Linux 下编译运行 ---
 * 早先一直声称「render.c 依赖完整 Win32，Linux 下跑不起来」，所以每次改渲染都只能
 * 靠真机日志反推，来回猜了三轮锚定策略。实际一试只缺下面这几个声明：
 * render.c 真正用到的 Win32 只有标题读写、临界区（上面已是空宏）和 tick 计数，
 * 渲染本身是往缓冲区写字节，完全可测。补齐后就能在 Linux 侧直接比对渲染输出，
 * 不必再等真机日志。 */

/* input.h 的 handle_key_input 签名要用到。字段按 src/input.c 实际访问的列出
 * （ke->bKeyDown / dwControlKeyState / uChar.UnicodeChar / wRepeatCount /
 *   wVirtualKeyCode / wVirtualScanCode）。 */
typedef struct _KEY_EVENT_RECORD {
    int bKeyDown;
    unsigned short wRepeatCount;
    unsigned short wVirtualKeyCode;
    unsigned short wVirtualScanCode;
    union {
        unsigned short UnicodeChar;
        char AsciiChar;
    } uChar;
    unsigned int dwControlKeyState;
} KEY_EVENT_RECORD;

/* input.c 的搜索路径用 UTF-8 → UTF-16 转换。测试里给一个够用的实现：
 * 逐字节解 UTF-8，只处理 BMP，够覆盖搜索关键字的场景。 */
#define CP_UTF8 65001
static inline int MultiByteToWideChar(unsigned int cp, unsigned int flags,
                                      const char *src, int srclen,
                                      WCHAR *dst, int dstlen) {
    (void)cp; (void)flags;
    if (srclen < 0) srclen = (int)strlen(src);
    int i = 0, o = 0;
    while (i < srclen && o < dstlen) {
        unsigned char c = (unsigned char)src[i];
        unsigned int cp2;
        int extra;
        if (c < 0x80)       { cp2 = c;              extra = 0; }
        else if (c < 0xE0)  { cp2 = c & 0x1F;       extra = 1; }
        else if (c < 0xF0)  { cp2 = c & 0x0F;       extra = 2; }
        else                { cp2 = c & 0x07;       extra = 3; }
        for (int k = 0; k < extra && i + 1 + k < srclen; k++)
            cp2 = (cp2 << 6) | ((unsigned char)src[i + 1 + k] & 0x3F);
        i += extra + 1;
        dst[o++] = (WCHAR)cp2;
    }
    return o;
}

/* input.c 的剪贴板与滚轮路径用到。测试里不碰系统剪贴板，全部给无害的空实现；
 * HIWORD 只是取高 16 位，用于滚轮方向。 */
#define HIWORD(x) (((unsigned int)(x) >> 16) & 0xFFFF)
#define LOWORD(x) ((unsigned int)(x) & 0xFFFF)
/* input.c 的鼠标分派用到的事件标志。 */
#ifndef MOUSE_MOVED
#define MOUSE_MOVED   0x0001
#endif
typedef short SHORT;
typedef void *HGLOBAL;
#define GMEM_MOVEABLE 0x0002
#define GHND 0x0042
#define CF_UNICODETEXT 13
static inline int OpenClipboard(void *h) { (void)h; return 0; }
static inline int CloseClipboard(void) { return 1; }
static inline int EmptyClipboard(void) { return 1; }
static inline void *GlobalAlloc(unsigned int f, unsigned long n) { (void)f; (void)n; return 0; }
static inline void *GlobalLock(void *h) { return h; }
static inline int GlobalUnlock(void *h) { (void)h; return 1; }
static inline unsigned int RegisterClipboardFormatA(const char *n) { (void)n; return 0; }
static inline void *SetClipboardData(unsigned int f, void *h) { (void)f; return h; }

/* ------------------------------------------------------------------
 * 控制台 API 的测试替身。
 *
 * 尺寸由 g_stub_cols / g_stub_rows 控制，harness 改这两个变量就等于
 * 拖动分屏；ReadConsoleInputW 从 g_stub_events 队列取事件。
 * 真实 IO 一律不做。
 * ------------------------------------------------------------------ */
typedef unsigned long long ULONGLONG;
typedef struct {
    COORD      dwSize;
    COORD      dwCursorPosition;
    WORD       wAttributes;
    SMALL_RECT srWindow;
    COORD      dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;
typedef struct {
    DWORD EventType;
    union {
        KEY_EVENT_RECORD   KeyEvent;
        MOUSE_EVENT_RECORD MouseEvent;
    } Event;
} INPUT_RECORD;
#define KEY_EVENT 1
#define MOUSE_EVENT 2
#define WINDOW_BUFFER_SIZE_EVENT 4
#define STD_OUTPUT_HANDLE (-11)
#define STD_INPUT_HANDLE  (-10)
#define ENABLE_WINDOW_INPUT 0x0008
#define CP_ACP 0
#define WAIT_OBJECT_0 0
#define INVALID_HANDLE_VALUE ((void *)(long)-1)
#define ENABLE_LINE_INPUT     0x0002
#define ENABLE_ECHO_INPUT     0x0004
#define ENABLE_PROCESSED_INPUT 0x0001
#define ENABLE_QUICK_EDIT_MODE 0x0040
#define ENABLE_MOUSE_INPUT    0x0010
#define ENABLE_EXTENDED_FLAGS 0x0080
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define ENABLE_PROCESSED_OUTPUT 0x0001
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#define ENABLE_WRAP_AT_EOL_OUTPUT 0x0002

/* 声明放头文件，定义由测试驱动单点提供，避免 multiple definition。 */
extern int  g_stub_cols;
extern int  g_stub_rows;
extern INPUT_RECORD *g_stub_events;
extern int  g_stub_event_count;
extern int  g_stub_event_pos;

static inline void *GetStdHandle(int n) { (void)n; return (void *)(long)1; }
static inline int GetConsoleScreenBufferInfo(void *h, CONSOLE_SCREEN_BUFFER_INFO *i) {
    (void)h;
    i->srWindow.Left = 0; i->srWindow.Top = 0;
    i->srWindow.Right = (SHORT)(g_stub_cols - 1);
    i->srWindow.Bottom = (SHORT)(g_stub_rows - 1);
    i->dwSize.X = (SHORT)g_stub_cols; i->dwSize.Y = (SHORT)g_stub_rows;
    i->dwCursorPosition.X = 0; i->dwCursorPosition.Y = 0;
    return 1;
}
static inline int ReadConsoleInputW(void *h, INPUT_RECORD *b, DWORD n, DWORD *rd) {
    (void)h;
    DWORD k = 0;
    while (k < n && g_stub_event_pos < g_stub_event_count) b[k++] = g_stub_events[g_stub_event_pos++];
    *rd = k;
    return k > 0;
}
/* host_write 走这里，所以把字节攒进 g_stub_out 就等于捕获整帧渲染输出。 */
extern char *g_stub_out;
extern int   g_stub_out_len;
extern int   g_stub_out_cap;
static inline int WriteConsoleA(void *h, const void *b, DWORD n, DWORD *w, void *r) {
    (void)h; (void)r;
    if (g_stub_out && g_stub_out_len + (int)n < g_stub_out_cap) {
        memcpy(g_stub_out + g_stub_out_len, b, n);
        g_stub_out_len += (int)n;
        g_stub_out[g_stub_out_len] = 0;
    }
    if (w) *w = n;
    return 1;
}
static inline int WideCharToMultiByte(unsigned int cp, DWORD f, const WCHAR *w, int wl,
                                      char *out, int ol, const char *def, int *used) {
    (void)cp; (void)f; (void)def; (void)used;
    int n = 0;
    for (int i = 0; i < wl && n + 4 < ol; i++) {
        unsigned int c = (unsigned int)w[i];
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
               out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    return n;
}
static inline int GetConsoleCP(void) { return 65001; }
static inline int GetConsoleOutputCP(void) { return 65001; }
static inline int SetConsoleCP(unsigned int c) { (void)c; return 1; }
static inline int SetConsoleOutputCP(unsigned int c) { (void)c; return 1; }
static inline int GetConsoleMode(void *h, DWORD *m) { (void)h; *m = 0; return 1; }
static inline int SetConsoleMode(void *h, DWORD m) { (void)h; (void)m; return 1; }
static inline int SetConsoleTitleW(const WCHAR *t) { (void)t; return 1; }
static inline int SetConsoleCtrlHandler(void *h, int a) { (void)h; (void)a; return 1; }
static inline void Sleep(DWORD ms) { (void)ms; }
static inline DWORD WaitForSingleObject(void *h, DWORD ms) { (void)h; (void)ms; return 0; }
static inline long InterlockedExchange(long *t, long v) { long o = *t; *t = v; return o; }
static inline int ResizePseudoConsole(void *h, COORD sz) { (void)h; (void)sz; return 0; }

/* 配置读写：harness 里没有 termux.ini，让这些调用优雅失败即可。 */
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
static inline DWORD GetModuleFileNameW(void *h, WCHAR *b, DWORD n) {
    (void)h; if (n > 0) b[0] = 0; return 0;
}
static inline DWORD GetFileAttributesW(const WCHAR *p) { (void)p; return INVALID_FILE_ATTRIBUTES; }
static inline WCHAR *_wgetenv(const WCHAR *n) { (void)n; return 0; }
static inline FILE *_wfopen(const WCHAR *p, const WCHAR *m) { (void)p; (void)m; return 0; }
static inline int _snwprintf(WCHAR *b, size_t n, const WCHAR *f, ...) {
    (void)b; (void)n; (void)f; return 0;
}
static inline void *ShellExecuteW(void *a, const WCHAR *b, const WCHAR *c, const WCHAR *d,
                                  const WCHAR *e, int f2) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f2; return 0;
}
#define SW_SHOWNORMAL 1

/* 标题读写：测试里不关心标题，吃掉即可。 */
static inline unsigned long GetConsoleTitleW(void *buf, unsigned long n) { (void)buf; (void)n; return 0; }
static inline int SetConsoleTitleA(const char *t) { (void)t; return 1; }

/* render_screen 用它做节流判断；测试里单调递增即可。 */
static inline unsigned long long GetTickCount64(void) {
    static unsigned long long t = 0;
    return ++t;
}

#endif /* WIN_TERMUX_TEST_STUB_WINDOWS_H */
