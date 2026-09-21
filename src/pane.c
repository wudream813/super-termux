#include "pane.h"
#include "platform.h"
#ifdef _WIN32
#include "conpty_loader.h"
#endif
#include "render.h"
#include "input.h"
#include "split.h"

/* input.c 定义：分屏全屏缩放（zoom）标志。窗格死亡时若处于 zoom 需要清掉。 */
extern int g_split_zoom;

void write_to_pane_internal(Pane *pane, const char *data, int len) {
    if (!pane || !pane->active) return;
    plat_write_fd(pane->pipe_in, data, len);
}

void write_to_pane(const char *data, int len) {
    if (g_mux.active_pane < 0 || g_mux.active_pane >= g_mux.pane_count) return;
    write_to_pane_internal(&g_mux.panes[g_mux.active_pane], data, len);
}

void pane_mark_dead(int idx) {
    if (idx < 0 || idx >= MAX_PANES) return;
    EnterCriticalSection(&g_mux.cs);
    Pane *pane = &g_mux.panes[idx];
    if (!pane->active) { LeaveCriticalSection(&g_mux.cs); return; }
    /* 分屏窗格死亡：统一从它所属 tab 的分屏树里摘除（活动/非活动都处理）。
     *  - 在多叶子树里：树收缩，survivor 为存活兄弟；若关掉的是锚点 pane，
     *    split_remove_pane 会把兄弟提升为新锚点，标签页不丢。
     *  - 单叶子树（独立标签页唯一 pane）：返回 0，按普通整 tab 关闭处理。 */
    int split_survivor = -1;
    int was_split = split_remove_pane(idx, &split_survivor);
    (void)was_split;
    if (was_split && g_split_zoom) g_split_zoom = 0;   /* 摘除窗格后退出 zoom */
    pane->active = 0;
    int next = -1;
    for (int i = 0; i < g_mux.pane_count; i++) if (g_mux.panes[i].active && i != idx) { next = i; break; }
    if (idx == g_mux.active_pane) {
        if (split_survivor >= 0 && g_mux.panes[split_survivor].active) {
            g_mux.active_pane = split_survivor; g_mux.panes[split_survivor].scroll_offset = 0;
        } else if (next >= 0) { g_mux.active_pane = next; g_mux.panes[next].scroll_offset = 0; }
        else g_mux.running = 0;
    }
    g_mux.needs_redraw = 1;
    LeaveCriticalSection(&g_mux.cs);
}

void reap_dead_panes(void) {
    for (int i = 0; i < g_mux.pane_count; i++) {
        Pane *p = &g_mux.panes[i];
        if (!p->active) {
            if (p->read_thread != NULL_HANDLE) close_pane(i);
            continue;
        }
        if (p->exited_hold) {
            continue;
        }
        DWORD exit_code = 0;
        if (p->process != NULL_HANDLE && plat_proc_exited(p->process, &exit_code)) {
            if (exit_code != 0) {
                plat_thread_join(&p->read_thread, 250);
                char msg[256];
                int mlen = snprintf(msg, sizeof(msg),
                    "\r\n\x1b[31;1m[进程异常退出，退出码: %lu (0x%lX)]\x1b[0m \x1b[33m按任意键关闭该标签页...\x1b[0m\r\n",
                    (unsigned long)exit_code, (unsigned long)exit_code);
                EnterCriticalSection(&g_mux.cs);
                screen_process_output(&p->screen, msg, mlen);
                p->exited_hold = 1;
                p->exit_code = exit_code;
                g_mux.needs_redraw = 1;
                LeaveCriticalSection(&g_mux.cs);
                continue;
            }
            plat_thread_join(&p->read_thread, 250);
            pane_mark_dead(i);
            close_pane(i);
        }
    }
}

unsigned __stdcall pane_read_thread(void *arg) {
    int idx = (int)(intptr_t)arg;
    Pane *pane = &g_mux.panes[idx];
    char buf[READ_BUF_SIZE];
    while (pane->active) {
        int br = plat_read(pane->pipe_out, buf, (int)sizeof(buf));
        if (br <= 0) break;
        dump_pane_bytes(idx, buf, br);
        EnterCriticalSection(&g_mux.cs);
        /* ConPTY 整屏重绘（ESC[H 起逐行重画）按其自身滚动缓冲对齐：重绘顶行可能比
         * 本地 reflow 环的可见顶行深若干行，直接落下会把本地还可见的顶部内容覆盖吞行。
         * 对齐只接受【正向】偏移且要求整块逐行吻合（见 screen_repaint_align 头注）。
         * 重绘比窗格高时由 screen_process_output 内部的重绘视口上滚对齐，比窗格矮时
         * 由 screen_repaint_reanchor 把提示符顶回底行。 */
        screen_repaint_align(&pane->screen, buf, br);
        screen_process_output(&pane->screen, buf, (size_t)br);
        /* 搜索开着时，新输出里的关键词也要能被找到（重扫在主循环里做，每帧一次）。
         * 只有活动窗格的内容会被搜索，别的窗格来了输出不用置脏。 */
        if (idx == g_mux.active_pane) search_mark_dirty();
        int avail;
        while ((avail = plat_peek_avail(pane->pipe_out)) > 0) {
            int br2 = plat_read(pane->pipe_out, buf,
                                avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail);
            if (br2 <= 0) break;
            dump_pane_bytes(idx, buf, br2);
            screen_repaint_align(&pane->screen, buf, br2);
            screen_process_output(&pane->screen, buf, (size_t)br2);
            if (idx == g_mux.active_pane) search_mark_dirty();
        }
        if (pane->screen.response_len > 0) {
            write_to_pane_internal(pane, pane->screen.response_buf, pane->screen.response_len);
            pane->screen.response_len = 0;
        }
        if (idx == g_mux.active_pane) g_mux.needs_redraw = 1;
        LeaveCriticalSection(&g_mux.cs);
    }
    pane_mark_dead(idx);
    return 0;
}

#ifdef _WIN32   /* 注册表只有 Windows 有；POSIX 侧走 plat_sysinfo(uname)。 */
typedef LSTATUS (APIENTRY *RegOpenKeyExW_fn)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS (APIENTRY *RegQueryValueExW_fn)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS (APIENTRY *RegCloseKey_fn)(HKEY);
#endif

void get_system_version_string(char *out, int max_len) {
    out[0] = 0;
#ifndef _WIN32
    plat_sysinfo(out, max_len);
    if (!out[0]) snprintf(out, (size_t)max_len, "%s", "POSIX");
#else
    HMODULE hAdv = LoadLibraryA("advapi32.dll");
    if (hAdv) {
        RegOpenKeyExW_fn pOpen = (RegOpenKeyExW_fn)(void*)GetProcAddress(hAdv, "RegOpenKeyExW");
        RegQueryValueExW_fn pQuery = (RegQueryValueExW_fn)(void*)GetProcAddress(hAdv, "RegQueryValueExW");
        RegCloseKey_fn pClose = (RegCloseKey_fn)(void*)GetProcAddress(hAdv, "RegCloseKey");
        if (pOpen && pQuery && pClose) {
            HKEY hKey;
            if (pOpen(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                WCHAR prod_name[128] = {0};
                WCHAR display_ver[64] = {0};
                WCHAR current_build[64] = {0};
                DWORD ubr = 0;
                DWORD size = sizeof(prod_name);
                pQuery(hKey, L"ProductName", NULL, NULL, (LPBYTE)prod_name, &size);
                size = sizeof(display_ver);
                pQuery(hKey, L"DisplayVersion", NULL, NULL, (LPBYTE)display_ver, &size);
                if (!display_ver[0]) {
                    size = sizeof(display_ver);
                    pQuery(hKey, L"ReleaseId", NULL, NULL, (LPBYTE)display_ver, &size);
                }
                size = sizeof(current_build);
                pQuery(hKey, L"CurrentBuild", NULL, NULL, (LPBYTE)current_build, &size);
                size = sizeof(ubr);
                pQuery(hKey, L"UBR", NULL, NULL, (LPBYTE)&ubr, &size);
                pClose(hKey);

                char u8_prod[128] = {0}, u8_disp[64] = {0}, u8_build[64] = {0};
                WideCharToMultiByte(CP_UTF8, 0, prod_name, -1, u8_prod, sizeof(u8_prod) - 1, NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, display_ver, -1, u8_disp, sizeof(u8_disp) - 1, NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, current_build, -1, u8_build, sizeof(u8_build) - 1, NULL, NULL);

                if (u8_prod[0] && u8_build[0]) {
                    if (ubr > 0 && u8_disp[0]) {
                        snprintf(out, max_len, "%s %s (Build %s.%lu)", u8_prod, u8_disp, u8_build, (unsigned long)ubr);
                    } else if (u8_disp[0]) {
                        snprintf(out, max_len, "%s %s (Build %s)", u8_prod, u8_disp, u8_build);
                    } else if (ubr > 0) {
                        snprintf(out, max_len, "%s (Build %s.%lu)", u8_prod, u8_build, (unsigned long)ubr);
                    } else {
                        snprintf(out, max_len, "%s (Build %s)", u8_prod, u8_build);
                    }
                    FreeLibrary(hAdv);
                    return;
                }
            }
        }
        FreeLibrary(hAdv);
    }
    snprintf(out, (size_t)max_len, "%s", "Windows 10 / Windows 11 (NT 10.0)");
#endif
}

int create_about_pane(void) {
    int idx = -1;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!g_mux.panes[i].active && g_mux.panes[i].read_thread == NULL_HANDLE) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    Pane *pane = &g_mux.panes[idx];
    memset(pane, 0, sizeof(*pane));
    int pane_cols = g_mux.host_cols;
    if (!screen_init(&pane->screen, pane_cols, g_mux.host_rows)) return -1;
    pane->screen.pane_index = idx;
    pane->screen.in_alt_screen = 1;
    pane->active = 1;
    pane->is_about = 1;
    pane->color = 7;
    snprintf(pane->title, sizeof(pane->title), "关于");
    snprintf(pane->full_title, sizeof(pane->full_title), "关于 termux (About)");

    if (idx >= g_mux.pane_count) g_mux.pane_count = idx + 1;

    char sys_ver[128] = {0};
    get_system_version_string(sys_ver, sizeof(sys_ver));
    /* v1.8.7: 关闭键跟随实际键位配置，不再写死 Ctrl+B x。 */
    char close_key[48] = {0};
    keymap_describe(ACT_CLOSE_PANE, close_key, sizeof(close_key));
    if (!close_key[0]) snprintf(close_key, sizeof(close_key), "%s", "关闭标签页快捷键");

    char about_buf[2048];
    int len = snprintf(about_buf, sizeof(about_buf),
        "\x1b[?1049h\x1b[?25l\r\n"
        "  \x1b[038;2;255;255;255m\x1b[048;2;217;119;054;1m ╔══════════════════════════════════════════════════════════╗ \x1b[0m\r\n"
        "  \x1b[038;2;255;255;255m\x1b[048;2;217;119;054;1m ║                  termux - 关于 (About)                   ║ \x1b[0m\r\n"
        "  \x1b[038;2;255;255;255m\x1b[048;2;217;119;054;1m ╚══════════════════════════════════════════════════════════╝ \x1b[0m\r\n\r\n"
        "  \x1b[038;2;217;119;054;1m" TERMUX_ABOUT_TITLE_U8 "\x1b[0m\r\n"
        "  \x1b[038;2;139;148;158m" TERMUX_ABOUT_SUB_U8 "\x1b[0m\r\n\r\n"
        "  \x1b[038;2;048;054;061m────────────────────────────────────────────────────────────\x1b[0m\r\n"
        "  \x1b[038;2;217;119;054;1m■ 版本号 (Version)      :\x1b[0m \x1b[038;2;230;237;243;1mv" TERMUX_VERSION "\x1b[0m\r\n"
        "  \x1b[038;2;217;119;054;1m■ 作  者 (Author)       :\x1b[0m \x1b[038;2;063;185;080;1mwu_dream813\x1b[0m\r\n"
        "  \x1b[038;2;217;119;054;1m■ 系统版本 (OS Version) :\x1b[0m \x1b[038;2;230;237;243m%s\x1b[0m\r\n"
        "  \x1b[038;2;048;054;061m────────────────────────────────────────────────────────────\x1b[0m\r\n\r\n"
        "  \x1b[038;2;139;148;158m开源项目仓库 : \x1b[038;2;088;166;255;4mhttps://github.com/wudream813/win-termux\x1b[0m\r\n"
        "  \x1b[038;2;139;148;158m开源许可协议 : \x1b[038;2;230;237;243mMIT License\x1b[0m\r\n\r\n"
        "  \x1b[038;2;110;118;129m提示: 这是一个独立的关于标签页，可点击右上角 [x] 或按 %s 关闭\x1b[0m\r\n",
        sys_ver, close_key);

    if (len > 0) theme_remap(about_buf, len);

    EnterCriticalSection(&g_mux.cs);
    screen_process_output(&pane->screen, about_buf, len);
    LeaveCriticalSection(&g_mux.cs);

    return idx;
}

int create_pane_shell_with_dir(const WCHAR *shell, const WCHAR *workdir) {
    int idx = -1;
    for (int i = 0; i < MAX_PANES; i++)
        if (!g_mux.panes[i].active && g_mux.panes[i].read_thread == NULL_HANDLE) { idx = i; break; }
    if (idx < 0) return -1;

    Pane *pane = &g_mux.panes[idx]; memset(pane, 0, sizeof(*pane));
    int pane_cols = g_mux.host_cols;
    if (!screen_init(&pane->screen, pane_cols, g_mux.host_rows)) return -1;
    pane->screen.pane_index = idx;

#ifdef _WIN32
    /* ---- Windows：ConPTY + CreateProcessW ---- */
    HANDLE pi_r = NULL, pi_w = NULL, po_r = NULL, po_w = NULL;
    COORD sz = {(SHORT)pane_cols, (SHORT)g_mux.host_rows};
    STARTUPINFOEXW si;
    SIZE_T as = 0;
    PROCESS_INFORMATION pi;
    WCHAR cmdline[256] = {0};
    WCHAR exp_dir[MAX_PATH] = {0};
    LPCWSTR cur_dir = NULL;
    BOOL created = FALSE;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.StartupInfo.cb = sizeof(si);

    if (!CreatePipe(&pi_r, &pi_w, NULL, 0)) goto create_fail;
    if (!CreatePipe(&po_r, &po_w, NULL, 0)) goto create_fail;
    if (FAILED(conpty_create(sz, pi_r, po_w, conpty_default_flags(), &pane->hpc))) goto create_fail;

    InitializeProcThreadAttributeList(NULL, 1, 0, &as);
    if (as == 0) goto create_fail;
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(as);
    if (!si.lpAttributeList) goto create_fail;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &as)) goto attr_fail;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pane->hpc, sizeof(HPCON), NULL, NULL)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        goto attr_fail;
    }

    wcsncpy(cmdline, shell, 255); cmdline[255] = 0;
    if (workdir && *workdir) {
        ExpandEnvironmentStringsW(workdir, exp_dir, MAX_PATH - 1);
    }
    cur_dir = exp_dir[0] ? exp_dir : NULL;

    created = CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT, NULL, cur_dir,
                             &si.StartupInfo, &pi);
    if (!created && _wcsicmp(shell, L"cmd.exe") != 0 && _wcsicmp(shell, L"powershell.exe") != 0) {
        WCHAR fallback[300];
        _snwprintf(fallback, 299, L"cmd.exe /c %s", shell);
        created = CreateProcessW(NULL, fallback, NULL, NULL, FALSE,
                                 EXTENDED_STARTUPINFO_PRESENT, NULL, cur_dir,
                                 &si.StartupInfo, &pi);
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    si.lpAttributeList = NULL;
    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(pi_r); pi_r = NULL;
        CloseHandle(po_w); po_w = NULL;
        pane->pipe_in = pi_w; pane->pipe_out = po_r;
        pane->active = 1;
        pane->exited_hold = 1;
        pane->exit_code = err;
        char u8cmd[64] = {0};
        WideCharToMultiByte(CP_UTF8, 0, shell, -1, u8cmd, 63, NULL, NULL);
        char *space = strchr(u8cmd, ' ');
        if (space) *space = 0;
        sanitize_title(u8cmd, (int)strlen(u8cmd), pane->title, sizeof(pane->title));

        char errmsg[256];
        int elen = snprintf(errmsg, sizeof(errmsg),
            "\x1b[31;1m[启动失败: 无法执行命令 \"%s\" (错误码: %lu)]\x1b[0m\r\n\x1b[33m按任意键关闭该标签页...\x1b[0m\r\n",
            u8cmd, (unsigned long)err);
        EnterCriticalSection(&g_mux.cs);
        screen_process_output(&pane->screen, errmsg, elen);
        LeaveCriticalSection(&g_mux.cs);

        if (idx >= g_mux.pane_count) g_mux.pane_count = idx + 1;
        return idx;
    }

    CloseHandle(pi_r); pi_r = NULL;
    CloseHandle(po_w); po_w = NULL;
    pane->pipe_in = pi_w; pane->pipe_out = po_r; pane->process = pi.hProcess; pane->thread = pi.hThread; pane->active = 1;
    if (_wcsicmp(shell, L"powershell.exe") == 0 || _wcsicmp(shell, L"powershell") == 0) {
        snprintf(pane->title, sizeof(pane->title), "PowerShell");
        snprintf(pane->full_title, sizeof(pane->full_title), "powershell.exe");
    } else if (_wcsicmp(shell, L"cmd.exe") == 0 || _wcsicmp(shell, L"cmd") == 0) {
        snprintf(pane->title, sizeof(pane->title), "cmd");
        snprintf(pane->full_title, sizeof(pane->full_title), "cmd.exe");
    } else {
        char u8cmd[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, shell, -1, u8cmd, 255, NULL, NULL);
        snprintf(pane->full_title, sizeof(pane->full_title), "%s", u8cmd);
        char *space = strchr(u8cmd, ' ');
        if (space) *space = 0;
        sanitize_title(u8cmd, (int)strlen(u8cmd), pane->title, sizeof(pane->title));
    }
    if (idx >= g_mux.pane_count) g_mux.pane_count = idx + 1;
    pane->read_thread = (HANDLE)_beginthreadex(NULL, 0, pane_read_thread, (void*)(intptr_t)idx, 0, NULL);
    if (!pane->read_thread) {
        pane->active = 0;
        conpty_close(pane->hpc);
        CloseHandle(pane->pipe_in); CloseHandle(pane->pipe_out);
        TerminateProcess(pane->process, 0); WaitForSingleObject(pane->process, 500);
        CloseHandle(pane->process); CloseHandle(pane->thread);
        screen_free(&pane->screen);
        return -1;
    }
    return idx;

attr_fail:
    free(si.lpAttributeList);
create_fail:
    if (pane->hpc) conpty_close(pane->hpc);
    if (pi_r) CloseHandle(pi_r);
    if (pi_w) CloseHandle(pi_w);
    if (po_r) CloseHandle(po_r);
    if (po_w) CloseHandle(po_w);
    screen_free(&pane->screen);
    memset(pane, 0, sizeof(*pane));
    return -1;

#else  /* ---- POSIX（Linux / macOS）：forkpty ----
        * 子进程拿到一个真 pty 作为控制终端，父进程持 master。pipe_in / pipe_out 都
        * 是那个 master fd（读写同一端）；尺寸变化用 TIOCSWINSZ 通知，shell 自己
        * 会重排 —— 这正是 Windows 侧 ResizePseudoConsole 的对应物。 */
    char cmd_utf8[512] = {0};
    WideCharToMultiByte(CP_UTF8, 0, shell, -1, cmd_utf8, (int)sizeof(cmd_utf8) - 1, NULL, NULL);
    char dir_utf8[MAX_PATH] = {0};
    if (workdir && *workdir) {
        /* Windows 分支用 ExpandEnvironmentStringsW；POSIX 侧必须自己展开，
         * 否则设置页承诺的「支持 %USERPROFILE%」在这边是死的。 */
        char raw_dir[MAX_PATH] = {0};
        WideCharToMultiByte(CP_UTF8, 0, workdir, -1, raw_dir, (int)sizeof(raw_dir) - 1, NULL, NULL);
        posix_expand_env(raw_dir, dir_utf8, sizeof(dir_utf8));
    }

    if (plat_proc_spawn(cmd_utf8, dir_utf8[0] ? dir_utf8 : NULL,
                        pane_cols, g_mux.host_rows,
                        &pane->pipe_in, &pane->pipe_out, &pane->process) != 0) {
        screen_free(&pane->screen);
        memset(pane, 0, sizeof(*pane));
        return -1;
    }
    pane->active = 1;

    /* 标题：POSIX 上没有 cmd / powershell 的特例，统一取命令名。 */
    snprintf(pane->full_title, sizeof(pane->full_title), "%s", cmd_utf8);
    char *sp = strchr(cmd_utf8, ' ');
    if (sp) *sp = 0;
    sanitize_title(cmd_utf8, (int)strlen(cmd_utf8), pane->title, sizeof(pane->title));

    if (idx >= g_mux.pane_count) g_mux.pane_count = idx + 1;
    pane->read_thread = plat_thread_start(pane_read_thread, (void *)(intptr_t)idx);
    if (pane->read_thread == NULL_HANDLE) {
        pane->active = 0;
        plat_proc_close(&pane->process, &pane->pipe_in, &pane->pipe_out);
        screen_free(&pane->screen);
        memset(pane, 0, sizeof(*pane));
        return -1;
    }
    return idx;
#endif
}

int create_pane_shell(const WCHAR *shell) {
    return create_pane_shell_with_dir(shell, NULL);
}

int create_pane_from_item(int idx) {
    if (idx < 0 || idx >= g_chooser_item_count) return create_pane();
    if (strcmp(g_chooser_items[idx].cmd, ":custom") == 0) {
        g_mux.custom_cmd_mode = 1;
        g_mux.custom_cmd_len = 0;
        g_mux.custom_cmd_pos = 0;
        g_mux.custom_cmd_buf[0] = 0;
        g_pop_anchor_x = g_mouse_x >= 0 ? g_mouse_x : 10;
        g_mux.needs_redraw = 1;
        return -1;
    }
    WCHAR wcmd[256] = {0};
    WCHAR wdir[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, g_chooser_items[idx].cmd, -1, wcmd, 255);
    if (g_chooser_items[idx].workdir[0]) {
        MultiByteToWideChar(CP_UTF8, 0, g_chooser_items[idx].workdir, -1, wdir, 255);
    }
    int p = create_pane_shell_with_dir(wcmd, wdir[0] ? wdir : NULL);
    if (p >= 0) {
        strncpy(g_mux.panes[p].title, g_chooser_items[idx].name, sizeof(g_mux.panes[p].title) - 1);
        strncpy(g_mux.panes[p].full_title, g_chooser_items[idx].name, sizeof(g_mux.panes[p].full_title) - 1);
        /* v1.8.9: 菜单项可以配置启动默认颜色（0 = 跟随默认蓝色）。 */
        int c = g_chooser_items[idx].color;
        g_mux.panes[p].color = (c >= 1 && c <= 8) ? c : 0;
    }
    return p;
}

int create_pane(void) {
    if (g_chooser_item_count > 0 && strcmp(g_chooser_items[0].cmd, ":custom") != 0) {
        return create_pane_from_item(0);
    }
#ifdef _WIN32
    return create_pane_shell(L"cmd.exe");
#else
    return create_pane_shell(plat_default_shell());
#endif
}

int open_settings_pane(void) {
    for (int i = 0; i < g_mux.pane_count; i++) {
        if (g_mux.panes[i].active && g_mux.panes[i].is_settings) {
            switch_pane(i);
            return i;
        }
    }
    int idx = -1;
    for (int i = 0; i < MAX_PANES; i++) {
        if (!g_mux.panes[i].active && g_mux.panes[i].read_thread == NULL_HANDLE) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    Pane *pane = &g_mux.panes[idx];
    memset(pane, 0, sizeof(*pane));
    int pane_cols = g_mux.host_cols;
    if (!screen_init(&pane->screen, pane_cols, g_mux.host_rows)) return -1;
    pane->screen.pane_index = idx;
    pane->screen.in_alt_screen = 1;
    pane->active = 1;
    pane->is_settings = 1;
    pane->color = 6;
    snprintf(pane->title, sizeof(pane->title), "设置");
    snprintf(pane->full_title, sizeof(pane->full_title), "termux - 设置 (Settings)");

    if (idx >= g_mux.pane_count) g_mux.pane_count = idx + 1;

    g_settings_nav = 0;
    g_settings_field = 0;
    g_settings_table_sel = 0;
    g_settings_show_presets = 0;
    g_preset_sel = 0;
    if (g_chooser_item_count > 0) {
        load_item_to_editor(0);
    }
    switch_pane(idx);
    return idx;
}

void close_pane(int idx) {
    if (idx < 0 || idx >= g_mux.pane_count) return;
    Pane *pane = &g_mux.panes[idx];

    EnterCriticalSection(&g_mux.cs);
    if (!pane->active && !pane->read_thread) { LeaveCriticalSection(&g_mux.cs); return; }
    pane->active = 0;
    LeaveCriticalSection(&g_mux.cs);

#ifndef _WIN32
    /* POSIX：读线程阻塞在 pty 的 read 上，只有 shell 真的退了才拿得到 EOF，
     * 所以必须【先杀进程再等线程】，否则 join 必然白等满 2000ms。
     * Windows 侧不需要：CloseHandle 会让阻塞中的 ReadFile 立刻失败返回。 */
    if (pane->process != NULL_HANDLE) plat_proc_kill(pane->process);
#endif
    plat_thread_join(&pane->read_thread, 2000);
    plat_proc_close(&pane->process, &pane->pipe_in, &pane->pipe_out);
    pane->hpc = NULL_HANDLE;
    pane->thread = NULL_HANDLE;

    EnterCriticalSection(&g_mux.cs);
    free(pane->rf_grid); pane->rf_grid = NULL; pane->rf_valid = 0; pane->rf_rows = pane->rf_cols = 0;
    screen_free(&pane->screen);
    LeaveCriticalSection(&g_mux.cs);
}

void switch_pane(int idx) {
    if (idx < 0 || idx >= g_mux.pane_count || !g_mux.panes[idx].active) return;
    /* 复制 / 搜索属于原来那个 pane：换标签页时先收回，避免两个标签页同时
     * 响应同一套按键（选区、匹配高亮都会串台）。 */
    if (idx != g_mux.active_pane) ui_modes_cancel();
    /* 新切到的 pane 若还不属于任何分屏树（新建的独立标签页 / 关于 / 设置页），
     * 给它注册一棵单叶子分屏树。分屏子窗格带 is_split_child 标记，已在切分
     * 流程里挂进树，不在此重复注册。 */
    if (!g_mux.panes[idx].is_split_child && !g_mux.panes[idx].is_settings &&
        !g_mux.panes[idx].is_about && split_tab_of_pane(idx) < 0) {
        split_init_tab(idx);
    }
    g_mux.active_pane = idx;
    g_mux.panes[idx].scroll_offset = 0;
    g_mux.needs_redraw = 1;
}

int find_next_active_pane(int cur) {
    for (int i = 1; i <= g_mux.pane_count; i++) {
        int n = (cur + i) % g_mux.pane_count;
        if (g_mux.panes[n].active) return n;
    }
    return -1;
}

/* ---- 分屏：按窗格矩形调整每个子 pane 的屏幕/ConPTY 尺寸 -------------------- */
void pane_resize_to(int idx, int cols, int rows) {
    if (idx < 0 || idx >= g_mux.pane_count) return;
    Pane *p = &g_mux.panes[idx];
    if (!p->active) return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    /* 拖动分屏边框期间：本地 screen_resize 和 ResizePseudoConsole 都【不】做，
     * 两边宽度一起冻结在拖动前的值，松手后第一帧一次性补齐。
     *
     * 早先只冻结了 ConPTY 那一侧、本地 screen_resize 照常逐帧做，理由是「拖动过程
     * 中画面仍然实时重排」。那个实时重排本身就是错位的来源：conhost 的输出始终按
     * 冻结的旧宽度排版，模型却每帧按新宽度 reflow，等于拿 16 列的碎片去拼 92 列的
     * 行，拼出来必然是错的（真机实测一次拖动里模型宽度在 15/16/20/30/45/60/76/92
     * 之间跳，非空历史行 8331→2771→1360→4167 来回剧变，而 conpty_cols 一直是 16）。
     * 松手后补发 resize，conhost 按新宽度重排，画面才恢复正确 —— 症状就是用户报的
     * 「拖动过程中有错位，要等停止拖动，错位才消失」。
     *
     * 两侧一起冻结后拖动中不存在宽度差，也就没有错位；窗格边界仍然跟鼠标动
     * （布局矩形由 split_layout 算，不经过这里），只是内容在松手时才重排。 */
    if (split_drag_active()) {
        g_mux.needs_redraw = 1;
        return;
    }
    EnterCriticalSection(&g_mux.cs);
    if (p->screen.cols != cols || p->screen.rows != rows) {
        screen_resize(&p->screen, cols, rows);
        p->screen.detect_count = 0;
        if (p->scroll_offset > 0) {
            int lim_pc = screen_scroll_limit(&p->screen);
            if (p->scroll_offset > lim_pc) p->scroll_offset = lim_pc;
        }
    }
    LeaveCriticalSection(&g_mux.cs);
    /* 只在尺寸真正变化时才向 ConPTY 下发 resize：分屏渲染每帧都会按布局调
     * pane_resize_to，若同尺寸也下发，ConPTY 会整屏重绘（记录里逐帧重复的
     * 全屏 repaint），还会让行内容被以不同 wrap 覆写而残留过期 line_wrap。 */
    if (p->process != NULL_HANDLE && (p->conpty_cols != cols || p->conpty_rows != rows)) {
        plat_proc_resize(p->process, p->pipe_in, p->pipe_out, cols, rows);
        p->conpty_cols = cols;
        p->conpty_rows = rows;
    }
    g_mux.needs_redraw = 1;
}
