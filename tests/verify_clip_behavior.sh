#!/bin/sh
# 构建渲染 harness 并跑方案 B 的验收断言。
# 用法：
#   sh tests/verify_clip_behavior.sh                 # 用当前 src/render.c
#   sh tests/verify_clip_behavior.sh path/to/render.c  # 用指定的 render.c（A/B 对照）
set -e
cd "$(dirname "$0")/.."
# ★ Windows（MSYS2/MINGW64，uname -s 形如 MINGW64_NT-...）上 MinGW 的 gcc 会给
#   【无扩展名】的 -o 自动补 .exe，后面再执行没扩展名的路径就找不到。显式带上。
case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
RENDER="${1:-src/render.c}"
gcc -O1 -Itests/stub -Iinclude \
    "$RENDER" src/split.c src/framediff.c src/theme.c src/screen.c src/vt.c \
    src/utf8.c src/keymap.c src/input.c src/config.c src/cliphtml.c \
    tests/render_harness_shims.c tests/render_harness.c \
    -o /tmp/rh_check$EXE
exec python3 tests/verify_clip_behavior.py /tmp/rh_check$EXE
