/* ==========================================================================
 * test_write_backpressure.c —— plat_write_fd() 的背压回归
 *
 * 背景：pty master 是 O_NONBLOCK 的（plat_proc_spawn 里设的）。子进程一时不读时
 * write() 返回 EAGAIN，而旧版 plat_write_fd 在这里直接 return -1，那一段输入就
 * 【静默丢掉】了 —— 没有任何提示，用户只会觉得「刚才那几下键盘没反应」。
 * Windows 侧的 WriteFile 是阻塞的、不会丢，两边行为不一致。
 *
 * 这个测试直接对真函数下判据（不是重写一份逻辑）：
 *   子进程先 stty raw -echo（关掉行规程，避开 MAX_CANON 干扰），sleep 1 秒
 *   —— 这一秒里没人排空 tty —— 然后 cat 到文件。父进程立刻 plat_write_fd
 *   灌 20000 字节。
 *     旧版：写满 4095（tty 缓冲）就 EAGAIN，立刻返回 4095，文件里只有 4095。
 *     新版：EAGAIN 时 poll(POLLOUT) 等子进程排空，最终 20000 全部到达。
 *
 * 编译： cc -O1 -Iinclude tests/test_write_backpressure.c src/platform_posix.c \
 *            src/term_input_posix.c -o /tmp/wb -lpthread -lutil
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <util.h>      /* forkpty：glibc 在 <pty.h>，Apple 在 <util.h> */
#else
#include <pty.h>
#endif
#include "common.h"
#include "platform.h"

/* platform_posix.c 里 plat_clip_copy 会调它，链进来就得有个定义。 */
void host_write(const char *d, int l) { (void)d; (void)l; }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int plat_write_fd(HANDLE h, const char *buf, int len);

#define NBYTES 20000
#define STALL  "1"          /* 子进程不排空的秒数，必须 < WRITE_WAIT_MS(1000) */

int main(void) {
    const char *out = "/tmp/termux_wb_test.out";
    unlink(out);

    int master = -1;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = 80; ws.ws_row = 24;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return 2; }

    if (pid == 0) {
        /* 子进程：关掉行规程（否则 canonical 模式下 MAX_CANON=4096 会先把多余输入
         * 丢掉，量到的就不是 plat_write_fd 的行为了），停一秒再排空到文件。 */
        /* 先切 raw 再喊 RDY：父进程必须等到 RDY 才能开灌，否则那时 tty 还是
         * canonical 模式，量到的 4096 是行规程的 MAX_CANON，不是 plat_write_fd。 */
        const char *argv[] = { "/bin/sh", "-c",
            "stty raw -echo; printf RDY; sleep " STALL
            "; cat > /tmp/termux_wb_test.out", NULL };
        execv("/bin/sh", (char *const *)argv);
        _exit(127);
    }

    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);

    /* 等子进程把 tty 切进 raw 并发出 RDY（最多等 5 秒）。 */
    {
        char acc[64]; int an = 0, ready = 0;
        double dl = now_sec() + 5.0;
        while (now_sec() < dl) {
            char c;
            ssize_t r = read(master, &c, 1);
            if (r == 1) {
                if (an < (int)sizeof(acc) - 1) acc[an++] = c;
                acc[an] = 0;
                if (strstr(acc, "RDY")) { ready = 1; break; }
            } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            } else {
                usleep(5000);
            }
        }
        if (!ready) { printf("  [FAIL] 等不到子进程的 RDY 标记\n"); return 2; }
        printf("  [ok]   子进程已切入 raw 模式（收到 RDY）\n");
    }

    char *buf = (char *)malloc(NBYTES);
    memset(buf, 'x', NBYTES);

    double t0 = now_sec();
    int wrote = plat_write_fd((HANDLE)(intptr_t)master, buf, NBYTES);
    double el = now_sec() - t0;

    /* 把子进程的输出读干净，否则它可能卡在写 stdout 上不肯退出。 */
    char sink[4096];
    for (int i = 0; i < 200; i++) {
        ssize_t r = read(master, sink, sizeof sink);
        if (r <= 0) break;
    }

    int status = 0;
    for (int i = 0; i < 100; i++) {          /* 最多等 10 秒 */
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        usleep(100000);
    }

    long got = -1;
    FILE *f = fopen(out, "rb");
    if (f) {
        long n = 0; int c;
        while ((c = fgetc(f)) != EOF) if (c == 'x') n++;
        got = n;
        fclose(f);
    }
    unlink(out);

    printf("  plat_write_fd 返回 %d / %d，耗时 %.2fs，文件收到 %ld 字节\n",
           wrote, NBYTES, el, got);

    int fails = 0;
    if (wrote != NBYTES) {
        printf("  [FAIL] plat_write_fd 只写了 %d 字节就返回了（EAGAIN 时被丢弃）\n", wrote);
        fails++;
    } else {
        printf("  [ok]   plat_write_fd 把 %d 字节全部写完\n", NBYTES);
    }
    if (got != NBYTES) {
        printf("  [FAIL] 子进程只收到 %ld 字节（丢了 %d）\n", got, NBYTES - (int)got);
        fails++;
    } else {
        printf("  [ok]   子进程收到全部 %d 字节，一个没丢\n", NBYTES);
    }
    if (el < 0.8) {
        printf("  [FAIL] 只用了 %.2fs —— 说明根本没等子进程排空（判据失效）\n", el);
        fails++;
    } else {
        printf("  [ok]   确实等待了子进程的 %.2fs 停顿（%.2fs）\n", atof(STALL), el);
    }
    free(buf);
    close(master);

    printf(fails ? "test_write_backpressure: %d 项失败\n" : "test_write_backpressure: 全部通过\n", fails);
    return fails ? 1 : 0;
}
