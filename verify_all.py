#!/usr/bin/env python3
"""Run every lightweight regression check and propagate failures."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
CHECKS = (
    "verify_picker.py",
    "verify_flow.py",
    "verify_mouse53.py",
    "verify_color8.py",
    "verify_emoji.py",
    "verify_ringbuf_asan.py",
    "verify_screen_state.py",
    "verify_loghist.py",
    "verify_html_clipboard.py",
    "verify_dirty_render.py",
    "verify_dirty_cursor.py",
    "verify_attr_bg.py",
    "verify_copy_colors.py",
    "verify_copy_trailing_bg.py",
    "verify_copy_wide.py",
    "verify_copy_snap.py",
    "verify_copy_step.py",
    "verify_copy_cursor.py",
    "verify_search.py",
    "verify_palette_search.py",
    "verify_input_layout.py",
    "verify_cursor_render.py",
    "verify_menu_settings.py",
    "verify_palette_interaction.py",
    "verify_alignment.py",
    "verify_confirm_exit.py",
    "verify_copy_mode.py",
    "verify_config_theme.py",
    "verify_settings_ui.py",
    "verify_item_color.py",
    "verify_search_box.py",
    "verify_wide_wrap.py",
    "verify_framediff_oom.py",
    "verify_split.py",
    "verify_resize_history.py",
    "verify_scrollbar.py",
    "verify_search_refresh.py",
)


def main() -> int:
    failed: list[str] = []
    for check in CHECKS:
        print(f"\n===== {check} =====", flush=True)
        result = subprocess.run([sys.executable, str(ROOT / check)], cwd=ROOT)
        if result.returncode:
            failed.append(check)

    if failed:
        print("\n失败: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("\n全部回归验证通过。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
