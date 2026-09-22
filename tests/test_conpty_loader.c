/* tests/test_conpty_loader.c —— src/conpty_loader.c 的决策逻辑单元测试。
 *
 * 每个进程只测一个配置（conpty_loader_init 内部缓存结果），用法：
 *   test_conpty_loader <case>
 * 退出码 0 = 该 case 通过。由 tests/verify_conpty_loader.sh 逐个驱动。
 *
 * 编译（见 verify_conpty_loader.sh）：
 *   gcc -Wall -Wextra -Werror -Itests/loaderstub -Iinclude \
 *       tests/test_conpty_loader.c src/conpty_loader.c -o /tmp/tcl
 */
#include "conpty_loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ★ setenv 是 POSIX 的，MinGW 没有（只有 putenv / _putenv_s），编译会报
 *   error: implicit declaration of function 'setenv'
 * —— CI 的 windows 作业第二次跑就是挂在这里（2026-09-22）。
 * _putenv_s 在 MinGW 的 <stdlib.h> 里，实测 -Wall -Wextra -Werror 能过。
 * 本文件 13 处 setenv 的第三个参数【全是 1】（覆盖），而 _putenv_s 恒为覆盖，
 * 语义完全一致，所以这个映射是安全的。 */
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

/* ---- 测试钩子 ---- */
int stub_dll_present = 1;
int stub_symbols_complete = 1;
int stub_kernel32_ok = 1;
int stub_free_called = 0;

/* 记录 LoadLibraryW 收到的名字，用来验证「先试 exe 同目录」这一步真的发生了。 */
static wchar_t g_last_load[260];
static int g_load_calls = 0;
static int g_first_was_abs = 0;

#define FAKE_DLL ((HMODULE)(void *)0xD11)
#define FAKE_K32 ((HMODULE)(void *)0x32)

HMODULE LoadLibraryW(const WCHAR *name)
{
    g_load_calls++;
    wcsncpy(g_last_load, name, 259);
    g_last_load[259] = 0;
    if (g_load_calls == 1)
        g_first_was_abs = (wcsstr(name, L"\\") != NULL || wcsstr(name, L"/") != NULL);
    if (!stub_dll_present)
        return NULL;
    return FAKE_DLL;
}

BOOL FreeLibrary(HMODULE h)
{
    (void)h;
    stub_free_called++;
    return 1;
}

void *GetProcAddress(HMODULE h, const char *name)
{
    int ok;
    if (h == FAKE_DLL)
        ok = stub_symbols_complete;
    else
        ok = stub_kernel32_ok;
    if (!ok)
        return NULL;
    (void)name;
    return (void *)(void *)0xF00;
}

DWORD GetModuleFileNameW(void *module, WCHAR *buf, DWORD cap)
{
    const wchar_t *p = L"C:\\fake\\dir\\termux.exe";
    (void)module;
    if (cap < 24)
        return 0;
    wcscpy(buf, p);
    return (DWORD)wcslen(p);
}

HMODULE GetModuleHandleW(const WCHAR *name)
{
    (void)name;
    return FAKE_K32;   /* kernel32 永远在，符号齐不齐由 GetProcAddress 决定 */
}

/* ---- 被测符号的真实实现（不是替身）：CreatePseudoConsole 等只作为符号存在 ---- */
HRESULT CreatePseudoConsole(COORD s, HANDLE a, HANDLE b, DWORD f, HPCON *p)
{
    (void)s; (void)a; (void)b; (void)f; (void)p;
    return 0;
}
HRESULT ResizePseudoConsole(HPCON h, COORD s) { (void)h; (void)s; return 0; }
void ClosePseudoConsole(HPCON h) { (void)h; }

/* ---- 断言 ---- */
static int g_fail = 0;
static void expect_int(const char *what, long got, long want)
{
    if (got != want) {
        printf("  [FAIL] %s: 得到 %ld，期望 %ld\n", what, got, want);
        g_fail = 1;
    } else {
        printf("  [ok] %s = %ld\n", what, got);
    }
}
static void expect_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("  [FAIL] %s: 得到 \"%s\"，期望 \"%s\"\n", what, got, want);
        g_fail = 1;
    } else {
        printf("  [ok] %s = \"%s\"\n", what, got);
    }
}

int main(int argc, char **argv)
{
    const char *c = (argc > 1) ? argv[1] : "";

    if (strcmp(c, "system") == 0) {
        setenv("TERMUX_CONPTY", "system", 1);
        expect_int("init 成功", conpty_loader_init(), 1);
        expect_str("source", conpty_source_name(), "kernel32");
        expect_int("bundled", conpty_is_bundled(), 0);
    } else if (strcmp(c, "auto_dll") == 0) {
        setenv("TERMUX_CONPTY", "auto", 1);
        stub_dll_present = 1;
        expect_int("init 成功", conpty_loader_init(), 1);
        expect_str("source", conpty_source_name(), "conpty.dll");
        expect_int("bundled", conpty_is_bundled(), 1);
        expect_int("第一次 LoadLibraryW 用的是 exe 同目录绝对路径", g_first_was_abs, 1);
    } else if (strcmp(c, "auto_nodll") == 0) {
        setenv("TERMUX_CONPTY", "auto", 1);
        stub_dll_present = 0;
        expect_int("init 成功（回退 kernel32）", conpty_loader_init(), 1);
        expect_str("source", conpty_source_name(), "kernel32");
        expect_int("bundled", conpty_is_bundled(), 0);
    } else if (strcmp(c, "dll_only_missing") == 0) {
        setenv("TERMUX_CONPTY", "dll", 1);
        stub_dll_present = 0;
        expect_int("init 失败（不静默降级）", conpty_loader_init(), 0);
        expect_str("source", conpty_source_name(), "conpty.dll(missing)");
    } else if (strcmp(c, "dll_partial") == 0) {
        setenv("TERMUX_CONPTY", "auto", 1);
        stub_dll_present = 1;
        stub_symbols_complete = 0;
        expect_int("init 成功（符号不全视为失败并回退）", conpty_loader_init(), 1);
        expect_str("source", conpty_source_name(), "kernel32");
        expect_int("FreeLibrary 被调用", stub_free_called, 1);
    } else if (strcmp(c, "flags_hex") == 0) {
        setenv("TERMUX_CONPTY", "system", 1);
        setenv("TERMUX_CONPTY_FLAGS", "0x10", 1);
        conpty_loader_init();
        expect_int("flags", (long)conpty_default_flags(), 0x10);
    } else if (strcmp(c, "flags_dec") == 0) {
        setenv("TERMUX_CONPTY", "system", 1);
        setenv("TERMUX_CONPTY_FLAGS", "2", 1);
        conpty_loader_init();
        expect_int("flags", (long)conpty_default_flags(), 2);
    } else if (strcmp(c, "flags_bad") == 0) {
        setenv("TERMUX_CONPTY", "system", 1);
        setenv("TERMUX_CONPTY_FLAGS", "abc", 1);
        conpty_loader_init();
        expect_int("非法值退回默认 0", (long)conpty_default_flags(), 0);
    } else if (strcmp(c, "default") == 0) {
        /* 不设 TERMUX_CONPTY，看编译期默认。
         * -DTERMUX_CONPTY_DEFAULT_DLL => auto（有 dll 就 bundled）
         * 否则                        => system（kernel32） */
#ifdef TERMUX_CONPTY_DEFAULT_DLL
        expect_str("编译期默认 = auto/bundled", conpty_source_name(), "conpty.dll");
        expect_int("bundled", conpty_is_bundled(), 1);
#else
        expect_str("编译期默认 = system", conpty_source_name(), "kernel32");
        expect_int("bundled", conpty_is_bundled(), 0);
#endif
    } else if (strcmp(c, "idempotent") == 0) {
        setenv("TERMUX_CONPTY", "auto", 1);
        expect_int("第一次", conpty_loader_init(), 1);
        setenv("TERMUX_CONPTY", "system", 1); /* 应被忽略：已缓存 */
        expect_int("第二次", conpty_loader_init(), 1);
        expect_str("source 不被后来的环境变量改写", conpty_source_name(), "conpty.dll");
    } else {
        printf("未知 case: %s\n", c);
        return 2;
    }

    printf(g_fail ? "[FAIL] %s\n" : "[PASS] %s\n", c);
    return g_fail;
}
