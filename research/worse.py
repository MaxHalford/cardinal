"""Queries whose plan costs more with the extension than without it.

    uv run worse.py <out> ...

Reads <out>/plans/cardinal_off.jsonl and cardinal_fanout.jsonl (ext.py plans).
Cost: true cardinalities over DuckDB's cost model. Exits 1 if any query is worse.
"""

import json
import pathlib
import sys

# Equal costs can differ by float rounding.
TOLERANCE = 1e-9


def costs(path):
    return {r["query"]: r["cost"] for r in map(json.loads, path.read_text().splitlines())}


def worse(out):
    plans = pathlib.Path(out) / "plans"
    off, fanout = costs(plans / "cardinal_off.jsonl"), costs(plans / "cardinal_fanout.jsonl")
    rows = sorted((fanout[q] / off[q], q) for q in off.keys() & fanout.keys() if off[q] and fanout[q] > off[q] * (1 + TOLERANCE))
    print(f"{out}: {len(rows)} of {len(off.keys() & fanout.keys())} queries worse, "
          f"total cost {sum(off.values()):.3g} -> {sum(fanout.values()):.3g}")
    for ratio, query in reversed(rows):
        print(f"  {query}: x{ratio:.3f}")
    return not rows


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(0 if all([worse(out) for out in sys.argv[1:]]) else 1)
