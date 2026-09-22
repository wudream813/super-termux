#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regression tests for the command-palette terminal chooser matching.

The terminal chooser must prefer a display-name match over incidental command
text.  This extracts the actual matching/filtering functions from render.c,
then supplies a small deterministic item table so one-letter and short,
unique searches are tested against the production algorithm rather than a
separate Python reimplementation.
"""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
# ★ 平台差异（MinGW 给无扩展名的 -o 补 .exe / MinGW 不支持 -fsanitize）
#   统一收在 tests/hbuild.py，不要在这里各写一份 sys.platform 判断。
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import hbuild  # noqa: E402


ROOT = Path(__file__).resolve().parent
SOURCE = (ROOT / "src" / "render.c").read_text(encoding="utf-8")


def extract_func(text: str, prefix: str) -> str:
    start = text.find(prefix)
    if start < 0:
        raise RuntimeError(f"function not found: {prefix}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"function body not found: {prefix}")
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise RuntimeError(f"unterminated function: {prefix}")


MATCHING = "\n\n".join(
    extract_func(SOURCE, prefix)
    for prefix in (
        "static int palette_strcasestr",
        "static int palette_strcase_prefix",
        "static int palette_strcase_equal",
        "static int palette_match_score",
        "int palette_filter_cmds",
    )
)

PRELUDE = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PALETTE_PAGE_ROOT = 0, PALETTE_PAGE_NEW_TERMINAL = 3 };

typedef struct {
    const char *id;
    const char *title;
    const char *desc;
    const char *shortcut;
    int action;
    int value;
    int number;
    int color;
} PaletteItemInfo;

static PaletteItemInfo items[] = {
    {"cmd",  "Command Prompt", "cmd.exe",       "cmd.exe",       0, 0, 1, 1},
    {"ps",   "PowerShell",     "powershell.exe", "powershell.exe", 0, 1, 2, 2},
    {"wsl",  "WSL",            "wsl.exe",        "wsl.exe",        0, 2, 3, 3},
    {"py",   "Python",         "python.exe",     "python.exe",     0, 3, 4, 4},
};
static int item_count = 4;

int palette_item_count(int page) {
    return page == PALETTE_PAGE_NEW_TERMINAL ? item_count : 2;
}

int palette_item_info(int page, int index, PaletteItemInfo *out) {
    if (!out || index < 0 || index >= palette_item_count(page)) return 0;
    *out = items[index];
    if (page == PALETTE_PAGE_ROOT && index == 0) {
        out->id = "open-settings";
        out->title = "Open Settings";
        out->desc = "Open the graphical settings page";
        out->shortcut = "Enter";
    }
    if (page == PALETTE_PAGE_ROOT && index == 1) {
        out->id = "settings";
        out->title = "Settings";
        out->desc = "Edit configuration";
        out->shortcut = "Enter";
    }
    return 1;
}
"""

DRIVER = r"""
static void expect_indices(int page, const char *query, const int *want, int want_count) {
    int got[8] = {0};
    int n = palette_filter_cmds(page, got, 8, query);
    if (n != want_count) {
        fprintf(stderr, "query %s: got count %d, want %d\\n", query, n, want_count);
        assert(n == want_count);
    }
    for (int i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            fprintf(stderr, "query %s: got[%d]=%d, want=%d\\n", query, i, got[i], want[i]);
            assert(got[i] == want[i]);
        }
    }
}

int main(void) {
    int one[] = {2};
    int py[] = {3};
    int p_order[] = {1, 3, 0};
    int all[] = {0, 1, 2, 3};
    int cmd[] = {0};
    int settings_exact_first[] = {1, 0};

    /* A short, unique name query must beat incidental command-line matches;
     * 'ws' selects WSL in two keystrokes, while the genuinely ambiguous 'w'
     * keeps both display-name matches. */
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "ws", one, 1);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "w", (int[]){2, 1}, 2);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "py", py, 1);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "p", p_order, 3);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "cmd", cmd, 1);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "exe", all, 4);
    expect_indices(PALETTE_PAGE_NEW_TERMINAL, "", all, 4);

    /* Exact title matches sort before a longer title containing the same
     * query on ordinary palette pages. */
    expect_indices(PALETTE_PAGE_ROOT, "settings", settings_exact_first, 2);

    puts("palette search regression passed: short name matches and command fallback are stable.");
    return 0;
}
"""


def main() -> int:
    code = PRELUDE + "\n" + MATCHING + "\n" + DRIVER
    with tempfile.TemporaryDirectory() as tmp:
        c_file = os.path.join(tmp, "palette_search.c")
        exe = hbuild.exe_path(tmp, "palette_search")
        Path(c_file).write_text(code, encoding="utf-8")
        result = subprocess.run(
            [hbuild.gcc(), "-std=c99", "-O2", "-Wall", "-Wextra", c_file, "-o", exe],
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
