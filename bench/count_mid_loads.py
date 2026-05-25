#!/usr/bin/env python3
import re
import sys

text = sys.stdin.read()
pat = re.compile(r"%mid_(?:bid|ask)\d*\s*=\s*load\s+double")
matches = list(pat.finditer(text))
print(f"count={len(matches)}")
for m in matches:
    start = max(0, m.start() - 2)
    end = min(len(text), m.end() + 40)
    print(text[start:end].replace("\n", "\\n"))
