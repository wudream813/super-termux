#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""「进程马上就没了」那几条路（Windows 的 CTRL_CLOSE_EVENT 处理器、POSIX 的信号）上，
不许出现【无条件拿锁】。

为什么要有这条：session_flush_now() 是 ctrl_handler 里直接调的，系统在处理器返回之后就把
进程终止了 —— 里面任何一次死等都等于「一次盘都没落」。v2.3.4 恰恰在函数最上面加了一句
session_pump_pending_all()，而它内部走的是无条件 EnterCriticalSection(&g_mux.cs)：ConPTY
读线程在那一刻多半正持着这把锁，处理器就此挂住 ⇒ 用户回报「没有记录终端」。函数自己的
注释就写着「死等等于把退出路径挂在锁上」，改的人没看见。

判法：抽出 session_flush_now 的函数体，要求
  ① TryEnterCriticalSection 之前不出现 EnterCriticalSection(；
  ② session_pump_pending_all() 只能出现在 if (locked) { ... } 里面。
另外正向钉住两件事，防止「为了不过锁而把落地整个删掉」：
  ③ session_flush_now 里确实还调着 session_pump_pending_all()；
  ④ 主循环每轮问一次 session_pump_due()（宽限期兜底，不依赖读线程那一处）。

用法：python3 tools/check_exit_path_locks.py [session.c 路径] [main.c 路径]
拿上一版的文件反向对照：python3 tools/check_exit_path_locks.py <(git show v2.3.4:src/session.c) ...
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def body_of(text, sig):
    """从 sig 那行起，按大括号配平截出函数体（含最外层括号）。"""
    i = text.index(sig)
    j = text.index('{', i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == '{':
            depth += 1
        elif text[k] == '}':
            depth -= 1
            if depth == 0:
                return text[j:k + 1]
    raise SystemExit("函数体没配平：" + sig)


def main():
    sp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "src/session.c")
    mp = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "src/main.c")
    sess = io.open(sp, encoding='utf-8', errors='replace').read()
    mainc = io.open(mp, encoding='utf-8', errors='replace').read()
    bad = []
    if 'int session_flush_now(' not in sess:
        print("[FAIL] 找不到 session_flush_now —— 判据不猜，直接算不过")
        return 1
    body = body_of(sess, 'int session_flush_now(')
    ti = body.find('TryEnterCriticalSection(')
    if ti < 0:
        bad.append("退出路径里已经没有 TryEnter 限时等待了（改成死等 = 把退出挂在锁上）")
    ei = body.find('EnterCriticalSection(')
    if ei >= 0 and (ti < 0 or ei < ti):
        bad.append("TryEnter 之前出现了无条件 EnterCriticalSection（第 1 类：处理器里死等）")
    if 'session_pump_pending_all(' not in body:
        bad.append("session_flush_now 不再落地挂起的快照（第 3 类：绕不过锁就把功能删了）")
    else:
        # 每次 pump 调用都必须落在 if (locked) 的大括号里
        li = body.find('if (locked)')
        if li < 0:
            bad.append("session_flush_now 里没有 if (locked) 这段（pump 没被锁保护）")
        else:
            lockblk = body_of(body, 'if (locked)') if 'if (locked) {' in body[li:li + 20] else None
            if lockblk is None:
                bad.append("if (locked) 的形状变了，判据看不懂了（改代码的人请同步这个检查）")
            else:
                for m in re.finditer(r'session_pump_pending_all\(\)', body):
                    if not (lockblk and m.start() >= body.index(lockblk) and m.start() < body.index(lockblk) + len(lockblk)):
                        bad.append("session_pump_pending_all() 出现在 if (locked) 之外（第 2 类）")
                        break
    if 'session_pump_due()' not in mainc:
        bad.append("主循环里没有 session_pump_due() 这一问（第 4 类：快照只认读线程那一处）")
    for b in bad:
        print("[FAIL] " + b)
    if bad:
        return 1
    print("[ok]   退出路径只限时等锁；挂起块的落地在锁内；主循环有宽限期兜底")
    return 0


if __name__ == '__main__':
    sys.exit(main())
