"""STATS-CEB query 58's join plans, with and without cardinal, as Graphviz SVGs.

    uv run plan_figures.py <out dir>

Writes <out dir>/plan-duckdb.svg and plan-fanout.svg, for the blog post. Needs
Graphviz's dot. Numbers measured with DuckDB v2.0.0 on data/stats.duckdb:
estimates from EXPLAIN with SET cardinal_estimator = 'off' and 'on'; true
counts from SQL counts of each join's tables, with the query's filters, so
DuckDB's dynamic filters don't shrink them; times and peak memory from
/usr/bin/time -l, three runs each.
"""
import subprocess
import sys

# An estimate off by more than this factor is red.
BAD = 100

TABLES = {"p": ("posts", 91489), "pl": ("postLinks", 11102), "u": ("users", 36624),
          "ph": ("postHistory", 66973), "v": ("votes", 327982), "b": ("badges", 79400)}


def join(name, est, true):
    off = max(est / true, true / est)
    color = "#d62728" if off > BAD else "#2ca02c"
    label = f"⋈\\nestimated {est:,}\\nactual {true:,}"
    return f'{name} [label="{label}", shape=box, style="rounded,filled", fillcolor=white, color="{color}", penwidth=2];'


def dot(title, joins, edges):
    lines = ["digraph G {", f'label="{title}"; labelloc=t; fontname="Helvetica,sans-Serif"; fontsize=13;',
             'node [fontname="Helvetica,sans-Serif", fontsize=11]; edge [dir=back];']
    for t, (name, rows) in TABLES.items():
        lines.append(f'{t} [label="{name}\\n{rows:,} rows", shape=box, style=filled, fillcolor="#eeeeee", color="#999999"];')
    lines += [join(*j) for j in joins]
    lines += [f"{a} -> {b};" for a, b in edges]
    return "\n".join(lines + ["}"])


if len(sys.argv) != 2:
    sys.exit(__doc__)

plans = {
    "plan-duckdb": dot(
        "DuckDB's estimates: 33 s, 3.5 GB",
        [("j1", 2220, 10959), ("j2", 484, 8909), ("j3", 132, 352054), ("j4", 57, 58582347), ("j5", 101, 17849233970)],
        [("j1", "p"), ("j1", "pl"), ("j2", "j1"), ("j2", "u"), ("j3", "j2"), ("j3", "ph"),
         ("j4", "j3"), ("j4", "b"), ("j5", "j4"), ("j5", "v")],
    ),
    "plan-fanout": dot(
        "cardinal's estimates: 17 s, 63 MB",
        [("j1", 1689, 10959), ("j2", 18123, 8909), ("j3", 1840597, 352054), ("vb", 2585582, 1052150),
         ("top", 3202569446, 17849233970)],
        [("j1", "p"), ("j1", "pl"), ("j2", "j1"), ("j2", "u"), ("j3", "j2"), ("j3", "ph"),
         ("vb", "v"), ("vb", "b"), ("top", "vb"), ("top", "j3")],
    ),
}
for name, src in plans.items():
    subprocess.run(["dot", "-Tsvg", "-o", f"{sys.argv[1]}/{name}.svg"], input=src.encode(), check=True)
    print(name)
