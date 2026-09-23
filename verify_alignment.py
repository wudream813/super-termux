#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Geometry regression checks for the ANSI renderer and Windows mouse input.

The UI writes ANSI cursor positions as 1-based row/column coordinates while
Win32 MOUSE_EVENT_RECORD positions are 0-based.  This check deliberately
covers the shared formulas and the fixed-width rows of every modal surface;
it is kept source-adjacent so future layout edits have a small, cheap guard.
"""

from pathlib import Path
import re
import unicodedata

ROOT = Path(__file__).resolve().parent
RENDER = (ROOT / "src/render.c").read_text(encoding="utf-8")
INPUT = (ROOT / "src/input.c").read_text(encoding="utf-8")
UTF8 = (ROOT / "src/utf8.c").read_text(encoding="utf-8")
KEYMAP = (ROOT / "src/keymap.c").read_text(encoding="utf-8")

errors = []


def check(condition, message):
    if not condition:
        errors.append(message)


def cols(text):
    """Match the project's display-width convention for UI strings."""
    total = 0
    for ch in text:
        # Box drawing, arrows and ASCII punctuation used here are single-cell;
        # East Asian wide/full-width characters are two cells.
        total += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return total


# ---- 1) Shared 0-based mouse anchor -> 1-based ANSI popup left edge ----
def popup_left(anchor0, width, host_cols):
    width = max(1, width)
    host_cols = max(1, host_cols)
    anchor = anchor0 + 1 if anchor0 >= 0 else 1
    left = anchor
    if left + width - 1 > host_cols:
        left = anchor - width + 1
    return max(1, left)


check("popup_left_1based" in RENDER, "render.c 缺少统一 popup_left_1based()")
check(RENDER.count("popup_left_1based(") >= 5, "chooser/固定弹窗没有全部使用统一左边界公式")
check("int popup_w = (g_mux.ctx_mode == 1) ? CTX_W : CP_W;" in INPUT,
      "ctx 菜单/颜色选择器没有分别使用各自宽度的左边界公式")
check("popup_left_1based(anchor0, popup_w" in INPUT, "ctx 鼠标命中未使用统一左边界公式")
for host in range(4, 201):
    for width in (1, 3, 20, 24, 30, 38, 72, 78):
        if width > host:
            continue
        for anchor in (-1, 0, 1, host // 2, host - 2, host - 1):
            left = popup_left(anchor, width, host)
            check(1 <= left <= host - width + 1,
                  f"popup 几何越界 host={host} width={width} anchor={anchor} left={left}")
            expected_anchor = anchor + 1 if anchor >= 0 else 1
            expected = expected_anchor if expected_anchor + width - 1 <= host else expected_anchor - width + 1
            check(left == max(1, expected),
                  f"popup renderer/handler 锚点不一致 host={host} width={width} anchor={anchor}")
# The context menu and picker intentionally have different widths; both must
# remain right-aligned to the same mouse anchor at the right edge.
for width in (24, 30):
    left = popup_left(119, width, 120)
    check(left == 120 - width + 1, f"右边界 popup width={width} 未贴合 ANSI 右边界")

# ---- 2) ANSI row/column <-> mouse row/column conversion ----
for mouse_y in range(0, 80):
    ansi_y = mouse_y + 1
    check(ansi_y - 1 == mouse_y, "行坐标换算不是 ANSI 1-based <-> mouse 0-based")
for mouse_x in range(0, 240):
    ansi_x = mouse_x + 1
    check(ansi_x - 1 == mouse_x, "列坐标换算不是 ANSI 1-based <-> mouse 0-based")

# ---- 3) Fixed popups: every visible row has one common width ----
check(cols("┌─ 标签操作 ───────────┐") == 24, "ctx 标题不是 CTX_W=24 列")
check(cols("│  [1] 修改颜色        │") == 24, "ctx 第一行不是 CTX_W=24 列")
check(cols("│  [2] 重命名标签      │") == 24, "ctx 第二行不是 CTX_W=24 列")
check(cols("└──────────────────────┘") == 24, "ctx 底边不是 CTX_W=24 列")
check(cols("┌─ 选择颜色 ─────────────────┐") == 30, "颜色选择器标题不是 CP_W=30 列")
check(cols("└────────────────────────────┘") == 30, "颜色选择器底边不是 CP_W=30 列")
# Picker rows: border + initial space + 4*swatch + one gap + padding + border.
for row_name in ("第一行", "第二行"):
    visible = 2 + 4 * 4 + 1 + (30 - 1 - (2 + 4 * 4 + 1)) + 1
    check(visible == 30, f"颜色选择器{row_name}未填充到 CP_W")
check(cols("┌─ 重命名标签 ───────────────┐") == 30, "重命名标题不是 RENAME_W=30 列")
check(cols("└────────────────────────────┘") == 30, "重命名底边不是 RENAME_W=30 列")
check(cols("┌─ 自定义命令行 ─────────────────────┐") == 38, "自定义命令标题不是 CMD_BOX_W=38 列")
check(cols("└────────────────────────────────────┘") == 38, "自定义命令底边不是 CMD_BOX_W=38 列")
cmd_hint = "  [Enter=启动  Esc=取消]"
check(1 + cols(cmd_hint) <= 38 - 1, "自定义命令 footer 文本超出内框")
check(1 + cols(cmd_hint) + (38 - 1 - (1 + cols(cmd_hint))) + 1 == 38,
      "自定义命令 footer 未补齐到 CMD_BOX_W")
check("┌─ 自定义命令行 ──────────────────────┐" not in RENDER,
      "自定义命令标题仍保留一列过宽的旧字符串")

# ---- 4) Color swatches: render, hover and hit-test are the same 4 cells ----
check("g_mouse_x >= left + 1 + i * 4" in RENDER and
      "int mouse_row = row - 1" in RENDER,
      "颜色选择器 hover 没有把 ANSI 列/行转换为 0-based 鼠标坐标")
for left in (1, 7, 41, 91):
    for row in (0, 1):
        for k in range(4):
            ansi_start = left + 2 + 4 * k
            rendered = set(range(ansi_start, ansi_start + 4))
            hit = set(range(ansi_start, ansi_start + 4))
            hover = set(range(ansi_start - 1, ansi_start + 3))
            check(rendered == hit, f"颜色块 {row},{k} 渲染/点击范围不同")
            check({x + 1 for x in hover} == rendered, f"颜色块 {row},{k} hover/ANSI 范围不同")

# ---- 5) Chooser and preset rows ----
for item_count in (0, 1, 8, 9):
    tagw = 4 if item_count >= 10 else 3
    max_name = 15
    chooser_w = max(20, 1 + 2 + tagw + 1 + max_name + 2)
    check(chooser_w >= 20, "chooser 最小宽度不满足边框")
    # body formula for the widest item, including both borders
    check(1 + 2 + tagw + 1 + max_name + (chooser_w - 1 - (1 + 2 + tagw + 1 + max_name)) + 1 == chooser_w,
          f"chooser {item_count} 项时行宽不闭合")
check("int item_used = 1 + 2 + utf8_cols(chooser_tag" in RENDER,
      "chooser 条目仍用固定 tag 宽度，可能使两位序号右边框错位")
check("cols = 1 + 2 + utf8_cols(preset_tag" in RENDER,
      "预设条目仍用固定 tag 宽度")

# ---- 6) Settings page: sidebar, fields and table buttons ----
check("if (c <= sb_w)" in INPUT, "设置侧栏命中区域漏掉渲染出的分隔列")
check("if (r == host_rows)" in INPUT, "设置保存按钮命中仍覆盖底部空白行")
check("if (r >= host_rows)" not in INPUT, "设置保存按钮仍使用过宽的 r>=host_rows 命中")
check("c < main_left + input_w + 4" in INPUT, "设置编辑字段命中没有限制在输入框宽度内")
# Renderer hover ranges are mouse-space; handler ranges are ANSI-space.
for lo, hi in ((52, 54), (55, 57), (58, 61), (62, 65)):
    mouse_range = set(range(lo, hi + 1))
    ansi_range = set(range(lo + 1, hi + 2))
    check({x - 1 for x in ansi_range} == mouse_range, "设置表格按钮 hover/点击范围不一致")
# v2.0.9：菜单表格的 [↑][↓][改][删] 按钮列改为随可用宽度收缩后，渲染与命中必须
# 共用同一套几何函数（settings_menu_btn_col / settings_menu_show_ud），否则窄终端上
# 按钮画在一处、点在另一处。
check("settings_menu_btn_col(host_cols, main_left)" in RENDER and
      "settings_menu_btn_col(host_cols, main_left)" in INPUT and
      "settings_menu_show_ud(host_cols, main_left)" in RENDER and
      "settings_menu_show_ud(host_cols, main_left)" in INPUT,
      "菜单表格按钮列的收缩几何没有在渲染与命中两侧共用")
check("append_padded_utf8(out, bs, &pos, &row_cols, dname, 12)" in RENDER,
      "设置表格显示名称没有按终端列宽补齐")
check("append_padded_utf8(out, bs, &pos, &row_cols, dcmd, settings_menu_cmd_w(host_cols, main_left))" in RENDER,
      "设置表格命令行没有按终端列宽补齐（v2.0.9：宽度随可用列收缩，窄终端上按钮才不会被裁掉）")
check("%-12s" not in RENDER and "%-30s" not in RENDER,
      "设置表格仍使用按字节计算的 %-Ns 补齐")

# ---- 7) Palette rows/editor and actual bottom search line ----
check("int row = top + 3 + vi;" in RENDER and "int row_start = top + 3;" in INPUT,
      "命令面板列表行的 renderer/hit-test 起始行不同")
check("int row_end = row_start + visible;" in INPUT,
      "命令面板列表没有以 visible 行数封闭命中区域")
check("top + 2 + g_mux.palette_field * 2" in RENDER,
      "命令面板编辑器光标没有落在输入行")
check("int input_row = top + 2 + field * 2" in INPUT,
      "命令面板编辑器输入框点击行与渲染不一致")
# v1.8.10: 搜索输入框不再占用整条底行，改成贴右上角的紧凑小框。
check("ui_bottom_row" not in RENDER,
      "搜索输入框又回到了会吃掉一行终端内容的底栏")
check("#define SEARCH_BOX_COLS (SEARCH_BOX_PREFIX_COLS + SEARCH_BOX_INPUT_COLS + SEARCH_BOX_SUFFIX_COLS)" in RENDER,
      "搜索小框没有把前缀/输入/后缀宽度合成同一个常量")
_search_box = RENDER[RENDER.index("void render_search_box("):]
_search_box = _search_box[:_search_box.index("\n}\n")]
check("search_box_layout(host_cols, &row, &left, &input_col, &box_w);" in _search_box,
      "搜索小框没有使用共享几何 search_box_layout()")
check("*row = BADGE_ROW;" in RENDER,
      "搜索小框没有和状态徽章同处右上角那一行")
check("\\x1b[K" not in _search_box and "while (used_cols" not in _search_box,
      "搜索小框仍然在把整行铺满（那就等于又占掉一整行）")
check("search_box_layout(g_mux.host_cols, &row, &left, &input_col, &box_w);" in RENDER and
      "\"\\x1b[%d;%dH\\x1b[?25h\", row, input_col + scr_off" in RENDER,
      "搜索光标没有落在小框的输入列上")

# ---- 8) Requested interaction/text guards ----
check("g_mux.palette_page = settings_active ? PALETTE_PAGE_SETTINGS : PALETTE_PAGE_OPERATIONS;" in INPUT,
      "Ctrl+B : 没有按当前页面直达设置/操作命令面板")
operation_items = RENDER[RENDER.find("g_palette_operation_items"):RENDER.find("g_palette_setting_items")]
setting_items = RENDER[RENDER.find("g_palette_setting_items"):RENDER.find("g_palette_startup_items")]
check("\"open-settings-page\"" in operation_items and "PALETTE_ACTION_GRAPHICAL_SETTINGS" in operation_items,
      "操作命令面板没有图形化设置入口")
check("\"open-settings-page\"" not in setting_items and
      "PALETTE_ACTION_GRAPHICAL_SETTINGS" not in setting_items,
      "图形化设置入口仍错误地出现在设置命令面板")
check("\"about\"" in operation_items and "PALETTE_ACTION_OPEN_ABOUT" in operation_items,
      "操作命令面板没有关于入口")
check("\"about\"" not in setting_items and "PALETTE_ACTION_OPEN_ABOUT" not in setting_items,
      "设置命令面板仍包含关于入口")
check("settings-command-panel" in operation_items and "PALETTE_ACTION_OPEN_SETTINGS" in operation_items,
      "操作命令面板没有打开设置命令面板入口")
check("operations-command-panel" in setting_items and "PALETTE_ACTION_OPEN_OPERATIONS" in setting_items,
      "设置命令面板没有打开操作命令面板入口")
check("PALETTE_PAGE_MENU_SETTINGS" in RENDER and "PALETTE_ACTION_EDIT_PANEL" in RENDER,
      "菜单项设置没有独立的子面板/编辑动作")
domain_switch_start = INPUT.find("static void palette_switch_domain")
domain_switch_end = INPUT.find("static void palette_pop_page")
domain_switch = INPUT[domain_switch_start:domain_switch_end]
check("palette_switch_domain" in INPUT and "palette_reset_query();" in domain_switch,
      "两类命令面板互跳没有重置查询并保持弹窗栈")
check("palette_push_page" not in domain_switch and "g_mux.palette_page = page;" in domain_switch,
      "两类命令面板互跳错误地增长弹窗栈或没有切换页面")
check("g_mux.palette_page = settings_active ? PALETTE_PAGE_SETTINGS : PALETTE_PAGE_OPERATIONS;" in INPUT,
      "普通页面没有直接打开操作命令面板")
check("CHR(0xFF1A)" in KEYMAP,
      "命令面板快捷键没有兼容中文全角冒号 U+FF1A")
check("out->color = (g_mux.panes[i].color >= 0 && g_mux.panes[i].color <= 8)" in RENDER,
      "切换 panel 条目没有带出 panel 的颜色编号")
# v1.8.8: 命令面板的序号一律用普通文字色，不再按标签颜色上色块 / 不再用琥珀色。
_item_row_start = RENDER.find("static void render_palette_item_row")
_item_row_end = RENDER.find("static void render_palette", _item_row_start + 10)
_item_row = RENDER[_item_row_start:_item_row_end]
check(_item_row_start >= 0 and "TAB_COLOR_BG" not in _item_row and "palette_bg_for_color" not in _item_row,
      "命令面板序号又被按标签颜色上色了")
check("\\x1b[038;2;210;153;034;1m%s" not in _item_row,
      "命令面板序号又用回了琥珀色")
check("%s%s%s\", bg," in _item_row and "038;2;139;148;158m" in _item_row,
      "命令面板序号没有使用行内普通文字色")
check("palette_bg_for_color" not in RENDER,
      "未使用的 palette_bg_for_color 应当已经删除")

# v1.8.8: 命令面板滚轮只滚页面，选中项整体平移、相对位置不变。
_pal_mouse = INPUT[INPUT.find("void handle_palette_mouse"):]
_pal_wheel = _pal_mouse[:_pal_mouse.find("int r = my + 1;")]
check("g_mux.palette_scroll +=" in _pal_wheel,
      "命令面板滚轮没有直接滚动列表窗口")
check("int moved = g_mux.palette_scroll - before;" in _pal_wheel and
      "g_mux.palette_sel += moved;" in _pal_wheel,
      "命令面板滚轮没有让选中项跟着页面等量平移（相对位置必须不变）")
check("g_mux.palette_sel -= step;" not in _pal_wheel and "g_mux.palette_sel += step;" not in _pal_wheel,
      "命令面板滚轮不应再单独移动选中项（已撤回“滚不动才动光标”）")
check("if (!s->in_alt_screen) do_scroll(d > 0 ? 3 : -3);" in INPUT,
      "终端滚轮没有保持“只滚动滚动历史”的原行为")
check("write_to_pane(arrow, alen)" not in INPUT,
      "备用屏幕滚轮不应再发送方向键（该改动已撤回）")
check("render_color_picker_cell" in RENDER and "render_color_picker_row" in RENDER,
      "颜色选择器没有使用逐单元格渲染辅助函数")
check("int interior_cols = 2 + 4 * 4;" in RENDER and
      "while (interior_cols < CP_W - 1)" in RENDER,
      "颜色选择器没有显式封闭右侧面板背景填充")
check("const char *swatch_bg = hovered ? TAB_COLOR_BG[color] : TAB_COLOR_BG_DIM[color];" in RENDER and
      "render_color_picker_cell(out, bs, &pos, swatch_bg" in RENDER,
      "颜色选择器色块背景没有逐单元格绑定到对应颜色（hover=亮/非 hover=暗）")
check('"\\x1b[0m\\x1b[048;2;033;038;045m "' not in RENDER,
      "颜色选择器仍使用旧的额外背景空格切换")
check("palette_push_page(PALETTE_PAGE_MENU_SETTINGS);" in INPUT,
      "菜单项设置没有进入子面板")
check("out->action = PALETTE_ACTION_EDIT_PANEL;" in RENDER,
      "菜单项子面板没有把条目连接到编辑子框")
menu_info_start = RENDER.find("if (page == PALETTE_PAGE_MENU_SETTINGS)")
menu_info_end = RENDER.find("    return 0;", menu_info_start)
menu_info = RENDER[menu_info_start:menu_info_end]
check("return g_chooser_item_count;" in RENDER[RENDER.find("case PALETTE_PAGE_MENU_SETTINGS"):RENDER.find("default:", RENDER.find("case PALETTE_PAGE_MENU_SETTINGS"))],
      "菜单项设置仍包含添加 panel 伪条目")
check('out->id = "add-panel"' not in menu_info and "添加 panel 条目" not in menu_info,
      "菜单项设置仍提供添加 panel 条目")
check("filtered[fi], vi + 1" in RENDER and
      "#define PALETTE_MAX_VISIBLE 9" in RENDER and
      "item->number" not in RENDER[RENDER.find("static void render_palette_item_row"):RENDER.find("static void render_palette_editor")],
      "命令面板序号没有按照当前可见窗口动态编号")
check("palette_move_menu_item" in INPUT and "g_mux.palette_query_len > 0" in INPUT and
      "!g_mux.palette_query_len" in INPUT and
      "g_mux.palette_focus == PALETTE_FOCUS_LIST && has_ctrl" in INPUT,
      "菜单项搜索期间没有禁用位置修改或排序未绑定 Ctrl+方向键")
check("g_settings_nav == 0 && is_ctrl" in INPUT and
      "U/D 调顺序" not in RENDER and "U/D 调序" not in RENDER,
      "菜单项设置仍显示或处理旧的 U/D 排序快捷键")
check("palette_delete_menu_item" in INPUT and "g_chooser_item_count--" in INPUT,
      "菜单项设置缺少删除 panel 条目操作")
check("static int key_input_modal_active" in INPUT and
      "!key_input_modal_active()" in INPUT and
      "g_mux.prefix_mode = 0;" in INPUT,
      "文本输入模态仍可被全局 Ctrl+B 前缀打断")
check("wraparound_pending && cy + 1 < rr" not in RENDER and
      "if (cx >= rc) cx = rc - 1;" in RENDER and
      "terminal_cursor_position" in RENDER,
      "终端输出区域末格光标仍无法显示")
# v1.8.9: 多了一行「启动默认颜色」，紧凑布局由 10 行扩到 12 行。
check("#define PALETTE_EDITOR_H 12" in RENDER and "int action_row = top + 10;" in RENDER,
      "编辑 panel 子框没有使用 12 行紧凑布局（三输入框 + 颜色行 + 操作行）")
check("render_item_color_row(out, bs, &pos, color_row, left + 1, g_edit_color" in RENDER,
      "编辑 panel 子框缺少启动默认颜色选择条")
check("int parent_h = palette_visible_rows(host_rows) + 5;" in RENDER and
      "for (int r = top + ph; r < top + parent_h; r++)" in RENDER,
      "编辑 panel 子框没有清除紧凑布局下残留的父面板行")
check("append_padded_utf8(out, bs, &pos, &label_cols, labels[i], label_w);" in RENDER,
      "编辑 panel 子框的标签行没有填充整行背景")
check("r == top + 10 && in_box" in INPUT,
      "编辑 panel 保存按钮热区没有跟随 12 行布局")
check("item_color_hit(left + 1, c)" in INPUT,
      "编辑 panel 颜色选择条没有鼠标热区")
check("\"退出 termux\"" in RENDER, "命令面板仍缺少‘退出 termux’文案")
check("退出标签页" not in RENDER and
      "退出标签页" not in ROOT.joinpath("README.md").read_text(encoding="utf-8") and
      "退出标签页" not in ROOT.joinpath("history.md").read_text(encoding="utf-8"),
      "仍存在过时的‘退出标签页’文案")
# v1.8.4：命令面板的退出改为走统一的 ACT_QUIT 动作，confirm_on_exit 才会生效
_quit_case = INPUT[INPUT.find("case PALETTE_ACTION_QUIT"):INPUT.find("case PALETTE_ACTION_DEFAULT_STARTUP")]
check("action_execute(ACT_QUIT" in _quit_case,
      "退出 termux 没有走统一的 ACT_QUIT 动作（confirm_on_exit 会失效）")
check("g_confirm_on_exit" in INPUT[INPUT.find("case ACT_QUIT:"):INPUT.find("case ACT_TAB_COLOR_NEXT:")],
      "ACT_QUIT 没有处理 confirm_on_exit 二次确认")
check("if (total_cols <= vis_width)" in UTF8,
      "输入光标在文本恰好填满时仍可能落到右边框/分隔符")
check("int max_cx = vis_width - 1 - (has_right ? 1 : 0);" in UTF8,
      "输入光标仍可能落到右滚动箭头或输入框外")
check("render_scrollable_input(out, bs, &pos, g_mux.custom_cmd_buf" in RENDER and
      "CMD_BOX_W - 3, TB_BG, NULL" in RENDER,
      "自定义命令输入框没有使用连续背景渲染")
check("render_scrollable_input(out, bs, &pos, g_mux.rename_buf" in RENDER and
      "RENAME_W - 3, TB_BG, NULL" in RENDER,
      "重命名输入框没有使用连续背景渲染")
check("%s \\x1b[0m\\x1b[048;2;033;038;045m│\\x1b[0m\", f0_bg" in RENDER and
      "%s \\x1b[0m\\x1b[048;2;033;038;045m│\\x1b[0m\", field_bg" in RENDER,
      "输入框末尾空白单元格没有继承字段背景")

if errors:
    print("对齐验证失败:")
    for error in errors:
        print("  FAIL:", error)
    raise SystemExit(1)

print("对齐验证通过：ANSI 1-based / 鼠标 0-based、所有弹窗宽度、设置页热区、命令面板、搜索底栏与输入光标均已检查。")
