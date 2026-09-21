#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_framediff_oom.py — BUG-11 回归：framediff 扩容时 rows/dirty 不同步的堆溢出。

framediff_begin_frame() 在终端行数变大时扩容两个并行数组：
  - rows  : FrameChunk* （每项 24 字节）
  - dirty : char*       （每行 1 字节）
两者共用 rows_cap 容量。旧代码只在 rows（nr）realloc 成功时就提升 rows_cap，
若 rows 成功而 dirty（nd）因 OOM 失败，rows_cap 虚高到 newcap、row_count 随之
变大，但 dirty 仍是旧的小缓冲——紧接着 `for(i<row_count) fd->dirty[i]=0` 越界写。

本脚本用 gcc + ASan + `--wrap=realloc` 故障注入：让小字节（dirty，1 字节/行）
的 realloc 返回 NULL、大字节（rows）正常，断言：
  1. 不发生 heap-buffer-overflow（ASan 不报错，进程退出码 0）；
  2. 扩容失败时容量不虚高（rows_cap/row_count 维持旧值，安全退化）；
  3. 下一帧（注入解除）扩容最终成功。

变异：把 `if (nr && nd) fd->rows_cap = newcap;` 改回 `if (nr) ...`，ASan 立即报
heap-buffer-overflow（本脚本返回非 0）。
"""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FRAMEDIFF_C = ROOT / "src" / "framediff.c"

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framediff.h"

/* ★ 故障注入原来是 `-Wl,--wrap=realloc` + __wrap_realloc/__real_realloc。
 * 那是 GNU ld 专有的，Apple 的 ld64 直接报 `ld: unknown options: --wrap=realloc`
 * —— CI 的 macOS 作业就栽在这儿。
 *
 * 改成可移植做法：本文件和 framediff.c 都用 -Drealloc=fio_realloc 编译，于是
 * 它们里面所有 realloc 调用都指向下面这个；真正的 realloc 由另一个【不带这个
 * 宏】的小文件 real_realloc.c 转发过来。三个平台同一条路径，本地就能验。
 * （不用 __asm__("realloc") 那种符号改名：Mach-O 上 C 符号带前导下划线，
 *   写死 "realloc" 会链接不到。） */
static int g_fail_small_realloc = 0;
void *fio_call_real_realloc(void *p, size_t n);
void *fio_realloc(void *p, size_t n) {
    /* dirty 按 1 字节/行扩容（32/64/... 小请求）；rows 每项 FrameChunk 24 字节
     * （64*24=1536），chunk 数据缓冲起步 256。只让 <=128 的小请求失败。 */
    if (g_fail_small_realloc && n > 0 && n <= 128) {
        fprintf(stderr, "  [inject] realloc(%zu) -> NULL\n", n);
        return NULL;
    }
    return fio_call_real_realloc(p, n);
}

int main(void) {
    FrameDiff fd;
    framediff_init(&fd);
    char buf[8192];
    char *out;
    size_t bp, need;

    /* 帧1：32 行（初始容量），正常。 */
    framediff_begin_frame(&fd, 32);
    bp = 0;
    for (int r = 0; r < 32; r++)
        bp += (size_t)snprintf(buf + bp, sizeof(buf) - bp, "\x1b[%d;1Hrow%d", r + 2, r);
    framediff_scan(&fd, buf, bp);
    need = framediff_emit(&fd, NULL, 0);
    out = malloc(need + 16); framediff_emit(&fd, out, need + 16); free(out);
    printf("帧1 rows_cap=%d row_count=%d\n", fd.rows_cap, fd.row_count);

    /* 帧2：扩到 64 行，注入 dirty realloc 失败。 */
    g_fail_small_realloc = 1;
    framediff_begin_frame(&fd, 64);
    g_fail_small_realloc = 0;
    printf("帧2(注入) rows_cap=%d row_count=%d\n", fd.rows_cap, fd.row_count);
    if (fd.rows_cap >= 64 || fd.row_count >= 64) {
        printf("FAIL: dirty 扩容失败但容量虚高 rows_cap=%d row_count=%d\n",
               fd.rows_cap, fd.row_count);
        return 2;
    }
    bp = 0;
    for (int r = 0; r < fd.row_count; r++)
        bp += (size_t)snprintf(buf + bp, sizeof(buf) - bp, "\x1b[%d;1Hline%d", r + 2, r);
    framediff_scan(&fd, buf, bp);
    need = framediff_emit(&fd, NULL, 0);
    out = malloc(need + 16); framediff_emit(&fd, out, need + 16); free(out);

    /* 帧3：注入解除，扩容应最终成功容纳 64 行。 */
    framediff_begin_frame(&fd, 64);
    printf("帧3 rows_cap=%d row_count=%d\n", fd.rows_cap, fd.row_count);
    if (fd.rows_cap < 64 || fd.row_count != 64) {
        printf("FAIL: 注入解除后扩容未恢复 rows_cap=%d row_count=%d\n",
               fd.rows_cap, fd.row_count);
        return 3;
    }
    bp = 0;
    for (int r = 0; r < 64; r++)
        bp += (size_t)snprintf(buf + bp, sizeof(buf) - bp, "\x1b[%d;1Hz%d", r + 2, r);
    framediff_scan(&fd, buf, bp);
    need = framediff_emit(&fd, NULL, 0);
    out = malloc(need + 16); framediff_emit(&fd, out, need + 16); free(out);

    framediff_free(&fd);
    printf("FRAMEDIFF-OOM OK\n");
    return 0;
}
"""


def main() -> int:
    print("=== framediff 扩容 OOM 故障注入 (verify_framediff_oom.py) ===")
    with tempfile.TemporaryDirectory() as td:
        harness = Path(td) / "oom.c"
        exe = Path(td) / "oom.bin"
        harness.write_text(HARNESS, encoding="utf-8")
        # 这个文件【不能】带 -Drealloc=fio_realloc，否则它自己也变成递归。
        real = Path(td) / "real_realloc.c"
        real.write_text(
            "#include <stdlib.h>\n"
            "void *fio_call_real_realloc(void *p, size_t n) { return realloc(p, n); }\n",
            encoding="utf-8")
        realobj = Path(td) / "real_realloc.o"
        cp0 = subprocess.run(["gcc", "-O1", "-c", str(real), "-o", str(realobj)],
                             capture_output=True, text=True)
        if cp0.returncode != 0:
            print(cp0.stderr, file=sys.stderr)
            print("FAIL: real_realloc.c 编译失败", file=sys.stderr)
            return 1
        cp = subprocess.run(
            ["gcc", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer",
             "-Drealloc=fio_realloc", "-I" + str(ROOT / "include"),
             str(harness), str(FRAMEDIFF_C), str(realobj), "-o", str(exe), "-lm"],
            capture_output=True, text=True)
        if cp.returncode != 0:
            print(cp.stderr, file=sys.stderr)
            print("FAIL: harness 编译失败", file=sys.stderr)
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout)
        if run.stderr.strip():
            print(run.stderr, file=sys.stderr)
        if run.returncode != 0:
            print("FAIL: ASan 报错或容量断言失败（BUG-11 回归）", file=sys.stderr)
            return 1
        if "heap-buffer-overflow" in run.stderr or "ERROR: AddressSanitizer" in run.stderr:
            print("FAIL: 检测到 AddressSanitizer 越界", file=sys.stderr)
            return 1
        if "FRAMEDIFF-OOM OK" not in run.stdout:
            print("FAIL: harness 未正常完成", file=sys.stderr)
            return 1
    print("  [OK] dirty 扩容失败时容量不虚高、无越界写，注入解除后恢复扩容。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
