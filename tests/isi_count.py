#!/usr/bin/env python3
"""usage: isi_count.py <recorded.ts> <ref_isi1.ts> [<ref_isi2.ts> ...]
Counts how many recorded packets come from each reference stream."""
import sys

P = 188
b = open(sys.argv[1], "rb").read()
out = [b[i:i + P] for i in range(0, len(b) - P + 1, P)]
counts = []
for ref_path in sys.argv[2:]:
    a = open(ref_path, "rb").read()
    ref = set(a[i:i + P] for i in range(0, len(a) - P + 1, P))
    counts.append(f"{ref_path.rsplit('.', 2)[-2]}: {sum(p in ref for p in out)}")
print(f"{len(out)} packets recorded; " + ", ".join(counts))
