#!/usr/bin/env python3
"""Download only a named, hash-locked build input."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("asset")
parser.add_argument("destination", type=Path)
args = parser.parse_args()
lock = json.loads((Path(__file__).parent / "dependencies.json").read_text())
asset = lock["assets"][args.asset]
args.destination.parent.mkdir(parents=True, exist_ok=True)
if not args.destination.exists():
    partial = args.destination.with_suffix(args.destination.suffix + ".partial")
    for attempt in range(3):
        try:
            with urllib.request.urlopen(asset["url"], timeout=30) as response, partial.open("wb") as stream:
                shutil.copyfileobj(response, stream)
            break
        except (OSError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(attempt + 1)
    if hashlib.sha256(partial.read_bytes()).hexdigest() != asset["sha256"]:
        raise SystemExit("Download digest mismatch; input rejected")
    partial.replace(args.destination)
if hashlib.sha256(args.destination.read_bytes()).hexdigest() != asset["sha256"]:
    raise SystemExit("Cached input digest mismatch; input rejected")
print(args.asset + ": SHA-256 verified")
