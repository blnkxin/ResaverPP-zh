#!/usr/bin/env python3
"""Screenshots of the save views, using a real save file."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "vs" / "Release" / "ReSaverPP.exe"
if not EXE.exists():
    EXE = ROOT / "release" / "ReSaverPP-中文版" / "ReSaverPP.exe"
OUT = ROOT / "i18n" / "shots"
if not os.environ.get("SHOOT_SAVE"):
    raise SystemExit("请用 SHOOT_SAVE=<某个 .ess 存档的路径> 指定要截图的存档")
SAVE = Path(os.environ["SHOOT_SAVE"])
DELAY = float(os.environ.get("SHOOT_DELAY", "3.0"))

CATS = {
    "overview": 0,
    "mods": 1,
    "plugins": 2,
    "changefs": 3,
    "strings": 4,
    "scripts": 5,
    "instances": 6,
    "references": 7,
    "arrays": 8,
    "threads": 9,
    "messages": 10,
    "suspended": 11,
    "unbinds": 12,
    "quests": 13,
}


def shoot(name: str, cat: int, delay: float = DELAY, extra: dict | None = None) -> None:
    ppm = OUT / f"save_{name}.ppm"
    png = OUT / f"save_{name}.png"
    if ppm.exists():
        ppm.unlink()
    env = dict(os.environ)
    env["RESAVERPP_TEST_CAT"] = str(cat)
    env["RESAVERPP_SCREENSHOT"] = str(ppm)
    env["RESAVERPP_SHOT_DELAY"] = str(delay)
    if extra:
        env.update(extra)
    p = subprocess.Popen([str(EXE), str(SAVE)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=90)
    except subprocess.TimeoutExpired:
        p.kill()
        print(f"{name}: timed out")
        return
    if not ppm.exists():
        print(f"{name}: no screenshot (exit {p.returncode})")
        return
    from PIL import Image

    Image.open(ppm).save(png)
    ppm.unlink()
    print(f"{name}: {png.relative_to(ROOT)}")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    names = sys.argv[1:] or ["overview", "mods", "scripts", "instances", "threads", "analysis"]
    for n in names:
        if n == "analysis":
            shoot(n, 0, 5.0, {"RESAVERPP_TEST_ANALYSIS": "1"})
        elif ":" in n:
            # "quests:0003372B" - the quest page with that quest selected (test hook of App::testOpen)
            view, fid = n.split(":", 1)
            shoot(f"quest_{fid}", CATS.get(view, 13), DELAY, {"RESAVERPP_TEST_OPEN": f"quest:{fid}"})
        else:
            shoot(n, CATS.get(n, 0), DELAY)
