#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 README.md 的「版本历史」里抽出某个版本的说明，当作 GitHub Release 的正文。

为什么要有这么个脚本：release.yml 原来不写 body，发出来的 Release 正文是空的。
直接往 workflow 里塞一段写死的文案又会和 README 脱钩 —— 改了一处忘另一处。
所以正文从 README 里抽，两边永远一致；抽不到就【报错退出】，
而不是悄悄发一个空正文的 Release（那样看不出出了问题）。

用法：
    python3 tools/release_notes.py 2.0.0 [> notes.md]

输出以 0 结尾表示成功。抽不到指定版本 => 退出码 1。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def extract(readme_text, version):
    """返回该版本在「版本历史」里的说明段（不含标题行本身），抽不到返回 None。"""
    m = re.search(r"^## 版本历史\s*$(.*?)(?=^## |\Z)", readme_text, re.M | re.S)
    if not m:
        return None
    section = m.group(1)

    # ★ 必须锚定在「**vX.Y.Z** ——」这种段落开头，不能全文搜 "**vX.Y.Z**"：
    #   文件开头「当前版本：**v2.0.0**」也写着同一个号，全文搜会把「当前版本」
    #   那行当成版本说明抽出来（verify_port.py 第一版判据就栽在同一个坑里）。
    pat = re.compile(r"^\*\*v%s\*\*\s*(?:——\s*)?" % re.escape(version), re.M)
    hit = pat.search(section)
    if not hit:
        return None

    rest = section[hit.end():]
    # 段落结束于下一个空行（段落之间用空行隔开），或者「更早的版本详见…」那行。
    stop = re.search(r"\n\s*\n|更早的版本详见", rest)
    body = rest[:stop.start()] if stop else rest
    body = body.strip()
    return body or None


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("用法: release_notes.py <版本号，不带 v>\n")
        return 2
    version = argv[1].lstrip("vV")

    path = os.path.join(ROOT, "README.md")
    if not os.path.exists(path):
        sys.stderr.write("找不到 %s\n" % path)
        return 1
    with io.open(path, "r", encoding="utf-8") as f:
        text = f.read()

    body = extract(text, version)
    if body is None:
        sys.stderr.write(
            "README.md 的「版本历史」里找不到 **v%s** 这一段。\n"
            "请先在 README 里补上这个版本的说明，再打 tag ——\n"
            "发布说明和 README 必须一致，不允许发空正文的 Release。\n" % version)
        return 1

    out = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n")
    out.write(u"## v%s\n\n%s\n" % (version, body))
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
