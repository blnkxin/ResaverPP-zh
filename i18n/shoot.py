#!/usr/bin/env python3
"""Take UI screenshots of ReSaverPP: run the exe with a test view, capture a frame, convert to PNG.

    python i18n/shoot.py                 # all views that work without a save
    python i18n/shoot.py settings log    # named views
"""
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "vs" / "Release" / "ReSaverPP.exe"
OUT = ROOT / "i18n" / "shots"

# name -> extra environment (RESAVERPP_TEST_OPEN / RESAVERPP_TEST_CAT ...)
VIEWS = {
    "welcome": {},
    "settings": {"RESAVERPP_TEST_OPEN": "settings"},
    "log": {"RESAVERPP_TEST_OPEN": "log"},
    "support": {"RESAVERPP_TEST_OPEN": "support"},
    "batch": {"RESAVERPP_TEST_OPEN": "batch"},
    "lookup": {"RESAVERPP_TEST_OPEN": "lookup:Nazeem"},
    "compilesettings": {"RESAVERPP_TEST_OPEN": "compilesettings"},
    "arrays": {"RESAVERPP_TEST_OPEN": "arrays"},
    "report": {"RESAVERPP_TEST_OPEN": "report"},
    "missing": {"RESAVERPP_TEST_OPEN": "missing"},
    "orphans": {"RESAVERPP_TEST_OPEN": "orphans"},
    "rowview": {"RESAVERPP_TEST_OPEN": "rowview"},
}


def shoot(name: str, save: str | None = None, extra: dict | None = None) -> None:
    ppm = OUT / f"{name}.ppm"
    png = OUT / f"{name}.png"
    if ppm.exists():
        ppm.unlink()
    env = dict(os.environ)
    env.update(VIEWS.get(name, {}))
    if "RESAVERPP_TEST_OPEN" in env:
        # main.cpp only honours TEST_OPEN inside the TEST_CAT branch
        env.setdefault("RESAVERPP_TEST_CAT", "0")
    if extra:
        env.update(extra)
    env["RESAVERPP_SCREENSHOT"] = str(ppm)
    env["RESAVERPP_SHOT_DELAY"] = "1.0"
    args = [str(EXE)] + ([save] if save else [])
    p = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=40)
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
    print(f"{name}: {png.relative_to(ROOT)} {Image.open(png).size}")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    save = os.environ.get("SHOOT_SAVE")
    names = sys.argv[1:] or list(VIEWS)
    for n in names:
        shoot(n, save)
