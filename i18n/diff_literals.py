#!/usr/bin/env python3
"""Compare the unique string literals of two source trees, to find what an upstream update added.

Run it after `git archive`-ing the previous upstream commit and the new one:

    mkdir %TEMP%\\old %TEMP%\\new
    git archive -o %TEMP%\\old.zip <last-merged-upstream-commit> && tar -xf %TEMP%\\old.zip -C %TEMP%\\old
    git archive -o %TEMP%\\new.zip upstream/main && tar -xf %TEMP%\\new.zip -C %TEMP%\\new
    python i18n/diff_literals.py %TEMP%\\old %TEMP%\\new --en

--en keeps only literals that look like English prose (the ones that usually need translating);
without it you get every added literal, protocol keys included. Point it at the working tree as
the second argument to check your own progress after translating.
"""
import re
import sys
from pathlib import Path

SKIP_EXACT = {
    "", " ", ".", "..", "/", "\\", "%", "|", "-", "+", "_", "##", "#",
    "##id", "##x", "##y", "##z",
}
HEX_RE = re.compile(r"^[0-9A-Fa-f]{1,8}$")
PATH_RE = re.compile(r"^(?:[A-Za-z]:[\\/]|/usr/|https?://|www\.)")
FMT_ONLY_RE = re.compile(r"^[%\d\s.,:;{}\[\]()<>+\-*/=#&|!^~`'\"]+$")
FILE_EXT_RE = re.compile(r"^\.[A-Za-z0-9]{1,5}$")
LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def strip_comments(src: str) -> str:
    """Blank out // and /* */ comments, keeping offsets and newlines."""
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
    return True


def collect(root: Path, dirs):
    """literal -> ["file:line", ...] for every source file under the given directories."""
    seen = {}
    for d in dirs:
        base = root / d
        if not base.exists():
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix not in (".cpp", ".h"):
                continue
            src = p.read_text(encoding="utf-8", errors="replace")
            clean = strip_comments(src)
            for lineno, line in enumerate(clean.split("\n"), 1):
                for m in LIT_RE.finditer(line):
                    lit = m.group(1)
                    if not is_candidate(lit):
                        continue
                    seen.setdefault(lit, []).append(f"{p.relative_to(root).as_posix()}:{lineno}")
    return seen


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    old, new = Path(sys.argv[1]), Path(sys.argv[2])
    dirs = ["src"]
    en_only = False
    for a in sys.argv[3:]:
        if a.startswith("--dirs="):
            dirs = a.split("=", 1)[1].split(",")
        elif a == "--en":
            en_only = True
    a, b = collect(old, dirs), collect(new, dirs)
    added = {k: v for k, v in b.items() if k not in a}
    removed = {k: v for k, v in a.items() if k not in b}
    if en_only:
        def prose(lit: str) -> bool:
            words = re.findall(r"[A-Za-z]{2,}", lit)
            return len(words) >= 2 and bool(re.search(r"[a-z]{2,}\s+[a-z]{2,}", lit) or len(words) >= 3)

        added = {k: v for k, v in added.items() if prose(k)}
        removed = {k: v for k, v in removed.items() if prose(k)}
    print(f"old unique={len(a)} new unique={len(b)} added={len(added)} removed={len(removed)}")

    print("\n=== ADDED ===")
    by_file = {}
    for lit, locs in added.items():
        for loc in locs:
            by_file.setdefault(loc.split(":")[0], []).append((loc, lit))
    for f in sorted(by_file):
        print(f"\n-- {f} ({len(by_file[f])})")
        for loc, lit in sorted(by_file[f], key=lambda t: int(t[0].split(":")[1])):
            print(f"  {loc.split(':')[1]:>5}  {lit}")
    if removed:
        print("\n=== REMOVED (no longer in the new tree) ===")
        for lit in sorted(removed):
            print(f"  {lit}")


if __name__ == "__main__":
    main()
