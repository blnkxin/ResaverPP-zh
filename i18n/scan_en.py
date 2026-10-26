#!/usr/bin/env python3
"""List the string literals of the given sources that still read like English prose.

Use it to check a translation pass: anything this prints in src/gui or src/core is either
untranslated UI text, or one of the strings that deliberately stay English (protocol keys,
Papyrus keywords, file names...). Compare with the list for the previous release to see what is new.

    python i18n/scan_en.py src/gui/AppCheck.cpp
    python i18n/scan_en.py (Get-ChildItem src -Recurse -Include *.cpp,*.h).FullName   # pwsh
"""
import re
import sys
from pathlib import Path

LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def strip_comments(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in '"\'':
            q = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def main(paths) -> None:
    for p in paths:
        p = Path(p)
        if not p.is_file():
            continue
        clean = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        hits = []
        for lineno, line in enumerate(clean.split("\n"), 1):
            for m in LIT_RE.finditer(line):
                lit = m.group(1)
                if not lit.strip():
                    continue
                if re.fullmatch(r"[%\w.%/*\\ :\-+#]*", lit) and " " not in lit.strip():
                    continue
                words = re.findall(r"[A-Za-z]{2,}", lit)
                if len(words) < 2 and " " not in lit.strip():
                    continue
                if not re.search(r"[a-z]{2,}\s+[a-z]{2,}", lit) and len(words) < 3:
                    continue
                hits.append((lineno, lit))
        if hits:
            print(f"\n=== {p} ({len(hits)})")
            for lineno, lit in hits:
                print(f"{lineno:6d}  {lit}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    main(sys.argv[1:])
