/* ---------------------------------------------------------------------------
 * platform_posix.c —— platform.h 的 Linux / macOS 实现。
 *
 * 对应关系：
 *   ConPTY + CreateProcessW   ->  forkpty（子进程拿真 pty，TIOCSWINSZ 改尺寸）
 *   ReadConsoleInputW         ->  termios 原始模式 + 自己把字节流解析成 INPUT_RECORD
 *   GetConsoleScreenBufferInfo->  ioctl(TIOCGWINSZ) + SIGWINCH
 *   Win32 Clipboard           ->  OSC 52（首选，免外部进程）+ pbcopy/wl-copy/xclip 兜底
 *   注册表读系统版本          ->  uname
 *
 * 输入解析是这里最要紧的一块：引擎（keymap.c / input.c）是按 Windows 的
 * 「虚拟键码 + 修饰键状态 + UnicodeChar」写的，所以转义序列必须被翻译成同样的形状，
 * 否则 keymap 整张表都用不上。
 * ------------------------------------------------------------------------- */
#include "platform.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <signal.h>
#include <sys/utsname.h>

#ifdef __APPLE__
#include <util.h>          /* forkpty / openpty（glibc 在 <pty.h>，Apple 在 <util.h>） */
/* _NSGetExecutablePath 声明在 <mach-o/dyld.h>。之前这里写的是 <crt_externs.h>，
 * 那个头给的是 _NSGetEnviron()，于是 _NSGetExecutablePath 成了隐式声明 ——
 * Xcode 15 起的 clang 默认把隐式函数声明当【错误】，macOS 上直接编不过。 */
#include <mach-o/dyld.h>
#else
#include <pty.h>
#endif

/* ========================================================================== */
/* UTF-8 <-> UTF-16                                                           */
/* ========================================================================== */

/* 语义与 Win32 一致：srclen<0 时按「以 NUL 结尾」处理并把 NUL 一起算进返回值。 */
int wc_MultiByteToWideChar(unsigned int cp, unsigned int flags, const char *src,
                           int srclen, WCHAR *dst, int dstlen) {
    (void)cp; (void)flags;
    if (!src) return 0;
    int n = (srclen < 0) ? (int)strlen(src) + 1 : srclen;
    if (n <= 0) return 0;
    int si = 0, di = 0;
    int include_nul = (srclen < 0);
    while (si < n && (di < dstlen || dstlen == 0)) {
        unsigned char c = (unsigned char)src[si];
        unsigned int cp4;
        int extra;
        if (c < 0x80)       { cp4 = c;        extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp4 = c & 0x1Fu;  extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp4 = c & 0x0Fu;  extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp4 = c & 0x07u;  extra = 3; }
        else { cp4 = 0xFFFD; extra = 0; }   /* 非法首字节：替换字符 */
        if (si + extra >= n + (include_nul ? 0 : 0) && extra > 0 && si + extra > n - 1) {
            /* 截断的多字节序列：按替换字符处理 */
            cp4 = 0xFFFD; extra = 0;
        }
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)src[si + k];
            if ((cc & 0xC0) != 0x80) { cp4 = 0xFFFD; extra = 0; break; }
            cp4 = (cp4 << 6) | (cc & 0x3Fu);
        }
        si += extra + 1;
        if (cp4 == 0 && include_nul) { if (di < dstlen) dst[di] = 0; di++; break; }
        if (cp4 > 0xFFFF) {
            /* 平面外：编码成代理对（引擎里有 g_high_surrogate 配套处理）。 */
            unsigned int v = cp4 - 0x10000u;
            if (di < dstlen) dst[di] = (WCHAR)(0xD800u + (v >> 10));
            di++;
            if (di < dstlen) dst[di] = (WCHAR)(0xDC00u + (v & 0x3FFu));
            di++;
        } else {
            if (di < dstlen) dst[di] = (WCHAR)cp4;
            di++;
        }
    }
    return di;
}

int wc_WideCharToMultiByte(unsigned int cp, unsigned int flags, const WCHAR *src,
                           int srclen, char *dst, int dstlen,
                           const char *defchar, int *useddef) {
    (void)cp; (void)flags; (void)defchar;
    if (useddef) *useddef = 0;
    if (!src) return 0;
    int n = (srclen < 0) ? (int)wcslen(src) + 1 : srclen;
    if (n <= 0) return 0;
    int si = 0, di = 0;
    while (si < n) {
        unsigned int cp4 = (unsigned int)src[si++];
        if (cp4 == 0 && srclen < 0) { if (di < dstlen) dst[di] = 0; di++; break; }
        if (cp4 >= 0xD800u && cp4 <= 0xDBFFu && si < n) {
            unsigned int lo = (unsigned int)src[si];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp4 = 0x10000u + ((cp4 - 0xD800u) << 10) + (lo - 0xDC00u);
                si++;
            }
        }
        char tmp[4]; int tl = 0;
        if (cp4 < 0x80) tmp[tl++] = (char)cp4;
        else if (cp4 < 0x800) {
            tmp[tl++] = (char)(0xC0 | (cp4 >> 6));
            tmp[tl++] = (char)(0x80 | (cp4 & 0x3F));
        } else if (cp4 < 0x10000) {
            tmp[tl++] = (char)(0xE0 | (cp4 >> 12));
            tmp[tl++] = (char)(0x80 | ((cp4 >> 6) & 0x3F));
            tmp[tl++] = (char)(0x80 | (cp4 & 0x3F));
        } else {
            tmp[tl++] = (char)(0xF0 | (cp4 >> 18));
            tmp[tl++] = (char)(0x80 | ((cp4 >> 12) & 0x3F));
            tmp[tl++] = (char)(0x80 | ((cp4 >> 6) & 0x3F));
            tmp[tl++] = (char)(0x80 | (cp4 & 0x3F));
        }
        for (int k = 0; k < tl; k++) { if (di < dstlen) dst[di] = tmp[k]; di++; }
    }
    return di;
}

/* ========================================================================== */
/* config.c 用到的 Win32 文件 / 环境替身                                      */
/* ========================================================================== */

static char g_exe_path_u8[MAX_PATH] = {0};

DWORD GetModuleFileNameW(void *h, WCHAR *b, DWORD n) {
    (void)h;
    if (!g_exe_path_u8[0]) {
#ifdef __APPLE__
        char p[MAX_PATH];
        uint32_t sz = sizeof(p);
        if (_NSGetExecutablePath(p, &sz) == 0) snprintf(g_exe_path_u8, sizeof(g_exe_path_u8), "%s", p);
#else
        ssize_t r = readlink("/proc/self/exe", g_exe_path_u8, sizeof(g_exe_path_u8) - 1);
        if (r > 0) g_exe_path_u8[r] = 0; else g_exe_path_u8[0] = 0;
#endif
    }
    if (!b || n == 0) return 0;
    int got = wc_MultiByteToWideChar(CP_UTF8, 0, g_exe_path_u8, -1, b, (int)n);
    return got > 0 ? (DWORD)(got - 1) : 0;
}

static char g_wfopen_path[MAX_PATH], g_wfopen_mode[16];

FILE *_wfopen(const WCHAR *p, const WCHAR *m) {
    if (!p || !m) return NULL;
    wc_WideCharToMultiByte(CP_UTF8, 0, p, -1, g_wfopen_path, (int)sizeof(g_wfopen_path), NULL, NULL);
    wc_WideCharToMultiByte(CP_UTF8, 0, m, -1, g_wfopen_mode, (int)sizeof(g_wfopen_mode), NULL, NULL);
    return fopen(g_wfopen_path, g_wfopen_mode);
}

static WCHAR g_wgetenv_buf[1024];

WCHAR *_wgetenv(const WCHAR *n) {
    if (!n) return NULL;
    char nu8[256] = {0};
    wc_WideCharToMultiByte(CP_UTF8, 0, n, -1, nu8, (int)sizeof(nu8), NULL, NULL);
    const char *v = getenv(nu8);
    if (!v) return NULL;
    wc_MultiByteToWideChar(CP_UTF8, 0, v, -1, g_wgetenv_buf, (int)(sizeof(g_wgetenv_buf)/sizeof(WCHAR)));
    return g_wgetenv_buf;
}

DWORD GetFileAttributesW(const WCHAR *p) {
    if (!p) return INVALID_FILE_ATTRIBUTES;
    char u8[MAX_PATH] = {0};
    wc_WideCharToMultiByte(CP_UTF8, 0, p, -1, u8, (int)sizeof(u8), NULL, NULL);
    struct stat st;
    if (stat(u8, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? 0x10u /* FILE_ATTRIBUTE_DIRECTORY */ : 0x80u;
}

/* --------------------------------------------------------------------------
 * _snwprintf
 *
 * ★ 坑：glibc/macOS 的 swprintf 把 %s / %c 当【窄】字符处理 —— 传 wchar_t*
 *   进去，它按 char* 读，遇到宽字符里的 0x00 字节就停：L"/tmp" 的内存是
 *   2F 00 00 00 74 ...，于是只读到 "/"。必须写 %ls / %lc 才对。
 *   而引擎（config.c 的 resolve_ini_path 等）是按 Windows 的 _snwprintf 写的，
 *   那里 %s 本来就是宽字符。所以在这一层把格式串里的裸 %s / %c 重写成
 *   %ls / %lc：调用方一个字不用改，Windows 走 platform_win.c 完全不受影响。
 *
 *   重写规则：跳过 %% 转义、跳过 flags/宽度/精度、已有 l/h/L/j/z/t 长度修饰
 *   的原样保留，只在裸 s/c 前插一个 l。
 * ------------------------------------------------------------------------- */
int _snwprintf(WCHAR *b, size_t n, const WCHAR *f, ...) {
    if (!b || n == 0) return -1;

    WCHAR fmt[1024];
    const size_t cap = sizeof(fmt) / sizeof(fmt[0]);
    size_t di = 0;
    int overflow = 0;
    for (size_t i = 0; f[i]; ) {
        if (di + 4 >= cap) { overflow = 1; break; }
        if (f[i] != L'%') { fmt[di++] = f[i++]; continue; }
        fmt[di++] = f[i++];                              /* '%' */
        if (f[i] == L'%') { fmt[di++] = f[i++]; continue; }  /* %% 原样 */
        while (f[i] && (wcschr(L"-+ #0", f[i]) ||
                        (f[i] >= L'0' && f[i] <= L'9') ||
                        f[i] == L'.' || f[i] == L'*')) {
            if (di + 4 >= cap) { overflow = 1; break; }
            fmt[di++] = f[i++];
        }
        if (overflow) break;
        int has_len = 0;
        while (f[i] == L'l' || f[i] == L'h' || f[i] == L'L' ||
               f[i] == L'j' || f[i] == L'z' || f[i] == L't') {
            if (di + 4 >= cap) { overflow = 1; break; }
            fmt[di++] = f[i++];
            has_len = 1;
        }
        if (overflow) break;
        if (!has_len && (f[i] == L's' || f[i] == L'c')) fmt[di++] = L'l';
        if (f[i]) fmt[di++] = f[i++];
    }
    fmt[di] = 0;

    va_list ap; va_start(ap, f);
    int r = vswprintf(b, n, overflow ? f : fmt, ap);
    va_end(ap);
    return r;
}

/* 「用系统默认程序打开 ini」：macOS 用 open，其余用 xdg-open。 */
void *ShellExecuteW(void *hwnd, const WCHAR *op, const WCHAR *file,
                    const WCHAR *params, const WCHAR *dir, int show) {
    (void)hwnd; (void)op; (void)params; (void)dir; (void)show;
    if (!file) return NULL;
    char u8[MAX_PATH] = {0};
    wc_WideCharToMultiByte(CP_UTF8, 0, file, -1, u8, (int)sizeof(u8), NULL, NULL);
#ifdef __APPLE__
    const char *argv[] = { "open", u8, NULL };
#else
    const char *argv[] = { "xdg-open", u8, NULL };
#endif
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
    return (void *)(intptr_t)1;
}

/* ========================================================================== */
/* 剪贴板                                                                     */
/* ========================================================================== */

/* 首选 OSC 52：把内容 base64 后塞进 `\x1b]52;c;<b64>\x07`，由宿主终端写系统剪贴板。
 * 好处是不用 fork 外部程序、也不依赖 X11/Wayland 是不是在跑；iTerm2 / kitty /
 * WezTerm / alacritty / Windows Terminal / gnome-terminal(VTE 0.76+) 都支持。
 * 失败兜底：macOS pbcopy，Wayland wl-copy，X11 xclip。 */
static void clip_osc52(const char *utf8, int len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    /* 宿主终端一般限制 OSC 长度；这里按 100KB 原文封顶，够用且不会撑爆转义序列。 */
    if (len > 100000) len = 100000;
    int cap = len * 2 + 64;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return;
    int o = snprintf(buf, (size_t)cap, "\x1b]52;c;");
    const unsigned char *s = (const unsigned char *)utf8;
    for (int i = 0; i < len; i += 3) {
        unsigned int v = ((unsigned int)s[i]) << 16;
        if (i + 1 < len) v |= ((unsigned int)s[i + 1]) << 8;
        if (i + 2 < len) v |= ((unsigned int)s[i + 2]);
        if (o + 4 >= cap) break;
        buf[o++] = tbl[(v >> 18) & 63];
        buf[o++] = tbl[(v >> 12) & 63];
        buf[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        buf[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    if (o + 3 < cap) { buf[o++] = '\x07'; }
    host_write(buf, o);
    free(buf);
}

static void clip_external(const char *utf8, int len) {
#ifdef __APPLE__
    const char *prog = "pbcopy";
#else
    const char *prog = getenv("WAYLAND_DISPLAY") ? "wl-copy" : "xclip";
#endif
    int fds[2];
    if (pipe(fds) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], 0);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }
        close(fds[0]);
#ifndef __APPLE__
        if (!getenv("WAYLAND_DISPLAY")) {
            const char *argv[] = { "xclip", "-selection", "clipboard", "-in", NULL };
            execvp(argv[0], (char *const *)argv);
            _exit(127);
        }
#endif
        execlp(prog, prog, (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    if (pid > 0) {
        int off = 0;
        while (off < len) {
            ssize_t w = write(fds[1], utf8 + off, (size_t)(len - off));
            if (w <= 0) break;
            off += (int)w;
        }
        close(fds[1]);
        int st; waitpid(pid, &st, 0);
    } else {
        close(fds[1]);
    }
}

void plat_clip_copy(const char *utf8, int len) {
    if (!utf8 || len <= 0) return;
    clip_osc52(utf8, len);
    clip_external(utf8, len);   /* OSC 52 不支持的宿主终端由这条兜底；两条都做不会互相干扰 */
}

/* ---- Win32 剪贴板 API 的替身 ----
 * ★ 这几个不是「只为满足链接」。src/input.c:1651 的复制模式走的是
 *   OpenClipboard / GlobalAlloc / GlobalLock / SetClipboardData(CF_UNICODETEXT)
 *   这一整套 Win32 调用，并没有走 plat_clip_copy —— 所以早先这里全写成空实现时，
 *   Linux/macOS 上【复制模式按 Enter 什么都不发生】，而且不报任何错。
 *   现在让 SetClipboardData 在收到 CF_UNICODETEXT 时把宽字符缓冲转成 UTF-8
 *   交给 plat_clip_copy（OSC 52 + pbcopy/wl-copy/xclip），input.c 因此一个字
 *   都不用改，Windows 路径也完全不受影响。
 * HTML Format 没有 OSC 52 的对应物，收到就直接忽略。 */
static char *g_clip_hglobal = NULL;      /* 待释放的最后一块 */

int   OpenClipboard(void *h) { (void)h; return 1; }
int   EmptyClipboard(void) { return 1; }
void *GlobalLock(void *h) { return h; }
int   GlobalUnlock(void *h) { (void)h; return 1; }
unsigned int RegisterClipboardFormatA(const char *n) { (void)n; return 0; }

void *GlobalAlloc(unsigned int f, size_t n) {
    (void)f;
    /* 调用方在一次 Open/Close 之间可能分配两块（纯文本 + HTML）。
     * 上一块在 SetClipboardData 时已经用完了，这里释放是安全的。 */
    free(g_clip_hglobal);
    g_clip_hglobal = (char *)calloc(1, n ? n : 1);
    return g_clip_hglobal;
}

void *SetClipboardData(unsigned int fmt, void *h) {
    if (fmt == CF_UNICODETEXT && h) {
        const WCHAR *w = (const WCHAR *)h;
        int n = (int)wcslen(w);
        if (n > 0) {
            /* 封顶 100KB：OSC 52 是把内容塞进转义序列，太大宿主终端会丢或卡。 */
            if (n > 100000) n = 100000;
            char *u8 = (char *)malloc((size_t)n * 4 + 4);
            if (u8) {
                int got = wc_WideCharToMultiByte(CP_UTF8, 0, w, n, u8, n * 4 + 3, NULL, NULL);
                if (got > 0) { u8[got] = 0; plat_clip_copy(u8, got); }
                free(u8);
            }
        }
    }
    return h;      /* HTML Format 等其它格式：忽略 */
}

int CloseClipboard(void) {
    free(g_clip_hglobal);
    g_clip_hglobal = NULL;
    return 1;
}

/* ========================================================================== */
/* 系统信息                                                                   */
/* ========================================================================== */

void plat_sysinfo(char *out, int cap) {
    struct utsname u;
    if (uname(&u) != 0) { snprintf(out, (size_t)cap, "%s", "POSIX"); return; }
#ifdef __APPLE__
    snprintf(out, (size_t)cap, "macOS (%s %s, %s)", u.sysname, u.release, u.machine);
#else
    snprintf(out, (size_t)cap, "%s %s (%s)", u.sysname, u.release, u.machine);
#endif
}

const WCHAR *plat_user_home(void) {
    static WCHAR buf[MAX_PATH];
    static int done = 0;
    if (done) return buf[0] ? buf : NULL;
    done = 1;
    const char *h = getenv("HOME");
    if (!h || !*h) {
        struct passwd *pw = getpwuid(getuid());
        h = (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : NULL;
    }
    if (!h) { buf[0] = 0; return NULL; }
    wc_MultiByteToWideChar(CP_UTF8, 0, h, -1, buf, (int)(sizeof(buf) / sizeof(WCHAR)));
    return buf;
}

/* ========================================================================== */
/* 子进程：forkpty                                                            */
/* ========================================================================== */

/* --------------------------------------------------------------------------
 * 把一条命令行切成 argv（原地改写 buf，返回 argc，argv 末尾补 NULL）。
 *
 * ★ 原来这里只按空格切，带引号的参数会被撕碎：
 *     /bin/sh -c "echo hi; sleep 6"
 *   会切出 ["/bin/sh","-c","\"echo","hi;","sleep","6\""]，于是子进程实际跑的是
 *   /bin/sh -c '"echo' —— 直接失败，窗格一闪就没了。Windows 侧 CreateProcessW
 *   收到的是整条字符串、引号由子进程的 CRT 解析，所以那边能用：两边行为不一致。
 *
 * 规则（与 shell 一致的常识部分，不做变量展开 / 通配 / 管道）：
 *   · 未加引号的空格 / 制表符是分词符
 *   · '...' 与 "..." 内的空格不分词，引号本身不进 argv
 *   · 反斜杠转义下一个字符（双引号内也认，与 shell 一致）
 *   · 引号没收尾就一直吃到串尾
 *   · 空参数（""）会被保留成一个空串
 * ------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------
 * 展开启动目录里的环境变量。
 *
 * Windows 侧用 ExpandEnvironmentStringsW，POSIX 分支原先【什么都不做】，直接把
 * %USERPROFILE% 原样交给 chdir —— 而设置页的提示明写着「支持 %USERPROFILE%」，
 * 于是这个功能在 Linux/macOS 上是死的，而且失败还被静默吞掉。
 *
 * 规则：
 *   · %NAME%  -> getenv(NAME)；找不到再试全大写；仍找不到就原样保留（与 Windows
 *                对未定义变量的处理一致）
 *   · %USERPROFILE% / %HOMEPATH% / %HOMEDRIVE%%HOMEPATH% -> 用户主目录
 *                （这是设置页唯一承诺的那个变量，POSIX 上对应 $HOME）
 *   · 开头的 ~ 或 ~/ -> 用户主目录（Linux 用户的直觉写法）
 * ------------------------------------------------------------------------- */
static const char *env_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : NULL;
}

void posix_expand_env(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!in) return;
    size_t o = 0;
    const char *p = in;

    /* 开头的 ~ 或 ~/ */
    if (p[0] == '~' && (p[1] == 0 || p[1] == '/')) {
        const char *h = env_home();
        if (h) {
            size_t n = strlen(h);
            if (n > out_sz - 1) n = out_sz - 1;
            memcpy(out, h, n);
            o = n;
        }
        p++;                                  /* 跳过 ~，后面的 / 照常拷贝 */
    }

    while (*p && o + 1 < out_sz) {
        if (*p == '%') {
            const char *q = p + 1;
            const char *name_end = q;
            while (*name_end && *name_end != '%' &&
                   ((*name_end >= 'A' && *name_end <= 'Z') ||
                    (*name_end >= 'a' && *name_end <= 'z') ||
                    (*name_end >= '0' && *name_end <= '9') || *name_end == '_'))
                name_end++;
            if (*name_end == '%' && name_end > q) {
                char name[128];
                size_t nl = (size_t)(name_end - q);
                if (nl >= sizeof(name)) nl = sizeof(name) - 1;
                memcpy(name, q, nl);
                name[nl] = 0;
                const char *val = NULL;
                if (!strcasecmp(name, "USERPROFILE") || !strcasecmp(name, "HOMEPATH") ||
                    !strcasecmp(name, "HOMEDRIVE")) {
                    val = env_home();          /* %HOMEDRIVE% 也给 HOME，够用且不出错 */
                } else {
                    val = getenv(name);
                    if (!val) {
                        char up[128];
                        size_t k;
                        for (k = 0; name[k]; k++) up[k] = (char)toupper((unsigned char)name[k]);
                        up[k] = 0;
                        val = getenv(up);
                    }
                }
                if (val) {
                    size_t n = strlen(val);
                    if (o + n >= out_sz) n = out_sz - 1 - o;
                    memcpy(out + o, val, n);
                    o += n;
                    p = name_end + 1;          /* 跳过收尾的 % */
                    continue;
                }
                /* 未定义的变量：原样保留，与 Windows 一致 */
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* 注意：这里【故意】不叫 plat_*、也不进 platform.h —— 它不是平台抽象，Windows
 * 侧压根没有对应物（CreateProcessW 收的是整条命令行，引号由子进程 CRT 解析）。
 * 叫 plat_* 会让 verify_port.py 第 1 节的「每个 plat_* 两侧都要有实现」不变量误报。 */
int posix_split_cmdline(char *buf, char **argv, int max_args) {
    if (!buf || !argv || max_args <= 1) { if (argv && max_args > 0) argv[0] = NULL; return 0; }
    int argc = 0;
    char *p = buf;
    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *out = p;
        char *w = out;
        while (*p) {
            if (*p == '\\' && p[1]) { *w++ = p[1]; p += 2; continue; }
            if (*p == '\'' || *p == '"') {
                char q = *p++;
                while (*p && *p != q) {
                    if (*p == '\\' && q == '"' && p[1]) { *w++ = p[1]; p += 2; }
                    else { *w++ = *p++; }
                }
                if (*p) p++;                 /* 跳过收尾引号 */
                continue;
            }
            if (*p == ' ' || *p == '\t') {
                /* ★ 必须先把分隔符吃掉再写 NUL：没有发生压缩时 w == p，
                 * 直接 *w = 0 会把 p 正要跳过的分隔符盖成 NUL，外层循环就以为
                 * 串已经到头，于是永远只切出第一个参数。 */
                p++;
                break;
            }
            *w++ = *p++;
        }
        *w = 0;
        argv[argc++] = out;                  /* 空参数（""）也保留 */
    }
    argv[argc] = NULL;
    return argc;
}

int plat_proc_spawn(const char *cmd_utf8, const char *workdir_utf8,
                    int cols, int rows, HANDLE *out_in, HANDLE *out_out,
                    HANDLE *out_proc) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) return -1;

    if (pid == 0) {
        /* ---- 子进程：已经有控制终端了，直接 exec shell ---- */
        if (workdir_utf8 && *workdir_utf8) {
            if (chdir(workdir_utf8) != 0) {
                /* 目录不存在 / 没权限。原来这里只有一句注释、什么都不做，于是
                 * 静默留在 termux 的启动目录 —— 用户完全不知道自己的设置没生效。
                 * 现在按注释承诺退回 HOME，并且把这件事说出来。 */
                int e = errno;
                const char *h = env_home();
                fprintf(stderr, "termux: 启动目录 \"%s\" 不可用（%s）",
                        workdir_utf8, strerror(e));
                if (h && *h && chdir(h) == 0)
                    fprintf(stderr, "，已改用 %s\r\n", h);
                else
                    fprintf(stderr, "，留在当前目录\r\n");
            }
        }
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        /* cmd_utf8 可能带参数（菜单项里可以写 "bash -l"，也可以写
         * /bin/sh -c "echo hi; sleep 6"），按 shell 的习惯切成 argv。 */
        char cmdbuf[512];
        snprintf(cmdbuf, sizeof(cmdbuf), "%s", cmd_utf8 && *cmd_utf8 ? cmd_utf8 : "/bin/sh");
        char *argv[32];
        int argc = posix_split_cmdline(cmdbuf, argv, 32);
        if (argc == 0) { argv[0] = (char *)"/bin/sh"; argv[1] = NULL; }
        execvp(argv[0], argv);
        fprintf(stderr, "termux: 无法启动 \"%s\": %s\r\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* ---- 父进程 ---- */
    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);
    if (out_in)   *out_in   = (HANDLE)(intptr_t)master;
    if (out_out)  *out_out  = (HANDLE)(intptr_t)master;   /* pty master 读写同一端 */
    if (out_proc) *out_proc = (HANDLE)(intptr_t)pid;
    return 0;
}

void plat_proc_resize(HANDLE proc, HANDLE in_fd, HANDLE out_fd, int cols, int rows) {
    (void)proc; (void)in_fd;
    int fd = (int)(intptr_t)out_fd;
    if (fd <= 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 1);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 1);
    ioctl(fd, TIOCSWINSZ, &ws);   /* 子进程会收到 SIGWINCH，shell 自己重排 */
}

int plat_proc_exited(HANDLE proc, DWORD *exit_code) {
    pid_t pid = (pid_t)(intptr_t)proc;
    if (pid <= 0) return 0;
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0 || r < 0) return 0;
    if (exit_code) {
        if (WIFEXITED(st)) *exit_code = (DWORD)WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) *exit_code = (DWORD)(128 + WTERMSIG(st));
        else *exit_code = 0;
    }
    return 1;
}

void plat_proc_kill(HANDLE proc) {
    pid_t pid = (pid_t)(intptr_t)proc;
    if (pid <= 0) return;
    kill(pid, SIGHUP);            /* 先礼后兵：shell 收到 SIGHUP 会正常退出 */
    for (int i = 0; i < 20; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(25000);
    }
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
}

void plat_proc_close(HANDLE *proc, HANDLE *in_fd, HANDLE *out_fd) {
    int fd = in_fd ? (int)(intptr_t)*in_fd : 0;
    if (out_fd && (int)(intptr_t)*out_fd != fd) {
        if (*out_fd != NULL_HANDLE) close((int)(intptr_t)*out_fd);
        *out_fd = NULL_HANDLE;
    }
    if (in_fd) {
        if (*in_fd != NULL_HANDLE) close(fd);
        *in_fd = NULL_HANDLE;
    }
    if (proc && *proc != NULL_HANDLE) {
        plat_proc_kill(*proc);
        *proc = NULL_HANDLE;
    }
}

/* ========================================================================== */
/* 线程 / 非阻塞 I/O                                                          */
/* ========================================================================== */

/* 线程上下文。为什么要留一个 done 标志而不是 detach 完事：
 * Windows 侧 close_pane / reap_dead_panes 用 WaitForSingleObject(read_thread, ms)，
 * 线程一结束就立刻返回。POSIX 上如果只是「睡满 wait_ms」，关掉 4 个窗格要
 * 白等 4×2000ms，退出一次要 8 秒 —— 实测就是这样。所以线程跑完置位 done，
 * join 每 2ms 查一次，一置位就 pthread_join 回收。 */
typedef struct {
    plat_thread_fn fn;
    void *arg;
    pthread_t th;
    volatile int done;
} thr_ctx;

static void *thr_trampoline(void *p) {
    thr_ctx *c = (thr_ctx *)p;
    c->fn(c->arg);
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

HANDLE plat_thread_start(plat_thread_fn fn, void *arg) {
    thr_ctx *c = (thr_ctx *)calloc(1, sizeof(thr_ctx));
    if (!c) return NULL_HANDLE;
    c->fn = fn; c->arg = arg;
    if (pthread_create(&c->th, NULL, thr_trampoline, c) != 0) { free(c); return NULL_HANDLE; }
    /* 不 detach：要能 pthread_join 回收。等不到就 detach，见下。 */
    return (HANDLE)(uintptr_t)c;
}

void plat_thread_join(HANDLE *th, unsigned wait_ms) {
    if (!th || *th == NULL_HANDLE) return;
    thr_ctx *c = (thr_ctx *)(uintptr_t)*th;
    unsigned waited = 0;
    while (waited < wait_ms && !__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) {
        usleep(2000);
        waited += 2;
    }
    if (__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) {
        pthread_join(c->th, NULL);      /* 已结束：回收栈和内核结构 */
        free(c);
    } else {
        /* 等不到（读线程还阻塞在 pty 上）。detach 让内核自己回收线程；
         * c 故意不 free —— 那个线程还要往 c->done 里写，free 了就是野指针。
         * 这是超时路径上的少量泄漏，语义与 Windows 侧「等不到就不管」一致。 */
        pthread_detach(c->th);
    }
    *th = NULL_HANDLE;
}

int plat_read(HANDLE h, char *buf, int cap) {
    int fd = (int)(intptr_t)h;
    if (fd <= 0 || cap <= 0) return -1;
    for (;;) {
        ssize_t r = read(fd, buf, (size_t)cap);
        if (r > 0) return (int)r;
        if (r == 0) return 0;                    /* EOF：子进程退了 */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 非阻塞 fd 上暂时没数据：等一会儿再读，保持和 Windows 侧
             * 「阻塞 ReadFile」一样的行为（读线程就靠这个空转等输出）。 */
            struct pollfd pfd;
            pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, 50);
            if (pr < 0 && errno != EINTR) return -1;
            continue;
        }
        return -1;
    }
}

/* master fd 是 O_NONBLOCK 的（见 plat_proc_spawn），所以子进程一时不读时 write 会
 * 返回 EAGAIN。旧代码在这里直接 return -1，那一段输入就【静默丢掉】了：实测子进程
 * sleep 时灌 30000 字节，只有 4095 字节（tty 缓冲上限）到达，其余 86% 凭空消失，
 * 而且没有任何提示。Windows 侧的 WriteFile 是阻塞的、不会丢，两边行为不一致。
 *
 * 这里改成有限等待：EAGAIN 时用 poll(POLLOUT) 让出 CPU，等子进程把 tty 排空再继续。
 * 累计封顶 WRITE_WAIT_MS —— 写键路径跑在输入线程上，一个卡死的子进程不能把整个
 * UI 永久冻住；到点了就止损返回已写字节数，行为退化成「丢弃」，但正常的慢子进程
 * （几百毫秒级）已经不会再丢任何东西。 */
/* ★ 原来这里是 1000ms，注释说「正常的慢子进程（几百毫秒级）已经不会再丢任何
 * 东西」。这个前提在 macOS 上【不成立】：Apple 的 pty 输入缓冲只有 ~1KB
 * （实测写满 1022 字节就 EAGAIN），而 Linux 的缓冲大得多。同样一个「停顿 1 秒
 * 才开始读」的子进程，Linux 上几乎不用等就灌完了，macOS 上必须实打实地等满那
 * 1 秒 —— 预算和停顿一样长，纯属赛跑，CI 的 macOS 作业就是这么红的（20000 字节
 * 只写出 1022，丢了 95%）。
 * 提到 5000ms：卡死的子进程最多把输入线程冻 5 秒（可恢复），而静默丢按键是
 * 看不见的、且用户无法重试。两害相权取其轻。 */
#define WRITE_WAIT_MS 5000

int plat_write_fd(HANDLE h, const char *buf, int len) {
    int fd = (int)(intptr_t)h;
    if (fd <= 0 || len <= 0) return 0;
    int off = 0;
    int waited = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w > 0) { off += (int)w; waited = 0; continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (waited >= WRITE_WAIT_MS) break;
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            waited += (pr == 0) ? 100 : 0;   /* 只有真超时才计入预算 */
            continue;
        }
        break;                               /* EPIPE / EIO：子进程已经没了 */
    }
    return off > 0 ? off : -1;
}

int plat_peek_avail(HANDLE h) {
    int fd = (int)(intptr_t)h;
    if (fd <= 0) return 0;
    int n = 0;
    if (ioctl(fd, FIONREAD, &n) != 0) return 0;
    return n;
}

/* ========================================================================== */
/* 宿主控制台                                                                 */
/* ========================================================================== */

static struct termios g_orig_termios;
static int g_termios_saved = 0;
/* term_input_posix.c 在 plat_console_read 里读它，收到 SIGWINCH 就补一条
 * WINDOW_BUFFER_SIZE_EVENT 给主循环。 */
volatile sig_atomic_t g_winch = 0;

static void on_sigwinch(int sig) { (void)sig; g_winch = 1; }

int plat_console_init(int want_mouse) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return -1;
    g_termios_saved = 1;

    struct termios t = g_orig_termios;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    /* 关输出处理：Windows 侧设了 DISABLE_NEWLINE_AUTO_RETURN，渲染器本来就自己
     * 发 \r\n，这里必须一致，否则换行会多一个 CR。 */
    t.c_oflag &= (tcflag_t)~(OPOST);
    t.c_cflag |= (tcflag_t)CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigwinch;
    sigaction(SIGWINCH, &sa, NULL);
    (void)want_mouse;   /* 鼠标追踪的开关序列由 main 发（\x1b[?1003h 等） */
    return 0;
}

void plat_console_shutdown(void) {
    if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_termios_saved = 0;
}

int plat_console_size(int *cols, int *total_rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) {
        const char *c = getenv("COLUMNS"), *r = getenv("LINES");
        if (cols) *cols = c ? atoi(c) : 80;
        if (total_rows) *total_rows = r ? atoi(r) : 24;
        return 0;
    }
    if (cols) *cols = ws.ws_col;
    if (total_rows) *total_rows = ws.ws_row;
    return 0;
}

const WCHAR *plat_default_shell(void) {
    static WCHAR buf[256];
    static int done = 0;
    if (done) return buf;
    done = 1;
    const char *sh = getenv("SHELL");
    if (!sh || !*sh) {
        struct passwd *pw = getpwuid(getuid());
        sh = (pw && pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh";
    }
    wc_MultiByteToWideChar(CP_UTF8, 0, sh, -1, buf, (int)(sizeof(buf)/sizeof(WCHAR)));
    return buf;
}
