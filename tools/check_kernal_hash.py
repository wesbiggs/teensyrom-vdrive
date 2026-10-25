#!/usr/bin/env python3
"""Check build/kernal-tr.bin against the SHA-256 the README publishes.

usage: check_kernal_hash.py STOCK.bin KERNAL-TR.bin

Only meaningful when STOCK is the 901227-03 dump the README names: with any
other stock ROM the check is skipped. Fails when the README row is stale,
i.e. a patch changed and the published hash was not updated.
"""
import hashlib, pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
readme = (root / "README.md").read_text()
sha = lambda p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
row = lambda label: re.search(r"^\s*\|[^|\n]*" + re.escape(label) + r"[^|\n]*\|\s*`([0-9a-f]{64})`", readme, re.M).group(1)

stock_want, tr_want = row("kernal-901227-03.bin"), row("from this revision")
stock_got, tr_got = sha(sys.argv[1]), sha(sys.argv[2])
if stock_got != stock_want:
    print(f"SKIP kernal-tr.bin hash: stock ROM {stock_got[:16]}... is not the README's 901227-03")
    sys.exit(0)
if tr_got != tr_want:
    print(f"FAIL kernal-tr.bin hash {tr_got} != README {tr_want}\n"
          f"     a patch changed: update the 'from this revision' row in README.md")
    sys.exit(1)
print("PASS kernal-tr.bin matches the README's SHA-256")
