#!/usr/bin/env python3
"""usage: tscompare.py <reference.ts> <output.ts> -- packets recovered intact."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bench"))
from loopback import compare_ts  # noqa: E402

intact, n_out, n_ref = compare_ts(sys.argv[1], sys.argv[2])
print(f"{intact}/{n_ref} reference packets recovered intact ({n_out} output packets, "
      f"{n_out - intact} bad)")
sys.exit(0 if intact == n_out else 1)
