#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逻辑行历史 + 本地 reflow 引擎回归（v1.8.46 地基模块）。

编译 src/loghist.c + src/utf8.c（用 tests/stub 的 windows.h 替身）跑纯逻辑断言：
  1. 硬换行产生独立逻辑行；软换行（续行）合并回同一条逻辑行。
  2. 尾空格（物理行空格填充）被裁掉。
  3. reflow：窄宽度把长逻辑行折成多行、宽宽度折回一行，顺序/内容正确。
  4. 宽字符（CJK 占 2 列、emoji 代理对占 2 列）在窄行边界不被拆开。
  5. 环形缓冲超容量淘汰最老逻辑行。
  6. 空逻辑行（连续回车）占一行。
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

TEST_C = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loghist.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  [FAIL] %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

/* 构造一条 cols 宽的物理行：cells[k] 由 ch 决定（空格=空格，0 传入 0）。
 * attrs 一律 0x07。 */
static void mkrow(CHAR_INFO *cells, int cols, const WCHAR *ch, int chlen) {
    for (int x = 0; x < cols; x++) {
        WCHAR c = (x < chlen) ? ch[x] : L' ';
        cells[x].Char.UnicodeChar = c;
        cells[x].Attributes = 0x07;
    }
}

/* 取第 v 条显示行到 buf（宽度 w），返回行内「去尾空格」后的字符串（宽字符/代理
 * 对按原样留在对应列）。简单起见测试用窄 ASCII 与单个 CJK/emoji。 */
/* ★ 这个名字原来叫 getline —— 撞上了 POSIX 的 getline(char**, size_t*, FILE*)。
 * Linux 上用 -std=c11（严格 ISO，不定义 _GNU_SOURCE/_POSIX_C_SOURCE）glibc 不
 * 声明它，所以本地这个能用；但 macOS 的 <stdio.h> 【无条件】声明 getline，于是
 * 标准原型（3 个参数）盖掉了本地这个（5 个参数），clang 直接报
 * "too many arguments to function call, expected 3, have 5"。
 * 只有 macOS 构建能抓到这个坑 —— CI 的 macOS 作业第一轮就栽在这儿。 */
static void get_vline(const LogHistory *h, int w, int v, CHAR_INFO *buf, int bw) {
    memset(buf, 0, sizeof(CHAR_INFO) * bw);
    int ok = loghist_get_visual_line(h, w, v, buf, NULL, NULL, NULL, bw);
    if (!ok) { /* 标记为越界：buf[0] = 0xFFFF */ buf[0].Char.UnicodeChar = (WCHAR)0xFFFF; }
}

/* 读显示行第 x 列字符（空/0 归一为 ' '）。 */
static char atx(CHAR_INFO *buf, int x) {
    WCHAR c = buf[x].Char.UnicodeChar;
    if (c == 0 || c == L' ') return ' ';
    return (char)(c & 0x7F);
}

static void test_hard_vs_soft_wrap(void) {
    LogHistory h; loghist_init(&h, 100);
    /* 行1：硬换行 "abc"（3 列内容 + 空格填充到 10）。 */
    CHAR_INFO r1[10]; mkrow(r1, 10, (WCHAR[]){L'a',L'b',L'c'}, 3);
    loghist_append_row(&h, r1, NULL, NULL, NULL, 10, 0);
    /* 行2：软换行续行 "def"（应并入行1 -> "abcdef"）。 */
    CHAR_INFO r2[10]; mkrow(r2, 10, (WCHAR[]){L'd',L'e',L'f'}, 3);
    loghist_append_row(&h, r2, NULL, NULL, NULL, 10, 1);
    /* 行3：硬换行 "ghi"。 */
    CHAR_INFO r3[10]; mkrow(r3, 10, (WCHAR[]){L'g',L'h',L'i'}, 3);
    loghist_append_row(&h, r3, NULL, NULL, NULL, 10, 0);

    /* 宽 20（折不下）：2 条逻辑行 -> 2 显示行。 */
    CHECK(loghist_visual_lines(&h, 20) == 2, "两硬换行=2逻辑行，宽20应为2显示行");
    CHAR_INFO buf[64];
    get_vline(&h, 20, 0, buf, 64);
    CHECK(atx(buf,0)=='a'&&atx(buf,1)=='b'&&atx(buf,2)=='c'&&
          atx(buf,3)=='d'&&atx(buf,4)=='e'&&atx(buf,5)=='f',
          "续行合并: 逻辑行0 = abcdef");
    CHECK(atx(buf,6)==' ', "逻辑行0 第6列为空（尾空格已裁）");
    get_vline(&h, 20, 1, buf, 64);
    CHECK(atx(buf,0)=='g'&&atx(buf,1)=='h'&&atx(buf,2)=='i', "逻辑行1 = ghi");
    loghist_free(&h);
    printf("  v1.8.46: 硬换行分行 / 软换行续行合并 / 尾空格裁剪\n");
}

static void test_reflow_narrow_wide(void) {
    LogHistory h; loghist_init(&h, 100);
    /* 一条 10 字符逻辑行：abcdefghij（先 6 个 + 续行 4 个）。 */
    CHAR_INFO r1[10]; mkrow(r1, 10, (WCHAR[]){L'a',L'b',L'c',L'd',L'e',L'f'}, 6);
    loghist_append_row(&h, r1, NULL, NULL, NULL, 10, 0);
    CHAR_INFO r2[10]; mkrow(r2, 10, (WCHAR[]){L'g',L'h',L'i',L'j'}, 4);
    loghist_append_row(&h, r2, NULL, NULL, NULL, 10, 1);

    /* 宽 4：10 字符 -> ceil(10/4)=3 显示行（4+4+2）。 */
    CHECK(loghist_visual_lines(&h, 4) == 3, "宽4: 10字符折3行");
    CHAR_INFO buf[64];
    get_vline(&h, 4, 0, buf, 64);
    CHECK(atx(buf,0)=='a'&&atx(buf,1)=='b'&&atx(buf,2)=='c'&&atx(buf,3)=='d', "reflow 宽4 行0=abcd");
    get_vline(&h, 4, 1, buf, 64);
    CHECK(atx(buf,0)=='e'&&atx(buf,1)=='f'&&atx(buf,2)=='g'&&atx(buf,3)=='h', "reflow 宽4 行1=efgh");
    get_vline(&h, 4, 2, buf, 64);
    CHECK(atx(buf,0)=='i'&&atx(buf,1)=='j'&&atx(buf,2)==' ', "reflow 宽4 行2=ij");
    /* 宽 20：折回一行。 */
    CHECK(loghist_visual_lines(&h, 20) == 1, "宽20: 折回1行");
    get_vline(&h, 20, 0, buf, 64);
    CHECK(atx(buf,0)=='a'&&atx(buf,9)=='j'&&atx(buf,10)==' ', "宽20: 整行 abcdefghij");
    /* 宽 6（原物理宽）：10 字符 -> 6+4 = 2 行。 */
    CHECK(loghist_visual_lines(&h, 6) == 2, "宽6: 10字符折2行");
    get_vline(&h, 6, 1, buf, 64);
    CHECK(atx(buf,0)=='g'&&atx(buf,3)=='j', "宽6 行1=ghij");
    loghist_free(&h);
    printf("  v1.8.46: reflow 窄(4)->折3行、宽(20)->1行、原宽(6)->2行，内容顺序不变\n");
}

static void test_cjk_not_split(void) {
    LogHistory h; loghist_init(&h, 100);
    /* 一条逻辑行：'X' + 中(2列) + 文(2列) + 字(2列) = 7 列内容。
     * 物理行宽 8 全部放满，硬换行单行。 */
    CHAR_INFO r[8];
    WCHAR ch[8] = {L'X', 0x4E2D/*中*/, 0, 0x6587/*文*/, 0, 0x5B57/*字*/, 0, L' '};
    mkrow(r, 8, ch, 8);
    /* mkrow 对 0 也置空格，需手动还原 CJK 次格为 0： */
    r[2].Char.UnicodeChar = 0; r[4].Char.UnicodeChar = 0; r[6].Char.UnicodeChar = 0;
    loghist_append_row(&h, r, NULL, NULL, NULL, 8, 0);

    /* 宽 4：CJK 占 2 列不拆。布局：X(1)+中(2)=3，'文' 2 列放不下(3+2>4)->折行，
     * 行1=文(2)+字? 文2列占满2，剩2列可放字(2) -> 行1=文字；行0=X中。 */
    int vl = loghist_visual_lines(&h, 4);
    CHECK(vl == 2, "宽4 CJK 不拆应折2行");
    CHAR_INFO buf[64];
    get_vline(&h, 4, 0, buf, 64);
    CHECK(atx(buf,0)=='X', "CJK reflow 行0 列0=X");
    CHECK(buf[1].Char.UnicodeChar == 0x4E2D, "CJK reflow 行0 列1=中(主格)");
    CHECK(buf[2].Char.UnicodeChar == 0, "CJK reflow 行0 列2=中(次格0)");
    CHECK(atx(buf,3)==' ', "CJK reflow 行0 列3空(文字被折到下一行)");
    get_vline(&h, 4, 1, buf, 64);
    CHECK(buf[0].Char.UnicodeChar == 0x6587, "CJK reflow 行1 列0=文(主格)");
    CHECK(buf[1].Char.UnicodeChar == 0, "CJK reflow 行1 列1=文(次格0)");
    CHECK(buf[2].Char.UnicodeChar == 0x5B57, "CJK reflow 行1 列2=字(主格)");
    loghist_free(&h);
    printf("  v1.8.46: CJK 宽字符在窄行边界整体折行、主/次格不拆\n");
}

static void test_emoji_not_split(void) {
    LogHistory h; loghist_init(&h, 100);
    /* emoji 😀 = U+1F600 -> 高代理 D83D、低代理 DE00，占 2 列。
     * 逻辑行：'a' + emoji + 'b' = 1+2+1 = 4 列。物理宽 5。 */
    CHAR_INFO r[5];
    WCHAR ch[5] = {L'a', 0xD83D, 0xDE00, L'b', L' '};
    for (int x = 0; x < 5; x++) { r[x].Char.UnicodeChar = ch[x]; r[x].Attributes = 0x07; }
    loghist_append_row(&h, r, NULL, NULL, NULL, 5, 0);

    /* 宽 3：a(1)+emoji(2)=3 放满一行；b(1) 折到下一行。 */
    CHECK(loghist_visual_lines(&h, 3) == 2, "宽3 emoji 不拆应折2行");
    CHAR_INFO buf[64];
    get_vline(&h, 3, 0, buf, 64);
    CHECK(atx(buf,0)=='a' && buf[1].Char.UnicodeChar==0xD83D && buf[2].Char.UnicodeChar==0xDE00,
          "emoji reflow 行0 = a + 代理对（完整）");
    get_vline(&h, 3, 1, buf, 64);
    CHECK(atx(buf,0)=='b', "emoji reflow 行1 = b（emoji 没被拆）");
    loghist_free(&h);
    printf("  v1.8.46: emoji 代理对在窄行边界整体折行、高/低代理不拆\n");
}

static void test_ring_eviction(void) {
    LogHistory h; loghist_init(&h, 3);   /* 只保留 3 条逻辑行 */
    for (int i = 0; i < 5; i++) {
        CHAR_INFO r[4];
        WCHAR ch[4] = {(WCHAR)('A' + i), L'x', L'x', L' '};
        mkrow(r, 4, ch, 3);
        loghist_append_row(&h, r, NULL, NULL, NULL, 4, 0);
    }
    /* 应只剩 C,D,E（A,B 被淘汰）。 */
    CHECK(h.count == 3, "容量3: 5行后 count=3");
    CHECK(loghist_visual_lines(&h, 80) == 3, "容量3: 3显示行");
    CHAR_INFO buf[64];
    get_vline(&h, 80, 0, buf, 64);
    CHECK(atx(buf,0)=='C', "淘汰后最老=C");
    get_vline(&h, 80, 2, buf, 64);
    CHECK(atx(buf,0)=='E', "淘汰后最新=E");
    get_vline(&h, 80, 3, buf, 64);
    CHECK(buf[0].Char.UnicodeChar == 0xFFFF, "越界 visual=3 返回失败");
    loghist_free(&h);
    printf("  v1.8.46: 环形缓冲超容量淘汰最老逻辑行\n");
}

static void test_empty_logical_lines(void) {
    LogHistory h; loghist_init(&h, 100);
    /* 空行（全空格）+ 硬换行：产生一条空逻辑行。 */
    CHAR_INFO blank[8];
    for (int x = 0; x < 8; x++) { blank[x].Char.UnicodeChar = L' '; blank[x].Attributes = 0x07; }
    loghist_append_row(&h, blank, NULL, NULL, NULL, 8, 0);
    CHAR_INFO r[8]; mkrow(r, 8, (WCHAR[]){L'h',L'i'}, 2);
    loghist_append_row(&h, r, NULL, NULL, NULL, 8, 0);
    CHECK(loghist_visual_lines(&h, 80) == 2, "空行+hi = 2 显示行");
    CHAR_INFO buf[64];
    get_vline(&h, 80, 0, buf, 64);
    CHECK(atx(buf,0)==' ' && buf[0].Char.UnicodeChar != 0xFFFF, "空逻辑行占一行（空白）");
    get_vline(&h, 80, 1, buf, 64);
    CHECK(atx(buf,0)=='h' && atx(buf,1)=='i', "第二行=hi");
    loghist_free(&h);
    printf("  v1.8.46: 连续回车产生空逻辑行（占一行）\n");
}

static void test_multi_lines_reflow(void) {
    LogHistory h; loghist_init(&h, 100);
    /* 两条逻辑行：第1条 5 字符 "12345"，第2条 5 字符 "abcde"。 */
    CHAR_INFO r1[5]; mkrow(r1, 5, (WCHAR[]){L'1',L'2',L'3',L'4',L'5'}, 5);
    loghist_append_row(&h, r1, NULL, NULL, NULL, 5, 0);
    CHAR_INFO r2[5]; mkrow(r2, 5, (WCHAR[]){L'a',L'b',L'c',L'd',L'e'}, 5);
    loghist_append_row(&h, r2, NULL, NULL, NULL, 5, 0);
    /* 宽 3：每条折 2 行（3+2），共 4 显示行：123 / 45 / abc / de。 */
    CHECK(loghist_visual_lines(&h, 3) == 4, "两条各5字符 宽3 共4显示行");
    CHAR_INFO buf[64];
    const char *expect[] = {"123","45","abc","de"};
    for (int v = 0; v < 4; v++) {
        get_vline(&h, 3, v, buf, 64);
        for (int x = 0; x < (int)strlen(expect[v]); x++)
            if (atx(buf,x) != expect[v][x]) { printf("  [FAIL] 多逻辑行reflow 行%d 列%d: 得%c want %s (line %d)\n", v, x, atx(buf,x), expect[v], __LINE__); failures++; }
    }
    loghist_free(&h);
    printf("  v1.8.46: 多条逻辑行 reflow 后逻辑行边界不串内容\n");
}

int main(void) {
    printf("=== 逻辑行历史 + reflow 引擎回归 (verify_loghist.py) ===\n");
    test_hard_vs_soft_wrap();
    test_reflow_narrow_wide();
    test_cjk_not_split();
    test_emoji_not_split();
    test_ring_eviction();
    test_empty_logical_lines();
    test_multi_lines_reflow();
    if (failures) { printf("  %d FAILURE(S)\n", failures); return 1; }
    printf("  [OK] 逻辑行历史 / 软换行合并 / 宽字符感知 reflow / 环形淘汰 全部通过。\n");
    return 0;
}
'''

def main():
    tmp = tempfile.mkdtemp()
    cpath = os.path.join(tmp, "test_loghist.c")
    with open(cpath, "w") as f:
        f.write(TEST_C)
    binp = os.path.join(tmp, "t.bin")
    cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
           "-Itests/stub", "-Iinclude",
           cpath, "src/loghist.c", "src/utf8.c", "-o", binp]
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        print("Compilation error:", r.stderr)
        return 1
    r = subprocess.run([binp], capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
