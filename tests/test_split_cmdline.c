/* ==========================================================================
 * test_split_cmdline.c —— posix_split_cmdline() 的引号解析回归
 *
 * 背景：菜单项 / 自定义命令行里写的是一整条命令行字符串，POSIX 侧必须自己切成
 * argv 才能 execvp。旧实现只按空格切、完全不认引号，于是
 *     /bin/sh -c "echo hi; sleep 6"
 * 被撕成 ["/bin/sh","-c","\"echo","hi;","sleep","6\""]，子进程实际执行的是
 * /bin/sh -c '"echo' —— 窗格一闪就没了，用户只看到「点了没反应」。
 * Windows 侧 CreateProcessW 收的是整条字符串、引号由子进程 CRT 解析，所以那边
 * 一直是好的：这是个纯 POSIX 的行为差异。
 *
 * 编译： cc -O1 -Wall -Iinclude tests/test_split_cmdline.c src/platform_posix.c \
 *            src/term_input_posix.c -o /tmp/sc -lpthread -lutil
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "platform.h"

void host_write(const char *d, int l) { (void)d; (void)l; }
/* 不在 platform.h 里（POSIX 内部辅助函数），单测自带声明。 */
int posix_split_cmdline(char *buf, char **argv, int max_args);

static int FAILS = 0;
static int CHECKS = 0;

static void ck_args(const char *label, const char *cmd, const char *const *want) {
    char buf[512];
    char *argv[32];
    snprintf(buf, sizeof buf, "%s", cmd);
    int n = posix_split_cmdline(buf, argv, 32);
    int want_n = 0;
    while (want[want_n]) want_n++;

    CHECKS++;
    int ok = (n == want_n);
    if (ok) {
        for (int i = 0; i < n; i++)
            if (strcmp(argv[i], want[i]) != 0) { ok = 0; break; }
    }
    if (ok) {
        printf("  [ok]   %-34s -> %d 个参数\n", label, n);
    } else {
        FAILS++;
        printf("  [FAIL] %-34s -> 得到 %d 个:", label, n);
        for (int i = 0; i < n; i++) printf(" [%s]", argv[i]);
        printf("  期望 %d 个:", want_n);
        for (int i = 0; i < want_n; i++) printf(" [%s]", want[i]);
        printf("\n");
    }
    /* argv 必须以 NULL 收尾，否则 execvp 会越界读 */
    CHECKS++;
    if (n < 32 && argv[n] == NULL) {
        printf("         argv[%d] == NULL（execvp 收尾正确）\n", n);
    } else {
        FAILS++;
        printf("  [FAIL] argv 没有 NULL 收尾\n");
    }
}

int main(void) {
    printf("=== posix_split_cmdline 引号解析 ===\n");

    const char *e0[] = { "/bin/sh", NULL };
    ck_args("无参数", "/bin/sh", e0);

    const char *e1[] = { "bash", "-l", NULL };
    ck_args("简单带参数", "bash -l", e1);

    const char *e2[] = { "/bin/sh", "-c", "echo hi; sleep 6", NULL };
    ck_args("双引号内含空格和分号", "/bin/sh -c \"echo hi; sleep 6\"", e2);

    const char *e3[] = { "/bin/sh", "-c", "echo hi; sleep 6", NULL };
    ck_args("单引号内含空格和分号", "/bin/sh -c 'echo hi; sleep 6'", e3);

    const char *e4[] = { "echo", "a b", "c", NULL };
    ck_args("引号在参数中间", "echo \"a b\" c", e4);

    const char *e5[] = { "echo", "", "x", NULL };
    ck_args("空的双引号参数要保留", "echo \"\" x", e5);

    const char *e6[] = { "echo", "a b", NULL };
    ck_args("反斜杠转义空格", "echo a\\ b", e6);

    const char *e7[] = { "echo", "say \"hi\"", NULL };
    ck_args("双引号内转义引号", "echo \"say \\\"hi\\\"\"", e7);

    const char *e8[] = { "echo", "it's", NULL };
    ck_args("双引号内的单引号", "echo \"it's\"", e8);

    const char *e9[] = { "echo", "a\"b", NULL };
    ck_args("单引号内的双引号", "echo 'a\"b'", e9);

    const char *e10[] = { "echo", "unclosed", NULL };
    ck_args("引号没收尾就吃到串尾", "echo \"unclosed", e10);

    const char *e11[] = { "cmd", "a", "b", NULL };
    ck_args("多个连续空格", "cmd    a     b", e11);

    const char *e12[] = { "cmd", "a", "b", NULL };
    ck_args("制表符也是分词符", "cmd\ta\tb", e12);

    const char *e13[] = { "python3", "-c", "print('hello world')", NULL };
    ck_args("真实用例：python3 -c", "python3 -c \"print('hello world')\"", e13);

    const char *e14[] = { "ssh", "user@host", "ls -la /tmp", NULL };
    ck_args("真实用例：ssh 远程命令", "ssh user@host \"ls -la /tmp\"", e14);

    /* 首尾空格 */
    const char *e15[] = { "cmd", "x", NULL };
    ck_args("首尾空格", "   cmd   x   ", e15);

    printf("\nposix_split_cmdline: %d 项检查，%d 项失败\n", CHECKS, FAILS);
    return FAILS ? 1 : 0;
}
