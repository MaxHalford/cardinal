"""Write fitted table models into a database, for the cardinal extension.

    uv run export.py <db> <out> [limit]

Fits the models of bn.py, then writes them to the cardinal schema of <db>:

    cardinal.tables          table, rows at fit time
    cardinal.columns         table, column, parent column (a Chow-Liu tree),
                             members grouped into OTHER (compacted columns)
    cardinal.states          table, column, state, value as text, kind
    cardinal.probabilities   P(state | parent state), observed pairs only

Values are text in the column's own SQL syntax: the extension casts them
back to the column type. Fanout columns, F(table.column), are written too:
they connect the tree.
"""

import pathlib
import sys

import duckdb
import pandas as pd
import sorobn

import bn
import joins
import tables
from ceq import read_jsonl

SCHEMA = """
DROP SCHEMA IF EXISTS cardinal CASCADE;
CREATE SCHEMA cardinal;
CREATE TABLE cardinal.tables (table_name VARCHAR, row_count BIGINT);
CREATE TABLE cardinal.columns (table_name VARCHAR, column_name VARCHAR, parent VARCHAR, members BIGINT);
CREATE TABLE cardinal.states (table_name VARCHAR, column_name VARCHAR, state INTEGER, value VARCHAR, kind VARCHAR);
CREATE TABLE cardinal.probabilities (
    table_name VARCHAR, column_name VARCHAR, state INTEGER, parent_state INTEGER, probability DOUBLE
);
"""


def text(value, typ):
    """Model state -> SQL text. Temporal states are epoch seconds."""
    if typ == "DATE":
        return pd.Timestamp(value, unit="s").strftime("%Y-%m-%d")
    if typ.startswith("TIMESTAMP"):
        return pd.Timestamp(value, unit="s").isoformat(sep=" ")
    if isinstance(value, float) and value.is_integer() and typ in ("TINYINT", "SMALLINT", "INTEGER", "BIGINT"):
        return str(int(value))
    return str(value)


def kind(value):
    if value is sorobn.OTHER:
        return "other"
    if sorobn.predicates.is_null(value):
        return "null"
    return "value"


def rows(con, name, model):
    """Rows of the four tables for one table model."""
    net = model._net
    types = dict(con.execute("SELECT column_name, data_type FROM information_schema.columns WHERE table_name = ?", [name]).fetchall())

    columns, states, probs = [], [], []
    index = {}
    for node in net.nodes:
        parent = net.parents.get(node, [None])[0]
        compactor = net.compactors.get(node)
        columns.append((name, node, parent, compactor.n_members_ if compactor else None))

        P = net.P[node]
        values = P.index.get_level_values(node).unique()
        index[node] = {v: i for i, v in enumerate(values)}
        typ = types.get(node, "VARCHAR")
        for v, i in index[node].items():
            k = kind(v)
            states.append((name, node, i, text(v, typ) if k == "value" else None, k))

        for key, p in P.items():
            if parent is None:
                probs.append((name, node, index[node][key], None, p))
            else:
                probs.append((name, node, index[node][key[1]], index[parent][key[0]], p))
    return columns, states, probs


def main(db, out, limit=f"max_categories={tables.MAX_STATES}"):
    out = pathlib.Path(out)
    graphs = bn.load_graphs(out)
    locals_ = bn.local_filters(graphs)
    con = duckdb.connect(db)
    schema = joins.infer_schema(con, graphs.values())
    models = bn.fit_tables(con, schema, locals_, limit)

    con.execute(SCHEMA)
    for name, model in models.items():
        columns, states, probs = rows(con, name, model)
        con.execute("INSERT INTO cardinal.tables VALUES (?, ?)", [name, model.rows])
        con.executemany("INSERT INTO cardinal.columns VALUES (?, ?, ?, ?)", columns)
        con.executemany("INSERT INTO cardinal.states VALUES (?, ?, ?, ?, ?)", states)
        df = pd.DataFrame(probs, columns=["table_name", "column_name", "state", "parent_state", "probability"])
        con.execute("INSERT INTO cardinal.probabilities SELECT * FROM df")
    print(f"{len(models)} table models written to {db}", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
