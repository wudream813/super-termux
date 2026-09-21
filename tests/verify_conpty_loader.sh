#!/bin/sh
# tests/verify_conpty_loader.sh —— src/conpty_loader.c 的决策逻辑回归。
#
# 每个 case 一个进程（conpty_loader_init 内部缓存结果），逐个跑并汇总退出码。
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=/tmp/tcl_base
BIN_A=/tmp/tcl_planA
rc=0

echo "=== conpty_loader 决策逻辑回归 ==="

gcc -O1 -Wall -Wextra -Werror -Itests/loaderstub -Iinclude \
    tests/test_conpty_loader.c src/conpty_loader.c -o "$BIN" || exit 1
gcc -O1 -Wall -Wextra -Werror -DTERMUX_CONPTY_DEFAULT_DLL -Itests/loaderstub -Iinclude \
    tests/test_conpty_loader.c src/conpty_loader.c -o "$BIN_A" || exit 1

for c in system auto_dll auto_nodll dll_only_missing dll_partial \
         flags_hex flags_dec flags_bad idempotent; do
    if "$BIN" "$c"; then :; else rc=1; fi
done

echo "--- 编译期默认（不带 -DTERMUX_CONPTY_DEFAULT_DLL）---"
if "$BIN" default; then :; else rc=1; fi
echo "--- 编译期默认（带 -DTERMUX_CONPTY_DEFAULT_DLL，即 Plan A）---"
if "$BIN_A" default; then :; else rc=1; fi

if [ "$rc" -eq 0 ]; then
    echo
    echo "[PASS] conpty_loader 全部 case 通过"
else
    echo
    echo "[FAIL] conpty_loader 有 case 未通过"
fi
exit $rc
