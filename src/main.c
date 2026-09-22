#include "common.h"
#ifdef _WIN32
#include "conpty_loader.h"
#endif
#include "platform.h"
#include "types.h"
#include "screen.h"
#include "utf8.h"
#include "vt.h"
#include "config.h"
#include "pane.h"
#include "render.h"
#include "input.h"
#include "split.h"


void host_write(const char *s, int len) {
    while (len > 0) {
        DWORD written = 0;
        DWORD chunk = (DWORD)len;
        if (!WriteConsoleA(g_mux.hOut, s, chunk, &written, NULL) || written == 0) break;
        s += written;
        len -= (int)written;
    }
}

static void host_printf(const char *fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        host_write(buf, len);
    }
}

static void handle_resize(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_mux.hOut, &csbi)) return;
    int nc = csbi.srWindow.Right - csbi.srWindow.Left + 1, nt = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (nc < 4) nc = 4;
    if (nt < 2) nt = 2;
    int nr = nt - 1;
    if (nc == g_mux.host_cols && nt == g_mux.total_host_rows) return;

    EnterCriticalSection(&g_mux.cs);
    g_mux.host_cols = nc; g_mux.total_host_rows = nt; g_mux.host_rows = nr;
    /* 分屏状态下各 pane 的尺寸由分屏矩形决定（渲染帧里 pane_resize_to 会按新
     * 布局统一调整 screen + ConPTY），这里只重置 detect 计数；非分屏 pane 仍按
     * 整屏尺寸 resize。 */
    int pane_cols = nc;
    int split_now = split_is_split();
    for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) {
        if (!split_now) {
            screen_resize(&g_mux.panes[i].screen, pane_cols, nr);
            if (g_mux.panes[i].scroll_offset > g_mux.panes[i].screen.hist_lines)
                g_mux.panes[i].scroll_offset = g_mux.panes[i].screen.hist_lines;
        }
        g_mux.panes[i].screen.detect_count = 0;
    }
    LeaveCriticalSection(&g_mux.cs);

    for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) {
        if (!split_now && g_mux.panes[i].hpc &&
            (g_mux.panes[i].conpty_cols != pane_cols || g_mux.panes[i].conpty_rows != nr)) {
            COORD sz = {(SHORT)pane_cols, (SHORT)nr};
            conpty_resize(g_mux.panes[i].hpc, sz);
            g_mux.panes[i].conpty_cols = pane_cols;
            g_mux.panes[i].conpty_rows = nr;
        }
    }
    g_mux.needs_redraw = 1;
}

static void handle_input(void) {
    INPUT_RECORD rec[128]; DWORD cnt;
    ULONGLONG last_render = 0;
    while (g_mux.running) {
        DWORD wait_ms = g_mux.needs_redraw ? 8 : 25;
        int has_input = (WaitForSingleObject(g_mux.hIn, wait_ms) == WAIT_OBJECT_0 && ReadConsoleInputW(g_mux.hIn, rec, 128, &cnt));
        if (has_input) {
            for (DWORD i = 0; i < cnt; i++) {
                if (rec[i].EventType == KEY_EVENT) handle_key(&rec[i].Event.KeyEvent);
                else if (rec[i].EventType == MOUSE_EVENT) handle_mouse(&rec[i].Event.MouseEvent);
                else if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT) handle_resize();
            }
        }

        // toast 到期自动消失：到点触发一次重绘让 toast 行被清掉。
        if (g_toast_until && GetTickCount64() >= g_toast_until) {
            g_toast_until = 0;
            g_toast_msg[0] = 0;
            g_mux.needs_redraw = 1;
        }

        // v1.1.5: hover preview 1.5s timer check
        if (g_hover_preview_pane >= 0 && !g_hover_preview_active) {
            if (GetTickCount64() - g_hover_preview_start >= 1500) {
                g_hover_preview_active = 1;
                g_mux.needs_redraw = 1;
            }
        }

        // v1.2.8: chooser item hover preview 1.0s timer check
        if (g_hover_chooser_idx >= 0 && !g_hover_chooser_active) {
            if (GetTickCount64() - g_hover_chooser_start >= 1000) {
                g_hover_chooser_active = 1;
                g_mux.needs_redraw = 1;
            }
        }

        // v1.2.7: settings name hover preview 1.0s timer check
        if (g_hover_settings_name_idx >= 0 && !g_hover_settings_name_active) {
            if (GetTickCount64() - g_hover_settings_name_start >= 1000) {
                g_hover_settings_name_active = 1;
                g_mux.needs_redraw = 1;
            }
        }

        // v1.2.4: settings cmd hover preview 1.0s timer check
        if (g_hover_settings_cmd_idx >= 0 && !g_hover_settings_cmd_active) {
            if (GetTickCount64() - g_hover_settings_cmd_start >= 1000) {
                g_hover_settings_cmd_active = 1;
                g_mux.needs_redraw = 1;
            }
        }

        reap_dead_panes();
        if (g_mux.active_pane < 0 || !g_mux.panes[g_mux.active_pane].active) {
            int f = -1;
            for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active) { f = i; break; }
            if (f >= 0) g_mux.active_pane = f; else { g_mux.running = 0; break; }
        }
        if (g_mux.needs_redraw) {
            ULONGLONG now = GetTickCount64();
            if (has_input || (now - last_render >= 12)) {
                /* 搜索开着时，新到的终端输出要先重算匹配再画，否则新打印出来的
                 * 内容里的关键词永远不会被高亮（2026-09-20 用户要求）。放在这里
                 * 而不是 pane 读线程里：每帧最多重扫一次。 */
                search_refresh_live();
                render_screen();
                last_render = now;
            }
        }
    }
}

static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    InterlockedExchange(&g_mux.running, 0);
    return TRUE;
}

/* bug #30 用：如果启动时另开了 CONOUT$ / CONIN$，退出时要自己关掉，
 * 而 GetStdHandle 拿来的那两个【不能】关（不属于我们）。 */
static HANDLE g_hout_owned = NULL;
static HANDLE g_hin_owned  = NULL;
static DWORD  g_hout_err = 0;          /* 第一次 GetConsoleScreenBufferInfo 的 GetLastError */
static DWORD  g_hin_err  = 0;          /* GetConsoleMode(hIn) 的 GetLastError */
static BOOL   g_setmode_in_ok = FALSE;
static BOOL   g_setmode_out_ok = FALSE;


int main(void) {
    memset(&g_mux, 0, sizeof(g_mux));
    InitializeCriticalSection(&g_mux.cs);
    /* 提前读，好让启动路径上的 dump_mark 打点生效（原来在 console 查询之后才赋值）。 */
    g_dump_enabled = getenv("TERMUX_DUMP") != NULL;

    g_mux.hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_mux.hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (g_mux.hOut == INVALID_HANDLE_VALUE || g_mux.hIn == INVALID_HANDLE_VALUE ||
        g_mux.hOut == NULL || g_mux.hIn == NULL) {
        fprintf(stderr, "termux: no console attached (run from a console window)\n");
        DeleteCriticalSection(&g_mux.cs);
        return 1;
    }
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_mux.hOut, &csbi)) {
        /* ★ bug #30：STD_OUTPUT_HANDLE 可能是【只写】的，而
         * GetConsoleScreenBufferInfo 要求句柄带 GENERIC_READ —— 只写句柄会让它
         * 返回 0（GetLastError = ERROR_INVALID_HANDLE，6）。
         *
         * 在 ConPTY 下这是【必然】的：子进程拿到的标准 I/O 是 ConDrv 上通用的
         * "Input"/"Output" 句柄，而不是 CONIN$/CONOUT$。于是 termux 在启动
         * 第一步就 "cannot query console buffer" 退出（2026-09-22 由 CI 的
         * ConPTY 冒烟测试首次暴露：裸跑 stderr 正好是这句话，退出码 1）。
         *
         * 解法是另开一个 CONOUT$（读写都有）并改用它。同一个句柄也供
         * host_write / SetConsoleMode 使用 —— 写 CONOUT$ 会落到进程所属的控制台
         * 会话，在 ConPTY 下那个会话就是伪控制台，所以输出照样回到管道里。
         * 这一改同时修好另外两处同样用 g_mux.hOut 查询的地方：
         *   src/main.c:43（resize 时静默 return，等于 resize 检测失效）
         *   src/platform_win.c:96（plat_console_size 直接返回 -1）
         *
         * 普通控制台窗口下 STD_OUTPUT_HANDLE 本来就可读，这段分支【不会】进入，
         * 所以现有 Windows 行为完全不变。 */
        g_hout_err = GetLastError();   /* 必须在 CreateFileW 之前记，否则会被覆盖 */
        dump_mark("[boot] console-query-failed err=%lu -> 尝试 CONOUT$",
                  (unsigned long)g_hout_err);
        HANDLE hCo = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        if (hCo != INVALID_HANDLE_VALUE) {
            g_mux.hOut = hCo;
            g_hout_owned = hCo;
        }
        if (!GetConsoleScreenBufferInfo(g_mux.hOut, &csbi)) {
            /* 带上 GetLastError，免得下次又要多花一轮 CI 才知道卡在哪。 */
            fprintf(stderr, "termux: cannot query console buffer (err=%lu)\n",
                    (unsigned long)GetLastError());
            if (g_hout_owned) { CloseHandle(g_hout_owned); g_hout_owned = NULL; }
            DeleteCriticalSection(&g_mux.cs);
            return 1;
        }
    }
    dump_mark("[boot] console-ok host=%dx%d conout_fallback=%d stdout_err=%lu",
              (int)(csbi.srWindow.Right - csbi.srWindow.Left + 1),
              (int)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1),
              g_hout_owned != NULL, (unsigned long)g_hout_err);
    g_mux.host_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    g_mux.total_host_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    g_mux.host_rows = g_mux.total_host_rows - 1;
    if (g_mux.host_cols < 4) g_mux.host_cols = 4;
    if (g_mux.total_host_rows < 2) g_mux.total_host_rows = 2;
    g_mux.host_rows = g_mux.total_host_rows - 1;

    GetConsoleMode(g_mux.hIn, &g_mux.orig_in_mode);
    GetConsoleMode(g_mux.hOut, &g_mux.orig_out_mode);
    GetConsoleTitleW(g_orig_title, 255);
    /* ★ bug #30（输入侧）：和输出侧同一个病根。
     * ConPTY 下 STD_INPUT_HANDLE 是 ConDrv 上通用的 "Input" 句柄，
     * GetConsoleMode / SetConsoleMode 在它上面都会失败。
     *
     * 我第一版写的兜底用 `!SetConsoleMode(hIn, orig_in_mode)` 当探针，
     * 这是错的：orig_in_mode 本身就是刚才 GetConsoleMode 失败后留下的 0，
     * 拿 0 去 SetConsoleMode 只会再失败一次，兜底根本触发不了。
     * CI 第十轮的打点把这件事钉死了：
     *     [boot] setmode in=0 out=1 in_mode=0x98
     *     [boot] input-loop-exited          ← 输入循环当场退出，进程 exit 0
     * 也就是「渲染全对、4132 字节输出都收到了，但一个按键都进不去」。
     *
     * 正确做法和输出侧对称：GetConsoleMode 失败就改开 CONIN$。 */
    if (!GetConsoleMode(g_mux.hIn, &g_mux.orig_in_mode)) {
        g_hin_err = GetLastError();
        dump_mark("[boot] in-mode-failed err=%lu -> 尝试 CONIN$",
                  (unsigned long)g_hin_err);
        HANDLE hCi = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        if (hCi != INVALID_HANDLE_VALUE) {
            g_mux.hIn = hCi;
            g_hin_owned = hCi;
            GetConsoleMode(g_mux.hIn, &g_mux.orig_in_mode);
        }
    }
    DWORD im = g_mux.orig_in_mode;
    im &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    im |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
    g_setmode_in_ok = SetConsoleMode(g_mux.hIn, im);
    DWORD om = g_mux.orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | DISABLE_NEWLINE_AUTO_RETURN;
    g_setmode_out_ok = SetConsoleMode(g_mux.hOut, om);
    g_mux.orig_cp = GetConsoleOutputCP();
    g_mux.orig_input_cp = GetConsoleCP();
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    dump_mark("[boot] setmode in=%d out=%d in_mode=0x%lx",
              (int)g_setmode_in_ok, (int)g_setmode_out_ok, (unsigned long)im);
    if (g_dump_enabled) {
        FILE *f = fopen("mouse_dump.log", "ab");
        if (f) {
            fprintf(f, "[v8.54] startup host=%dx%d\n", g_mux.host_cols, g_mux.host_rows);
            fclose(f);
        }
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    load_config();
    dump_mark("[boot] config-loaded mouse=%d default_startup=%d",
              (int)g_mouse_enabled, (int)g_default_startup);

    /* mouse = false 时既不申请控制台鼠标事件，也不打开 VT 鼠标追踪 */
    if (!g_mouse_enabled) SetConsoleMode(g_mux.hIn, im & ~(DWORD)ENABLE_MOUSE_INPUT);
    if (g_mouse_enabled)
        host_printf("\x1b[?1049h\x1b[?1003h\x1b[?1006h\x1b[2J\x1b[H\x1b[?25l");
    else
        host_printf("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l");
    g_mux.running = 1;
    split_reset();
    int first = create_pane();
    if (first < 0) {
        host_printf("\x1b[31mFailed! Need Win10 1809+ and enough memory\x1b[0m\r\n");
        Sleep(3000);
        goto cleanup;
    }
    dump_mark("[boot] pane-created id=%d", first);
    split_init_tab(first);
    g_mux.active_pane = first;
    if (g_default_startup == 1) {
        g_mux.help_mode = 1;
    }
    g_mux.needs_redraw = 1;
    render_screen();
    dump_mark("[boot] first-render-done help_mode=%d", (int)g_mux.help_mode);
    handle_input();
    dump_mark("[boot] input-loop-exited");
    for (int i = 0; i < g_mux.pane_count; i++) close_pane(i);

cleanup:
    host_printf("\x1b[?1003l\x1b[?1006l\x1b[?1049l\x1b[?25h\x1b[0m");
    if (g_orig_title[0]) {
        SetConsoleTitleW(g_orig_title);
        char tbuf[512];
        int tl = WideCharToMultiByte(CP_UTF8, 0, g_orig_title, -1, tbuf, sizeof(tbuf), NULL, NULL);
        if (tl > 0) host_printf("\x1b]0;%s\x07", tbuf);
    }
    SetConsoleCtrlHandler(ctrl_handler, FALSE);
    SetConsoleMode(g_mux.hIn, g_mux.orig_in_mode);
    SetConsoleMode(g_mux.hOut, g_mux.orig_out_mode);
    SetConsoleOutputCP(g_mux.orig_cp);
    SetConsoleCP(g_mux.orig_input_cp);
    render_cleanup();
    if (g_hout_owned) { CloseHandle(g_hout_owned); g_hout_owned = NULL; }
    if (g_hin_owned)  { CloseHandle(g_hin_owned);  g_hin_owned  = NULL; }
    DeleteCriticalSection(&g_mux.cs);
    printf("Bye!\n");
    return 0;
}
