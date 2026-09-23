#!/usr/bin/env python3
"""图形化设置页三个新分类（外观 / 键位 / 行为）的结构验证。

这几页的渲染与鼠标命中共用同一批常量和几何函数，所以最有价值的检查不是在
Python 里把公式再抄一遍（那正是容易和 C 实现漂移的做法），而是卡住
「两边必须用同一个符号」这条约束：

1. input.c 的命中测试不得出现写死的行号 / 列号，必须引用 render.h 的常量
   或 settings_*() 几何函数；
2. 每个可交互元素都要同时存在于渲染与命中两侧；
3. 各页的行号布局不能互相压行（例如再加一套内置主题就会顶到语义色标题）。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RENDER = (ROOT / "src/render.c").read_text(encoding="utf-8")
INPUT = (ROOT / "src/input.c").read_text(encoding="utf-8")
RENDER_H = (ROOT / "include/render.h").read_text(encoding="utf-8")
CONFIG_H = (ROOT / "include/config.h").read_text(encoding="utf-8")
THEME_C = (ROOT / "src/theme.c").read_text(encoding="utf-8")
KEYMAP_C = (ROOT / "src/keymap.c").read_text(encoding="utf-8")
CONFIG_C = (ROOT / "src/config.c").read_text(encoding="utf-8")
TYPES_H = (ROOT / "include/types.h").read_text(encoding="utf-8")

errors: list[str] = []


def check(cond: bool, msg: str) -> None:
    if cond:
        print(f"  [ok] {msg}")
    else:
        errors.append(msg)
        print(f"  [FAIL] {msg}")


def const(name: str) -> int:
    m = re.search(rf"#define\s+{name}\s+(\d+)", RENDER_H)
    if not m:
        errors.append(f"render.h 缺少常量 {name}")
        return -1
    return int(m.group(1))


def func_body(src: str, signature: str) -> str:
    start = src.index(signature)
    depth = 0
    i = src.index("{", start)
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
    raise AssertionError(f"未找到 {signature} 的函数体")


print("== 1) 布局常量互不压行 ==")
theme_row0 = const("SETTINGS_THEME_ROW0")
role_row0 = const("SETTINGS_ROLE_ROW0")
role_rows = const("SETTINGS_ROLE_ROWS")
keys_row0 = const("SETTINGS_KEYS_ROW0")
behavior_row0 = const("SETTINGS_BEHAVIOR_ROW0")
edit_col = const("SETTINGS_KEYS_EDIT_COL")
reset_col = const("SETTINGS_KEYS_RESET_COL")
minus_col = const("SETTINGS_SB_MINUS_COL")
plus_col = const("SETTINGS_SB_PLUS_COL")

theme_count = len(re.findall(r'\{"[\w-]+", \{', THEME_C.split("const ThemeDef g_builtin_themes[] = {", 1)[1]
                             .split("\n};", 1)[0]))
role_count = len(re.findall(r'"[a-z_]+"', re.search(
    r"static const char \*const g_role_names\[TH_ROLE_COUNT\] = \{(.*?)\};", THEME_C, re.S).group(1)))
action_count = len(re.findall(r"\{ACT_\w+,\s*\"[a-z-]+\"", KEYMAP_C))

check(theme_row0 + theme_count - 1 < role_row0 - 2,
      f"{theme_count} 个内置主题（行 {theme_row0}..{theme_row0 + theme_count - 1}）没有顶到语义色标题（行 {role_row0 - 2}）")
check(role_count == role_rows * 2,
      f"{role_count} 个语义角色正好排成 2 列 × {role_rows} 行")
check(keys_row0 > 5, "键位表格从表头下一行开始")
check(behavior_row0 > 4, "行为页开关行不覆盖标题与说明")
# v1.8.44：说明列加宽到 36 后按钮右移（[复位]@92，加按钮宽 5、左侧栏 22+3），
# 表格内容最右约 main_left(25)+92+5 ≈ 122 列；按钮列互不重叠、且落在常见窗口
# （>=130 列）宽度内。
check(edit_col < reset_col and reset_col + 6 < 110, "[改] / [复位] 按钮列不重叠且不越出常见窗口宽度")
check(minus_col + 4 <= plus_col, "scrollback 的 [-] / 数值 / [+] 不重叠")

print("\n== 2) 命中测试不得写死几何 ==")
mouse_body = func_body(INPUT, "void handle_settings_mouse(MOUSE_EVENT_RECORD *me)")
new_pages = mouse_body.split("if (g_settings_nav == SETTINGS_NAV_APPEARANCE)", 1)[1] \
                      .split("if (g_settings_nav == 0)", 1)[0]
for sym in ("settings_theme_row(", "settings_role_row(", "settings_role_col(",
            "settings_keys_entry_at(", "settings_keys_edit_col(", "settings_keys_reset_col(",
            "SETTINGS_BEHAVIOR_ROW0", "SETTINGS_SB_MINUS_COL", "SETTINGS_SB_PLUS_COL"):
    check(sym in new_pages, f"命中测试通过 {sym} 复用渲染侧几何")
menu_page = mouse_body.split("if (g_settings_nav == 0)", 1)[1]
check("settings_menu_btn_col(host_cols, main_left)" in menu_page and
      "settings_menu_show_ud(host_cols, main_left)" in menu_page,
      "菜单项表格的按钮命中同样复用渲染侧收缩几何（窄终端下 [改]/[删] 不会画点分离）")
literal_rows = re.findall(r"r == (\d+)", new_pages)
check(not literal_rows, f"命中测试没有写死行号（发现 {literal_rows}）")
check("settings_sidebar_extra_rows(" in mouse_body and "settings_sidebar_extra_rows(" in RENDER,
      "侧栏三个新入口的行号由 settings_sidebar_extra_rows() 单点提供")

print("\n== 3) 渲染与交互一一对应 ==")
appearance = func_body(RENDER, "static void render_settings_appearance(")
keys = func_body(RENDER, "static void render_settings_keys(")
behavior = func_body(RENDER, "static void render_settings_behavior(")

check("theme_name_at(i)" in appearance and "theme_index()" in appearance,
      "外观页列出全部内置主题并标出当前主题")
check("theme_role_name(role)" in appearance and "theme_role_is_overridden(role)" in appearance,
      "外观页列出 16 个语义角色并标记已自定义项")
check("g_hex_edit_active" in appearance and "g_hex_edit_active" in INPUT,
      "语义色十六进制编辑在渲染与输入两侧都实现")
check("keymap_describe(action" in keys and "keymap_action_is_overridden(action)" in keys,
      "键位页显示实时键位与自定义标记")
check("keymap_prefix_describe(combo" in keys and "keymap_prefix_text()" not in keys,
      "键位页第一行必须显示 Ctrl+B 这种可读写法，而不是 ini 里的 C-b")
check("keymap_action_uses_prefix(action)" in keys and '"[前缀]" : "[直接]"'.replace("'", "") in keys.replace("'", "") and
      "settings_keys_toggle_prefix" in INPUT and "settings_keys_prefix_col(" in RENDER and
      "settings_keys_prefix_col(" in INPUT,
      "键位页没有“是否使用前缀”的切换（渲染 / 键盘 / 鼠标三侧）")
check("search_case_sensitive" in behavior and "SETTINGS_BEHAVIOR_TOGGLES" in RENDER and
      "SETTINGS_BEHAVIOR_TOGGLES" in INPUT,
      "行为页缺少搜索锁定大小写开关，或开关数量仍写死")
check("g_key_capture_active" in keys and "handle_key_capture" in INPUT,
      "键位录制在渲染与输入两侧都实现")
check(all(k in behavior for k in ("mouse", "copy_move_deselect", "confirm_on_exit", "scrollback")),
      "行为页覆盖四个 [general] 开关")
check("copy_on_select" not in behavior and "g_copy_on_select" not in INPUT,
      "已删除的 copy_on_select 设置仍有残留")

key_handlers = ("handle_settings_appearance_key", "handle_settings_keys_key", "handle_settings_behavior_key")
for h in key_handlers:
    check(h in INPUT, f"存在键盘处理函数 {h}")
check(INPUT.count("settings_leave_subpage()") >= 3, "三个分类页都能用 Esc 返回")
check("save_config();" in func_body(INPUT, "static void settings_behavior_toggle(int idx)"),
      "行为开关改动会落盘")
check("save_config();" in func_body(INPUT, "static void handle_key_capture(WORD vk, DWORD ctrl, WCHAR uc)"),
      "键位录制结果会落盘")

print("\n== 4) 导航模型 ==")
for nav in ("SETTINGS_NAV_APPEARANCE", "SETTINGS_NAV_KEYS", "SETTINGS_NAV_BEHAVIOR"):
    check(nav in CONFIG_H and nav in RENDER and nav in INPUT,
          f"{nav} 在配置 / 渲染 / 输入三侧一致定义")
check("settings_nav_at(" in INPUT and "settings_nav_index_of(" in INPUT,
      "Ctrl+↑/↓ 通过统一的侧栏顺序遍历，包含新分类页")
check("keymap_action_at(" in keys and "settings_keys_rows()" in keys and
      "keymap_action_count()" in RENDER,
      f"键位表格按动作表（当前 {action_count} 项）动态生成，不写死条目")

print("\n== 5) 命令面板入口与光标 ==")
for act in ("PALETTE_ACTION_OPEN_APPEARANCE", "PALETTE_ACTION_OPEN_KEYS", "PALETTE_ACTION_OPEN_BEHAVIOR"):
    check(act in RENDER and act in INPUT, f"命令面板存在入口 {act}")
cursor_block = RENDER[RENDER.index("g_hex_edit_active && !g_settings_show_presets"):]
cursor_block = cursor_block[:cursor_block.index("if (g_settings_nav >= 1")]
check("g_settings_nav <= g_chooser_item_count" in RENDER,
      "只有菜单项详情页显示文本光标，三个分类页不会留下错位光标")
check("settings_role_row(g_hex_edit_role)" in cursor_block and
      "settings_pane_row(host_cols, main_left, slot)" in cursor_block.replace("g_mux.host_cols", "host_cols"),
      "颜色十六进制编辑时光标落在对应行（外观页用 settings_role_row，窗格配色页用 settings_pane_row）")

print("\n== 6) 菜单项的启动默认颜色 (v1.8.9) ==")
check("int color;" in TYPES_H, "ChooserItem 必须带 color 字段")
check("extern int g_edit_color;" in CONFIG_H and "int g_edit_color = 0;" in CONFIG_C,
      "菜单项编辑器缺少 g_edit_color 状态")
check("g_edit_color = g_chooser_items[idx].color;" in CONFIG_C and
      "g_chooser_items[idx].color = (g_edit_color >= 0 && g_edit_color <= 8) ? g_edit_color : 0;" in CONFIG_C,
      "编辑器与菜单项之间没有双向搬运颜色")
check('", color=%d"' in CONFIG_C and 'if (_strnicmp(ctext, "color", 5) == 0)' in CONFIG_C,
      "[menu] 段没有读写 color= 字段")
check("int item_color_hit(int left, int col);" in RENDER_H and
      "void render_item_color_row(" in RENDER_H,
      "颜色选择条的几何/渲染没有在 render.h 公开")
check("render_item_color_row(out, bs, &pos, 15, main_left, g_edit_color, f3_sel);" in RENDER,
      "设置页菜单项详情页缺少启动默认颜色选择条")
check("int act_r = 17;" in RENDER and "r == 17" in INPUT,
      "详情页操作按钮行没有随颜色行下移到第 17 行")
check("item_color_hit(main_left, c)" in INPUT,
      "详情页颜色选择条没有鼠标热区")
# v1.8.30：外观页语义颜色网格的 hover 列区间必须与点击命中对齐（c=g_mouse_x+1，
# 点击 c∈[col, col+W-2] ⇔ hover g_mouse_x∈[col-1, col+W-3]）。旧代码上界少 1，
# 鼠标停在格子右半边时不高亮却可点击。
check("g_mouse_x <= col + SETTINGS_ROLE_COL_W - 3" in RENDER,
      "语义颜色网格 hover 列区间与点击命中未对齐（上界错位）")
# v1.8.31：键位页整行 hover 不得覆盖右侧按钮列（[前缀]/[改]/[复位]），否则鼠标
# 停在按钮上时行底色与按钮底色叠加（“文字 hover 带到按钮上”）。按钮高亮独立判断。
check("int keys_on_btn" in RENDER and
      "g_mouse_x < main_left + prefix_col - 1" in RENDER and "int prefix_col = settings_keys_prefix_col(host_cols, main_left)" in RENDER and
      "int row_under_mouse = (g_mouse_y == row - 1);" in RENDER and
      'h_edit = (row_under_mouse' in RENDER and 'h_reset = (show_reset && row_under_mouse' in RENDER,
      "键位页整行 hover 未排除按钮列（文字 hover 会带到 [前缀]/[改]/[复位] 按钮上）")
# 行为页 scrollback 行的 [-]/[+] 同理。
check("int sb_on_btn" in RENDER and
      "g_mouse_x < main_left + SETTINGS_SB_MINUS_COL - 1" in RENDER and
      "h_minus = (sb_row_under_mouse" in RENDER and "h_plus = (sb_row_under_mouse" in RENDER,
      "行为页 scrollback 整行 hover 未排除 [-]/[+] 按钮列")
# 搜索框 Aa/aa 大小写标记必须可 hover/点击（render.c 提供命中几何，input.c 处理点击）。
check("search_box_case_hit(" in RENDER and "search_box_case_hovered(" in RENDER and
      "handle_search_box_mouse" in INPUT and "g_search_case_sensitive = !g_search_case_sensitive" in INPUT,
      "搜索框大小写标记 Aa/aa 没有 hover 高亮 / 点击切换")
check("(g_settings_field + 1) % 4" in INPUT and "(g_settings_field + 3) % 4" in INPUT,
      "Tab 没有把颜色行算成第 4 个字段")
check("g_edit_color = (g_edit_color + 1) % 9" in INPUT and
      "g_edit_color = (g_edit_color + 8) % 9" in INPUT,
      "颜色行不支持 ←/→ 循环选色")
check("g_mux.panes[p].color = (c >= 1 && c <= 8) ? c : 0;" in
      (ROOT / "src/pane.c").read_text(encoding="utf-8"),
      "create_pane_from_item 没有把菜单项颜色应用到新标签页")

if errors:
    print(f"\n设置页 UI 验证失败：{len(errors)} 项", file=sys.stderr)
    raise SystemExit(1)
print("\n设置页 UI 验证通过。")
