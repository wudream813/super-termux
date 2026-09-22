/* ---------------------------------------------------------------------------
 * platform.h —— 平台后端接口（Windows / POSIX 各一份实现，链接期选择）。
 *
 * 划分原则：能被表达成「Win32 API 的 POSIX 等价物」的东西放在 include/wincompat.h
 * 里（类型、时钟、临界区、UTF 转换），引擎代码因此完全不用改；只有语义上真正不同、
 * 无法用同名 API 糊过去的才进这个接口：
 *
 *   plat_proc_*   子进程（Windows = ConPTY + CreateProcessW；POSIX = forkpty）
 *   plat_console_*宿主控制台（Windows = SetConsoleMode/ReadConsoleInputW；
 *                              POSIX = termios + 自己解析字节流）
 *   plat_clip_*   剪贴板（Windows = Win32 Clipboard；POSIX = OSC 52 / pbcopy / xclip）
 *   plat_sysinfo  关于页的系统信息（Windows = 注册表；POSIX = uname）
 *
 * ⚠ 句柄编码约定（POSIX 侧）：types.h 里 Pane 的 HANDLE 字段是 intptr_t，
 *    pipe_in / pipe_out 存 fd，process 存 pid，read_thread 存 pthread_t。
 *    判断有效性一律用平台实现里的辅助函数，不要直接和 NULL 比。
 * ------------------------------------------------------------------------- */
#ifndef WIN_TERMUX_PLATFORM_H
#define WIN_TERMUX_PLATFORM_H

#include "common.h"

/* ---- 默认 shell ----
 * 引擎里有好几处「没有明确指定 shell 时用什么」的字面量（分屏建新窗格、
 * 自定义命令留空、新建启动项的默认值）。以前都硬写 cmd.exe，在 POSIX 上
 * execvp("cmd.exe") 必然失败，表现为「新窗格是空白的」。收成宏以后 Windows
 * 侧展开出来还是同一个字面量，行为不变。 */
#ifdef _WIN32
#define TERMUX_DEFAULT_SHELL_W   L"cmd.exe"
#define TERMUX_DEFAULT_SHELL_U8  "cmd.exe"
/* 路径分隔符。config.c 的 resolve_ini_path() 要拿它切 exe 路径、拼 ini 路径；
 * 写死反斜杠的话 POSIX 上 wcsrchr 永远找不到分隔符，配置就退化成相对路径
 * "termux.ini" —— 落在【当前工作目录】，换目录启动就换一份配置。 */
#define TERMUX_PATH_SEP          L'\\'
#define TERMUX_PATH_SEP_S        "\\"
#define TERMUX_ABOUT_TITLE_U8    "Windows 终端复用器 (Terminal Multiplexer)"
#define TERMUX_ABOUT_SUB_U8      "基于 Windows ConPTY 的高性能终端复用多标签环境"
#else
#define TERMUX_DEFAULT_SHELL_W   L"/bin/sh"
#define TERMUX_DEFAULT_SHELL_U8  "/bin/sh"
#define TERMUX_PATH_SEP          L'/'
#define TERMUX_PATH_SEP_S        "/"
/* 关于页文案。原来写死「Windows 终端复用器 / 基于 Windows ConPTY」，在同一页的
 * 「系统版本」已经正确显示 Linux/macOS 的情况下自相矛盾。 */
#define TERMUX_ABOUT_TITLE_U8    "终端复用器 (Terminal Multiplexer)"
#define TERMUX_ABOUT_SUB_U8      "基于 POSIX pty (forkpty) 的高性能终端复用多标签环境"
#endif

/* ---- 子进程后端 ----
 * Windows：conpty_create + CreateProcessW；POSIX：forkpty（子进程拿到真 pty，
 * 尺寸变化用 TIOCSWINSZ 通知，shell 自己会重排 —— 这正是 Windows 侧
 * ResizePseudoConsole 的对应物）。 */
int  plat_proc_spawn(const char *cmd_utf8, const char *workdir_utf8,
                     int cols, int rows, HANDLE *out_in, HANDLE *out_out,
                     HANDLE *out_proc);
void plat_proc_resize(HANDLE proc, HANDLE proc_in, HANDLE proc_out, int cols, int rows);
/* 1 = 已退出（*exit_code 有效）；0 = 还在跑。非阻塞。 */
int  plat_proc_exited(HANDLE proc, DWORD *exit_code);
void plat_proc_kill(HANDLE proc);
/* 关 fd / 收尸。可重入（重复调用安全）。 */
void plat_proc_close(HANDLE *proc, HANDLE *in_fd, HANDLE *out_fd);

/* 读线程：Windows 用 _beginthreadex，POSIX 用 pthread。统一签名。 */
typedef unsigned (*plat_thread_fn)(void *arg);
HANDLE plat_thread_start(plat_thread_fn fn, void *arg);
/* 等待线程结束（最多 wait_ms 毫秒），然后释放句柄。 */
void   plat_thread_join(HANDLE *th, unsigned wait_ms);

/* 非阻塞地把 fd/handle 里现有的字节读出来。返回 >0 字节数，0 = 没数据，<0 = 出错/EOF。 */
int  plat_read(HANDLE h, char *buf, int cap);
int  plat_write_fd(HANDLE h, const char *buf, int len);
/* 管道里还有多少字节可读（对应 PeekNamedPipe）。 */
int  plat_peek_avail(HANDLE h);

/* ---- 宿主控制台后端 ---- */
/* 进入原始模式 + 备用屏 + 鼠标追踪。返回 0 成功。 */
int  plat_console_init(int want_mouse);
void plat_console_shutdown(void);
/* 当前尺寸：*cols 列，*total_rows 行（含标签栏那一行）。返回 0 成功。 */
int  plat_console_size(int *cols, int *total_rows);
/* 把宿主终端的字节流解析成 Windows 形状的 INPUT_RECORD。
 * 返回读到的记录数（可能为 0）。timeout_ms 内没有输入就返回 0。
 * 需要产生窗口尺寸变化时塞一条 WINDOW_BUFFER_SIZE_EVENT。 */
int  plat_console_read(INPUT_RECORD *out, int cap, int timeout_ms);

/* POSIX 上的默认 shell（$SHELL，兜底 /bin/sh）。Windows 侧返回 L"cmd.exe"。 */
const WCHAR *plat_default_shell(void);

/* 用户主目录（Windows = %USERPROFILE%，POSIX = $HOME，都没有就查 passwd）。
 * 找不到时返回 NULL，调用方要判空。 */
const WCHAR *plat_user_home(void);

#ifndef _WIN32
/* 展开启动目录里的 %VAR% 与开头的 ~。Windows 侧用 ExpandEnvironmentStringsW，
 * 所以这个只在 POSIX 侧存在（也因此不叫 plat_*，避免「两侧都要有实现」误报）。 */
void posix_expand_env(const char *in, char *out, size_t out_sz);
#endif


/* ---- 剪贴板 ---- */
void plat_clip_copy(const char *utf8, int len);

/* ---- 关于页的系统信息 ---- */
void plat_sysinfo(char *out, int cap);

#endif /* WIN_TERMUX_PLATFORM_H */
