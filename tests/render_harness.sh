#!/bin/sh
# 构建并运行渲染 harness（Linux，用 tests/stub 的 Win32 替身）。
#
# 不链 src/main.c —— 它自带 main()，而 harness 有自己的 main()。
# main.c 的全局变量与宿主回调由 tests/render_harness_shims.c 提供。
# 不链 src/pane.c / src/conpty.c —— 真 ConPTY 在 Linux 下没有意义，
# 其接口由 shim 空实现。
set -e
cd "$(dirname "$0")/.."
gcc -O1 -Itests/stub -Iinclude \
    src/render.c src/split.c src/framediff.c src/theme.c src/screen.c src/vt.c \
    src/utf8.c src/keymap.c src/input.c src/config.c src/cliphtml.c \
    tests/render_harness_shims.c tests/render_harness.c \
    -o /tmp/render_harness
exec /tmp/render_harness "$@"
