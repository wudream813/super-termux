/* ---------------------------------------------------------------------------
 * platform_win.c —— platform.h 的 Windows 实现。
 *
 * 这些函数体就是 pane.c / main.c 里原本直接写的 Win32 调用，原样搬过来，
 * 行为不变。搬的目的只是让 pane.c 变成平台无关（见 include/platform.h）。
 * ------------------------------------------------------------------------- */
#include "platform.h"
#include "types.h"
#include "pane.h"
#include "conpty_loader.h"
#include <process.h>
#include <shellapi.h>

int plat_proc_spawn(const char *cmd_utf8, const char *workdir_utf8,
                    int cols, int rows, HANDLE *out_in, HANDLE *out_out,
                    HANDLE *out_proc) {
    /* Windows 的进程创建要挂 ConPTY 的 ProcThreadAttributeList，跟 STARTUPINFOEX
     * 缠在一起，拆不出来；真正的实现在 pane.c 的 create_pane_shell_with_dir()
     * 里（#ifdef _WIN32 分支）。这里只服务 POSIX 侧的调用约定，不会被调到。 */
    (void)cmd_utf8; (void)workdir_utf8; (void)cols; (void)rows;
    (void)out_in; (void)out_out; (void)out_proc;
    return -1;
}

void plat_proc_resize(HANDLE proc, HANDLE in_fd, HANDLE out_fd, int cols, int rows) {
    (void)proc; (void)in_fd; (void)out_fd; (void)cols; (void)rows;
    /* ConPTY 的 resize 需要 HPCON，而 HPCON 挂在 Pane 上、由 pane.c 直接调
     * conpty_resize()（见 pane_resize_to）。这里保持空实现以免两处重复下发。 */
}

int plat_proc_exited(HANDLE proc, DWORD *exit_code) {
    if (proc == NULL_HANDLE) return 0;
    if (WaitForSingleObject((HANDLE)proc, 0) != WAIT_OBJECT_0) return 0;
    DWORD ec = 0;
    GetExitCodeProcess((HANDLE)proc, &ec);
    if (exit_code) *exit_code = ec;
    return 1;
}

void plat_proc_kill(HANDLE proc) {
    if (proc == NULL_HANDLE) return;
    TerminateProcess((HANDLE)proc, 0);
    WaitForSingleObject((HANDLE)proc, 500);
}

void plat_proc_close(HANDLE *proc, HANDLE *in_fd, HANDLE *out_fd) {
    if (in_fd && *in_fd != NULL_HANDLE)  { CloseHandle((HANDLE)*in_fd);  *in_fd  = NULL_HANDLE; }
    if (out_fd && *out_fd != NULL_HANDLE) { CloseHandle((HANDLE)*out_fd); *out_fd = NULL_HANDLE; }
    if (proc && *proc != NULL_HANDLE) {
        TerminateProcess((HANDLE)*proc, 0);
        WaitForSingleObject((HANDLE)*proc, 500);
        CloseHandle((HANDLE)*proc);
        *proc = NULL_HANDLE;
    }
}

HANDLE plat_thread_start(plat_thread_fn fn, void *arg) {
    return (HANDLE)(intptr_t)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
}

void plat_thread_join(HANDLE *th, unsigned wait_ms) {
    if (!th || *th == NULL_HANDLE) return;
    WaitForSingleObject((HANDLE)*th, wait_ms);
    CloseHandle((HANDLE)*th);
    *th = NULL_HANDLE;
}

int plat_read(HANDLE h, char *buf, int cap) {
    DWORD br = 0;
    if (h == NULL_HANDLE || cap <= 0) return -1;
    if (!ReadFile((HANDLE)h, buf, (DWORD)cap, &br, NULL)) return -1;
    return (int)br;
}

int plat_write_fd(HANDLE h, const char *buf, int len) {
    if (h == NULL_HANDLE || len <= 0) return 0;
    DWORD w = 0;
    if (!WriteFile((HANDLE)h, buf, (DWORD)len, &w, NULL)) return -1;
    return (int)w;
}

int plat_peek_avail(HANDLE h) {
    DWORD avail = 0;
    if (h == NULL_HANDLE) return 0;
    if (!PeekNamedPipe((HANDLE)h, NULL, 0, NULL, &avail, NULL)) return 0;
    return (int)avail;
}

/* 控制台 / 剪贴板 / 系统信息：Windows 侧仍由 main.c 与 pane.c 里的原代码负责
 * （ReadConsoleInputW 主循环、Win32 Clipboard、注册表读版本），不走 platform.h。
 * 这里给出满足链接的等价实现，供将来统一时替换。 */
int  plat_console_init(int want_mouse) { (void)want_mouse; return 0; }
void plat_console_shutdown(void) {}
int  plat_console_size(int *cols, int *total_rows) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_mux.hOut, &csbi)) return -1;
    if (cols) *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    if (total_rows) *total_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 0;
}
int  plat_console_read(INPUT_RECORD *out, int cap, int timeout_ms) {
    (void)out; (void)cap; (void)timeout_ms; return 0;
}
void plat_clip_copy(const char *utf8, int len) { (void)utf8; (void)len; }
void plat_sysinfo(char *out, int cap) {
    get_system_version_string(out, cap);
}
const WCHAR *plat_default_shell(void) { return L"cmd.exe"; }
const WCHAR *plat_user_home(void) { return _wgetenv(L"USERPROFILE"); }
