#!/bin/sh
# bug #28 判据：滚动条拖动期间的鼠标捕获（2026-09-20 用户报「滚动时光标移到另一个
# 窗格会直接变成移动另一个窗格的滚动条」）。
#
# 编的是【真实 src/input.c + src/split.c】，用合成鼠标事件驱动 handle_mouse()。
# 末尾带自证：把捕获拆掉重新编一遍，判据必须变红。
set -e
cd "$(dirname "$0")/.."
# ★ Windows（MSYS2/MINGW64，uname -s 形如 MINGW64_NT-...）上 MinGW 的 gcc 会给
#   【无扩展名】的 -o 自动补 .exe，后面再执行没扩展名的路径就找不到。显式带上。
case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac

compile() {   # $1 = 输出 exe, $2 = 用哪个 input.c
    gcc -O1 -Itests/stub -Iinclude \
        src/render.c src/split.c src/framediff.c src/theme.c src/screen.c src/vt.c \
        src/utf8.c src/keymap.c "$2" src/config.c src/cliphtml.c \
        tests/render_harness_shims.c tests/sb_drag_harness.c -o "$1"
}

echo "=== 滚动条拖动鼠标捕获 (tests/verify_sb_drag.sh) ==="
compile /tmp/sbdrag$EXE src/input.c
/tmp/sbdrag$EXE

# ---- 自证（验红）：把 handle_split_mouse 顶上的捕获整块短路掉，判据必须失败。 ----
MUT=/tmp/input_mutant_sbdrag.c
python3 - "$MUT" <<'PY'
import sys
src = open("src/input.c", encoding="utf-8").read()
anchor = ("     * 分隔条拖动本来就有同样的捕获（第 1 步直接 return 1），这边补上。 */\n"
          "    if (g_sb_dragging) {")
assert src.count(anchor) == 1, "自证锚点没找到，改 src/input.c 时要同步这里"
mut = src.replace(anchor, anchor.replace("if (g_sb_dragging) {",
                                         "if (0) {  /* MUTANT: 拆掉鼠标捕获 */"))
open(sys.argv[1], "w", encoding="utf-8").write(mut)
PY

set +e
compile /tmp/sbdrag_mut$EXE "$MUT"
OUT=$(/tmp/sbdrag_mut$EXE 2>&1)
RC=$?
set -e
if [ "$RC" -eq 0 ]; then
    echo "FAIL: 自证失败 —— 拆掉捕获后测试仍然全绿，说明判据没抓住 bug #28"
    exit 1
fi
echo "$OUT" | grep '^\[FAIL\]' | head -6 | sed 's/^/      /'
N=$(echo "$OUT" | grep -c '^\[FAIL\]')
echo "自证通过：拆掉鼠标捕获后被抓住 $N 条。"
