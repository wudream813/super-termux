/* ==========================================================================
 * test_expand_env.c —— posix_expand_env() 的回归
 *
 * 背景：Windows 分支用 ExpandEnvironmentStringsW 展开启动目录，POSIX 分支原先
 * 【什么都不做】，把 %USERPROFILE% 原样交给 chdir —— 而设置页的提示明写着
 * 「支持 %USERPROFILE%」，于是这个功能在 Linux/macOS 上是死的。更糟的是 chdir
 * 失败还被静默吞掉（注释写着「退回 HOME」，代码里是个空语句），用户完全不知道
 * 自己的设置没生效。
 *
 * 编译： cc -O1 -Wall -Iinclude tests/test_expand_env.c src/platform_posix.c \
 *            src/term_input_posix.c -o /tmp/ee -lpthread -lutil
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "common.h"
#include "platform.h"

void host_write(const char *d, int l) { (void)d; (void)l; }
void posix_expand_env(const char *in, char *out, size_t out_sz);

static int FAILS = 0, CHECKS = 0;

static void ck(const char *label, const char *in, const char *want) {
    char out[512];
    memset(out, 0xAA, sizeof out);
    posix_expand_env(in, out, sizeof out);
    CHECKS++;
    if (strcmp(out, want) == 0) {
        printf("  [ok]   %-30s %s\n", label, out);
    } else {
        FAILS++;
        printf("  [FAIL] %-30s 得到 [%s] 期望 [%s]\n", label, out, want);
    }
}

int main(void) {
    printf("=== posix_expand_env ===\n");
    const char *home = getenv("HOME");
    if (!home || !*home) { printf("  (跳过：没有 $HOME)\n"); return 0; }

    char want[512];

    ck("无变量原样输出", "/tmp/foo", "/tmp/foo");
    ck("空串", "", "");

    snprintf(want, sizeof want, "%s", home);
    ck("%USERPROFILE% -> $HOME", "%USERPROFILE%", want);

    snprintf(want, sizeof want, "%s/Documents", home);
    ck("%USERPROFILE%/Documents", "%USERPROFILE%/Documents", want);

    snprintf(want, sizeof want, "%s", home);
    ck("%HOMEPATH% -> $HOME", "%HOMEPATH%", want);

    /* %HOME% 走普通 getenv 路径 */
    setenv("HOME", home, 1);
    snprintf(want, sizeof want, "%s", home);
    ck("%HOME% 走 getenv", "%HOME%", want);

    /* 自定义变量 */
    setenv("TERMUX_TEST_DIR", "/opt/termux", 1);
    ck("自定义 %TERMUX_TEST_DIR%", "%TERMUX_TEST_DIR%/log", "/opt/termux/log");

    /* 小写变量名也能查到全大写的 */
    ck("小写名回退到大写", "%termux_test_dir%", "/opt/termux");

    /* 未定义的变量原样保留（与 Windows 对未定义变量的处理一致） */
    ck("未定义变量原样保留", "%NO_SUCH_VAR_XYZ%/a", "%NO_SUCH_VAR_XYZ%/a");

    /* 开头的 ~ */
    snprintf(want, sizeof want, "%s", home);
    ck("裸 ~", "~", want);
    snprintf(want, sizeof want, "%s/x", home);
    ck("~/x", "~/x", want);
    /* 中间的 ~ 不展开（与 shell 一致） */
    ck("中间的 ~ 不展开", "/a~b", "/a~b");

    /* % 不成对：原样 */
    ck("落单的 %", "50% off", "50% off");
    ck("% 后面不是变量名", "%/tmp", "%/tmp");

    /* 多个变量 */
    snprintf(want, sizeof want, "%s/opt/termux", home);
    ck("两个变量连着", "%USERPROFILE%%TERMUX_TEST_DIR%", want);

    /* 截断安全：给一个很小的缓冲区，必须 NUL 收尾且不越界 */
    {
        char small[8];
        memset(small, 0xAA, sizeof small);
        posix_expand_env("%USERPROFILE%/very/long/path", small, sizeof small);
        CHECKS++;
        if (small[7] == 0 && strlen(small) <= 7) {
            printf("  [ok]   %-30s [%s]（未越界、有 NUL）\n", "小缓冲区截断安全", small);
        } else {
            FAILS++;
            printf("  [FAIL] 小缓冲区没 NUL 收尾或越界\n");
        }
    }
    /* out_sz = 0 不能崩 */
    posix_expand_env("x", NULL, 0);
    {
        char z[1] = {0x5A};
        posix_expand_env("x", z, 0);
        CHECKS++;
        if (z[0] == 0x5A) printf("  [ok]   %-30s 缓冲区没被碰\n", "out_sz=0 不写");
        else { FAILS++; printf("  [FAIL] out_sz=0 却写了缓冲区\n"); }
    }

    printf("\nposix_expand_env: %d 项检查，%d 项失败\n", CHECKS, FAILS);
    return FAILS ? 1 : 0;
}
