#!/usr/bin/env python3
"""Extract string literals (with file/line) from C++ sources for translation."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(r"C:\Users\14034\Documents\deepseek-harness\default-workspace\ResaverPP")
OUT = ROOT / "i18n" / "strings_src.json"

# literals we never want to translate
SKIP_EXACT = {
    "", " ", ".", "..", "/", "\\", "%", "|", "-", "+", "_", "##", "#",
    "##id", "##x", "##y", "##z",
}

HEX_RE = re.compile(r"^[0-9A-Fa-f]{1,8}$")
PATH_RE = re.compile(r"^(?:[A-Za-z]:[\\/]|/usr/|https?://|www\.)")
FMT_ONLY_RE = re.compile(r"^[%\d\s.,:;{}\[\]()<>+\-*/=#&|!^~`'\"]+$")
PY_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_.]*$")
FILE_EXT_RE = re.compile(r"^\.[A-Za-z0-9]{1,5}$")


def strip_comments(src: str) -> str:
    """Blank out // and /* */ comments, keeping offsets and newlines."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "'":
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == "'":
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


LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def is_candidate(lit: str) -> bool:
    if lit in SKIP_EXACT:
        return False
    if not re.search(r"[A-Za-z]{2,}", lit):
        return False
    if HEX_RE.match(lit) or PATH_RE.match(lit):
        return False
    if FMT_ONLY_RE.match(lit):
        return False
    if FILE_EXT_RE.match(lit):
        return False
    if lit.startswith("#"):
        return False
    # file names / extensions / identifiers used as keys, e.g. "resaverpp.ini", "Skyrim.esm"
    return True


def main() -> None:
    files = sorted(str(p) for p in (ROOT / "src").rglob("*") if p.suffix in (".cpp", ".h"))
    entries = []
    for f in files:
        src = Path(f).read_text(encoding="utf-8", errors="replace")
        clean = strip_comments(src)
        lines = clean.split("\n")
        for lineno, line in enumerate(lines, 1):
            for m in LIT_RE.finditer(line):
                lit = m.group(1)
                if not is_candidate(lit):
                    continue
                entries.append({
                    "file": str(Path(f).relative_to(ROOT)).replace("\\", "/"),
                    "line": lineno,
                    "text": lit,
                })
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(entries, ensure_ascii=False, indent=1), encoding="utf-8")
    # stats
    per_file = {}
    uniq = set()
    for e in entries:
        per_file[e["file"]] = per_file.get(e["file"], 0) + 1
        uniq.add(e["text"])
    print(f"occurrences={len(entries)} unique={len(uniq)}")
    for f, c in sorted(per_file.items(), key=lambda kv: -kv[1]):
        print(f"{c:6d}  {f}")


if __name__ == "__main__":
    main()
