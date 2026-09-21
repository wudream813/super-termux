#include "conpty_loader.h"

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
    fprintf(f, "[conpty] source=%s bundled=%d flags=0x%lx\n",
            g_source, g_bundled, (unsigned long)g_flags);
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
