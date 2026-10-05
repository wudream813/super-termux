#include "conpty_loader.h"

/* tests/loaderstub 的替身头文件只替了 4 个 Win32 调用，没带这些宏；真 windows.h
 * 有，所以用 #ifndef 兜住，两边都能编。 */
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((long)(hr)) >= 0)      /* 不用 LONG：替身头文件里也没这个 typedef */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT(WINAPI *fn_create)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT(WINAPI *fn_resize)(HPCON, COORD);
typedef void(WINAPI *fn_close)(HPCON);

static int g_init_done = 0;
static int g_ok = 0;
static int g_bundled = 0;
static HMODULE g_dll = NULL;

static fn_create p_create = NULL;
static fn_resize p_resize = NULL;
static fn_close p_close = NULL;

static const char *g_source = "none";
static DWORD g_flags = 0;
static int g_flags_set = 0;
static int g_pas_tri = 0;        /* 0 auto / 1 on / -1 off */
static int g_pas_from_ini = 0;   /* 1 = 已由 main 从 ini 设定，env 不再覆盖 */
static int g_pas_effective = 0;  /* 本进程实际创建 ConPTY 时有没有带上 0x8 */
static unsigned long g_os_build = 0;
static int g_os_build_read = 0;

/* 两个纯函数：判定与解析。之所以从 loader 里拆出来，是因为 loader 其余部分依赖
 * LoadLibrary / CreatePseudoConsole，单测只能拿桩子跑；把「什么时候该加 0x8」
 * 这条会直接决定功能有没有效果的规则做成纯函数，才能在 make 的这一格里真验一遍。*/
int conpty_passthrough_wanted(int tri, unsigned long build) {
    if (tri < 0) return 0;                 /* off：什么都不试 */
    if (tri > 0) return 1;                 /* on：用户明确要，带位试；建不出再由上层退回 */
    return build >= TERMUX_PASSTHROUGH_MIN_BUILD;   /* auto：只在认这个位的系统上试 */
}

/* ASCII 大小写不敏感比较。不用 _stricmp：那个是 MSVC 拼法，本文件还要在
 * tests/loaderstub 的替身下编译（make verify-loader 那一格就跑在 Linux 上）。*/
static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int conpty_passthrough_tri_from_text(const char *s) {
    if (!s) return 0;
    if (ieq(s, "on") || ieq(s, "true") || ieq(s, "1") ||
        ieq(s, "yes") || ieq(s, "always"))
        return 1;
    if (ieq(s, "off") || ieq(s, "false") || ieq(s, "0") ||
        ieq(s, "no") || ieq(s, "never"))
        return -1;
    return 0;                              /* auto / 空 / 认不出 */
}

void conpty_set_passthrough(int tri) {
    g_pas_tri = tri < 0 ? -1 : (tri > 0 ? 1 : 0);
    g_pas_from_ini = 1;
}

int conpty_passthrough_state(void) { return g_pas_effective; }

/* in-box ConPTY 的 0x8 只在 Win11 22H2+ 存在。版本号必须走 ntdll!RtlGetVersion：
 * GetVersionEx 会被应用清单谎报成 6.2，VerifyVersionInfo 又要写 manifest —— 这两个
 * 都是这一带老坑，别再用。拿不到就报 0（= auto 不加位，行为与上一版一致）。*/
static unsigned long os_build(void) {
    /* 自己声明结构体，不用 OSVERSIONINFOW：本文件还要能在 tests/loaderstub 的替身
     * 头文件下编译（那一格只替了 4 个 Win32 调用，没有这个 typedef）。布局与
     * RTL_OSVERSIONINFOW 完全一致，RtlGetVersion 两个都收。 */
    struct termux_osver {
        DWORD dwOSVersionInfoSize;
        DWORD dwMajorVersion;
        DWORD dwMinorVersion;
        DWORD dwBuildNumber;
        DWORD dwPlatformId;
        WCHAR szCSDVersion[128];
    };
    typedef long (WINAPI *fn_rtlgetver)(struct termux_osver *);
    if (!g_os_build_read) {
        struct termux_osver vi;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        fn_rtlgetver f = ntdll ? (fn_rtlgetver)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        g_os_build_read = 1;
        g_os_build = 0;
        if (f) {
            memset(&vi, 0, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (f(&vi) == 0) g_os_build = (unsigned long)vi.dwBuildNumber;
        }
    }
    return g_os_build;
}

/* 读一个整数环境变量，支持 0x 前缀。返回 1 表示读到了。 */
static int env_int(const char *name, DWORD *out)
{
    const char *v = getenv(name);
    char *end = NULL;
    unsigned long n;
    if (!v || !*v)
        return 0;
    n = strtoul(v, &end, 0);
    if (end == v)
        return 0;
    *out = (DWORD)n;
    return 1;
}

/* exe 同目录下的 conpty.dll 全路径。找不到 exe 路径时留空。 */
static void module_dir_dll(WCHAR *buf, DWORD cap)
{
    WCHAR path[MAX_PATH];
    DWORD n;
    WCHAR *slash;
    buf[0] = 0;
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    slash[1] = 0;
    if (wcslen(path) + wcslen(L"conpty.dll") >= cap)
        return;
    wcscpy(buf, path);
    wcscat(buf, L"conpty.dll");
}

/* 尝试从一个模块里取三个符号；三个都取到才算成功。 */
static int bind_symbols(HMODULE h)
{
    fn_create c = (fn_create)(void *)GetProcAddress(h, "CreatePseudoConsole");
    fn_resize r = (fn_resize)(void *)GetProcAddress(h, "ResizePseudoConsole");
    fn_close x = (fn_close)(void *)GetProcAddress(h, "ClosePseudoConsole");
    if (!c || !r || !x)
        return 0;
    p_create = c;
    p_resize = r;
    p_close = x;
    return 1;
}

static void log_source(void)
{
    FILE *f;
    if (!getenv("TERMUX_DUMP"))
        return;
    f = fopen("conpty_source.log", "a");
    if (!f)
        return;
    fprintf(f, "[conpty] source=%s bundled=%d flags=0x%lx pas=%d tri=%d build=%lu\n",
            g_source, g_bundled, (unsigned long)g_flags,
            g_pas_effective, g_pas_tri, os_build());
    fclose(f);
}

int conpty_loader_init(void)
{
    const char *mode;
    WCHAR dllpath[MAX_PATH];
    HMODULE h;

    if (g_init_done)
        return g_ok;
    g_init_done = 1;

    /* 编译期默认值：Plan A 的二进制定义 TERMUX_CONPTY_DEFAULT_DLL。 */
#ifdef TERMUX_CONPTY_DEFAULT_DLL
    mode = "auto";
#else
    mode = "system";
#endif
    {
        const char *v = getenv("TERMUX_CONPTY");
        if (v && *v)
            mode = v;
    }

    if (!g_flags_set) {
        DWORD f = 0;
        if (env_int("TERMUX_CONPTY_FLAGS", &f)) {
            g_flags = f;
            g_flags_set = 1;
        }
    }
    /* 直通开关的环境变量版：现场实验用（同一个二进制换来换去比改 ini 快）。
     * main 在读完 ini 之后会调 conpty_set_passthrough() 并置 g_pas_from_ini，
     * 那时 ini 优先 —— 顺序是「先读 ini、后开第一个窗格」，见 main.c。 */
    if (!g_pas_from_ini) {
        const char *v = getenv("TERMUX_CONPTY_PASSTHROUGH");
        if (v && *v) g_pas_tri = conpty_passthrough_tri_from_text(v);
    }

    if (strcmp(mode, "system") != 0) {
        module_dir_dll(dllpath, MAX_PATH);
        h = dllpath[0] ? LoadLibraryW(dllpath) : NULL;
        if (!h)
            h = LoadLibraryW(L"conpty.dll");
        if (h && bind_symbols(h)) {
            g_dll = h;
            g_bundled = 1;
            g_ok = 1;
            g_source = "conpty.dll";
            log_source();
            return 1;
        }
        if (h)
            FreeLibrary(h);
        if (strcmp(mode, "dll") == 0) {
            /* 明确要求 bundled 却没拿到：不静默降级，让上层创建 pane 失败，
             * 这样实验里不会误把 kernel32 的结果当成 bundled 的结果。 */
            g_ok = 0;
            g_source = "conpty.dll(missing)";
            log_source();
            return 0;
        }
    }

    h = GetModuleHandleW(L"kernel32.dll");
    if (h && bind_symbols(h)) {
        g_ok = 1;
        g_source = "kernel32";
    } else {
        g_ok = 0;
        g_source = "none";
    }
    log_source();
    return g_ok;
}

DWORD conpty_default_flags(void)
{
    conpty_loader_init();
    return g_flags;
}

const char *conpty_source_name(void)
{
    conpty_loader_init();
    return g_source;
}

int conpty_is_bundled(void)
{
    conpty_loader_init();
    return g_bundled;
}

HRESULT conpty_create(COORD size, HANDLE hInput, HANDLE hOutput,
                      DWORD dwFlags, HPCON *phPC)
{
    if (!conpty_loader_init() || !p_create)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    /* v2.3.6：图形协议直通。不加 PSEUDOCONSOLE_PASSTHROUGH_MODE，in-box conhost 会
     * 自己把 sixel 的 DCS 与 kitty 的 APC 吃掉（微软那边是 issue #1173，直到今天
     * 没放行），我们转发得再干净也到不了宿主终端。老系统带着这个位调
     * CreatePseudoConsole 会直接失败 ⇒ 先带位试一次，失败立刻退回不带位：
     * 直通是锦上添花，把窗格开不出来是事故。 */
    if (conpty_passthrough_wanted(g_pas_tri, os_build()) &&
        !(dwFlags & TERMUX_PSEUDOCONSOLE_PASSTHROUGH) && p_create) {
        HRESULT hr = p_create(size, hInput, hOutput,
                              dwFlags | TERMUX_PSEUDOCONSOLE_PASSTHROUGH, phPC);
        if (SUCCEEDED(hr)) {
            g_pas_effective = 1;
            log_source();
            return hr;
        }
        g_pas_effective = 0;
    }
    return p_create(size, hInput, hOutput, dwFlags, phPC);
}

HRESULT conpty_resize(HPCON hPC, COORD size)
{
    if (!conpty_loader_init() || !p_resize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return p_resize(hPC, size);
}

void conpty_close(HPCON hPC)
{
    if (!hPC)
        return;
    if (!conpty_loader_init() || !p_close)
        return;
    p_close(hPC);
}
