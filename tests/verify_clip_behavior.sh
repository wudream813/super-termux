#!/bin/sh
# 构建渲染 harness 并跑方案 B 的验收断言。
# 用法：
#   sh tests/verify_clip_behavior.sh                 # 用当前 src/render.c
#   sh tests/verify_clip_behavior.sh path/to/render.c  # 用指定的 render.c（A/B 对照）
set -e
cd "$(dirname "$0")/.."
RENDER="${1:-src/render.c}"
gcc -O1 -Itests/stub -Iinclude \
    "$RENDER" src/split.c src/framediff.c src/theme.c src/screen.c src/vt.c \
    src/utf8.c src/keymap.c src/input.c src/config.c src/cliphtml.c \
    tests/render_harness_shims.c tests/render_harness.c \
    -o /tmp/rh_check
exec python3 tests/verify_clip_behavior.py /tmp/rh_check
