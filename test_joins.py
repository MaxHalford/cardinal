"""The fanout estimator against exact counts on tiny databases.

Each table model has at most two variables, so Chow-Liu is exact and any
error comes from the join estimator itself.
"""

import duckdb
import pandas as pd
import pytest

import bn
import joins
import tables

LIMIT = "max_categories=1000"

# Movies 0..5 with skewed, correlated cast and info counts.
CAST = [3, 0, 1, 2, 5, 1]
INFO = [1, 4, 0, 2, 1, 3]


def database(**frames):
    con = duckdb.connect()
    for name, frame in frames.items():
        con.register(f"{name}_df", frame)
        con.execute(f"CREATE TABLE {name} AS SELECT * FROM {name}_df")
    return con


def repeat(counts):
    """[2, 0, 1] -> [0, 0, 2]: a foreign key with these fanouts."""
    return [k for k, n in enumerate(counts) for _ in range(n)]


def estimate(con, names, preds, filters=None, question=None):
    """Fit like bn.py on a workload of one query, and estimate a subset of it.

    question: the relation ids asked about, all by default. Join keys are
    inferred from every predicate, as from a whole workload.
    """
    filters = filters or {}
    rels = [
        {"id": i, "kind": "table", "table": t, "base_rows": con.execute(f"SELECT count(*) FROM {t}").fetchone()[0]}
        for i, t in enumerate(names)
    ]
    edges = [{"set": sorted(side[0] for side in joins.equi_join(p)), "sql": p} for p in preds]
    schema = joins.infer_schema(con, [(rels, edges)])
    locals_ = {("q", 0, r["id"]): (r["table"], r["base_rows"], 0, filters.get(r["id"], [])) for r in rels}
    models = bn.fit_tables(con, schema, locals_, LIMIT)

    question = set(range(len(names)) if question is None else question)
    asked = [e["sql"] for e in edges if set(e["set"]) <= question]
    local = {r: filters.get(r, []) for r in question}
    return joins.JoinEstimator(schema, models).estimate({r["id"]: r for r in rels}, local, asked)


def count(con, names, preds, filters=None):
    filters = filters or {}
    where = list(preds) + [f for fs in filters.values() for f in fs]
    sql = f"SELECT count(*) FROM {', '.join(f'{t} AS r{i}' for i, t in enumerate(names))} WHERE {' AND '.join(where)}"
    return con.execute(sql).fetchone()[0]


@pytest.fixture
def movies():
    return database(
        title=pd.DataFrame({"id": range(6), "kind": ["a", "b", "a", "b", "a", "b"]}),
        cast_info=pd.DataFrame({"movie_id": repeat(CAST)}),
        movie_info=pd.DataFrame({"movie_id": repeat(INFO)}),
    )


def test_star_captures_correlated_fanouts(movies):
    names = ["title", "cast_info", "movie_info"]
    preds = ["(r0.id = r1.movie_id)", "(r0.id = r2.movie_id)", "(r1.movie_id = r2.movie_id)"]
    assert estimate(movies, names, preds) == pytest.approx(count(movies, names, preds))
    assert count(movies, names, preds) == sum(c * i for c, i in zip(CAST, INFO))


def test_fk_fk_join_uses_absent_domain(movies):
    # DuckDB asks about cast_info ⋈ movie_info alone, through a transitive edge.
    names = ["cast_info", "movie_info", "title"]
    preds = ["(r0.movie_id = r1.movie_id)", "(r0.movie_id = r2.id)", "(r1.movie_id = r2.id)"]
    est = estimate(movies, names, preds, question=[0, 1])
    assert est == pytest.approx(sum(c * i for c, i in zip(CAST, INFO)))


def test_filter_on_domain_is_correlated_with_fanout(movies):
    names = ["title", "cast_info"]
    preds = ["(r0.id = r1.movie_id)"]
    filters = {0: ["(r0.kind = 'a')"]}
    assert estimate(movies, names, preds, filters) == pytest.approx(count(movies, names, preds, filters))
    assert count(movies, names, preds, filters) == CAST[0] + CAST[2] + CAST[4]


def test_snowflake_weights_filter_by_fanout():
    # cast_info points to title and to name. The name filter selects heavy people.
    people = [0, 0, 0, 0, 1, 2]
    con = database(
        title=pd.DataFrame({"id": range(3)}),
        name=pd.DataFrame({"id": range(3), "gender": ["f", "m", "m"]}),
        cast_info=pd.DataFrame({"movie_id": [0, 1, 2, 0, 1, 2], "person_id": people}),
    )
    names = ["name", "cast_info", "title"]
    preds = ["(r0.id = r1.person_id)", "(r1.movie_id = r2.id)"]
    filters = {0: ["(r0.gender = 'f')"]}
    assert estimate(con, names, preds, filters) == pytest.approx(count(con, names, preds, filters))
    assert count(con, names, preds, filters) == 4


def test_fk_filter_is_assumed_independent_of_key(movies):
    movies.execute("ALTER TABLE cast_info ADD COLUMN role VARCHAR DEFAULT 'actor'")
    movies.execute("UPDATE cast_info SET role = 'director' WHERE movie_id = 4")
    names = ["title", "cast_info"]
    preds = ["(r0.id = r1.movie_id)"]
    filters = {1: ["(r1.role = 'director')"]}
    # True: the 5 rows of movie 4. Estimate: |cast_info| · P(director), whatever the key.
    assert count(movies, names, preds, filters) == 5
    assert estimate(movies, names, preds, filters) == pytest.approx(5)


def test_cycle_is_not_supported():
    con = database(
        users=pd.DataFrame({"id": range(3)}),
        posts=pd.DataFrame({"id": range(4), "owner": [0, 0, 1, 2]}),
        comments=pd.DataFrame({"post": [0, 0, 1, 3], "user": [0, 1, 1, 2]}),
    )
    names = ["users", "posts", "comments"]
    preds = ["(r0.id = r1.owner)", "(r1.id = r2.post)", "(r0.id = r2.user)"]
    assert estimate(con, names, preds) is None


def test_two_columns_of_one_relation_in_a_class_is_not_supported(movies):
    movies.execute("CREATE TABLE movie_link AS SELECT movie_id, movie_id AS linked_id FROM cast_info")
    names = ["title", "movie_link"]
    preds = ["(r0.id = r1.movie_id)", "(r0.id = r1.linked_id)"]
    assert estimate(movies, names, preds) is None


def test_bin_fanout_keeps_zero_exact_and_means_per_bin():
    codes, means = tables.bin_fanout([0, 1, 1, 2, 0])
    assert list(codes) == [0, 1, 1, 2, 0]
    assert list(means) == [0, 1, 2]

    values = list(range(100)) + [10_000]
    codes, means = tables.bin_fanout(values)
    assert len(means) <= tables.FANOUT_BINS + 2
    # Bin means preserve the total: E[F] is exact.
    assert sum(means[c] for c in codes) == pytest.approx(sum(values))
    # The heavy value gets its own bin.
    assert means[codes[-1]] == 10_000


def test_one_to_one_foreign_key(movies):
    # Each tag points to a distinct movie: a unique foreign key.
    movies.execute("CREATE TABLE tags AS SELECT * FROM (VALUES (0, 'x'), (4, 'y')) t(movie_id, name)")
    names = ["title", "tags", "cast_info"]
    preds = ["(r0.id = r1.movie_id)", "(r0.id = r2.movie_id)", "(r1.movie_id = r2.movie_id)"]
    assert estimate(movies, names, preds) == pytest.approx(count(movies, names, preds))
    assert count(movies, names, preds) == CAST[0] + CAST[4]
