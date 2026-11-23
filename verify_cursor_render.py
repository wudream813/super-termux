#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Exercise the production terminal-output cursor boundary calculation.

The last terminal cell is distinct from the single-line input viewport.  In
particular, VT delayed auto-wrap must leave the cursor visible on the written
rightmost cell until the next character arrives.
"""

from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
SOURCE = (ROOT / "src" / "render.c").read_text(encoding="utf-8")


def extract_func(text: str, prefix: str) -> str:
    start = text.find(prefix)
    if start < 0:
        raise RuntimeError(f"function not found: {prefix}")
    brace = text.find("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise RuntimeError(f"unterminated function: {prefix}")


FUNCTION = extract_func(SOURCE, "static int terminal_cursor_position")

PRELUDE = r"""
#include <assert.h>
#include <stdio.h>

typedef struct {
    int cols, rows;
    int cursor_x, cursor_y, cursor_visible;
    int wraparound_pending;
    int session_view;
} ScreenBuffer;
"""

DRIVER = r"""
static void expect(ScreenBuffer *s, int scroll, int session, int session_row, int hr, int hc,
                   int visible, int want_visible, int want_row, int want_col) {
    int row = -1, col = -1;
    s->cursor_visible = visible;
    int got = terminal_cursor_position(s, scroll, session, session_row, hr, hc, &row, &col);
    assert(got == want_visible);
    if (want_visible) {
        assert(row == want_row);
        assert(col == want_col);
    }
}

int main(void) {
    ScreenBuffer s = {5, 3, 4, 2, 1, 0, 0};

    /* Normal last cell and delayed-wrap last cell must both render at row 4,
     * column 5 (the pane starts below the one-row tab bar). */
    expect(&s, 0, 0, -1, 3, 5, 1, 1, 4, 5);
    s.wraparound_pending = 1;
    expect(&s, 0, 0, -1, 3, 5, 1, 1, 4, 5);

    /* Defensive clamp: a transient one-past column still uses the final
     * visible cell rather than hiding the cursor. */
    s.cursor_x = 5;
    expect(&s, 0, 0, -1, 3, 5, 1, 1, 4, 5);
    s.cursor_y = 1;
    expect(&s, 0, 0, -1, 3, 5, 1, 1, 3, 5);

    /* v2.4.1：会话视图（恢复历史 + 实时内容）按 screen_reflow_view 算出的行定位 ——
     * 内容不足一屏时视图贴顶，光标就在 prompt 那一行，不再钉在最底行。 */
    s.cursor_y = 0;
    expect(&s, 0, 1, 2, 3, 5, 1, 1, 4, 5);     /* 视口第 2 行（0 基）+2 = 宿主第 4 行 */
    expect(&s, 0, 1, -1, 3, 5, 1, 1, 4, 5);    /* 未知行号才退回最底行 */

    /* A scrolled or hidden terminal deliberately hides the native cursor. */
    expect(&s, 1, 0, -1, 3, 5, 1, 0, 0, 0);
    expect(&s, 0, 0, -1, 3, 5, 0, 0, 0, 0);
    puts("terminal cursor regression passed: output last cell remains visible.");
    return 0;
}
"""


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        c_file = os.path.join(tmp, "cursor_render.c")
        exe = os.path.join(tmp, "cursor_render")
        Path(c_file).write_text(PRELUDE + "\n" + FUNCTION + "\n" + DRIVER,
                                encoding="utf-8")
        result = subprocess.run(
            ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", c_file, "-o", exe],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            return 1
        result = subprocess.run([exe], capture_output=True, text=True)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            return 1
        print(result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
