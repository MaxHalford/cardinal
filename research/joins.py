"""Join cardinalities from per-table models with fanout columns (BayesCard).

Keys are inferred, not declared: a join column is a primary key when its
values are unique, and a foreign key references the primary keys the
workload equates it to.

A question's join predicates split into classes: columns made equal by the
joins. Each class has a domain D, the table whose primary key it is. D may
be absent from the question: cast_info ⋈ movie_info on movie_id uses title.

    σT ──── title.id = ci.movie_id = mi.movie_id ──── σci
                             │
                            σmi

    card = |T| · E_T[1(f_T) · F_ci · F_mi] · P_ci(f_ci) · P_mi(f_mi)

The expectation captures skew (popular movies have many cast and many info
rows) and correlation between T's filters and its fanouts. A filter on a
foreign-key table is assumed independent of the key it points to.

For any tree of relations and classes, rooted at a relation:

    weight(R)  = E_R[1(f_R) · Π F_M] · Π weight(M) · Π avg(c, R)   matches per row of R
                 (M: other members of the class R is the domain of;
                  c: the classes R is a foreign key in)
    avg(c, R)  = |D| / |R| · E_D[1(f_D) · F_R · Π F_M] · Π weight(M) · ...
    card       = |root| · weight(root)

A question outside this shape (a cycle, a join that is not on a foreign
key) gets None: the caller falls back.
"""

import collections
import re

# One side of an equi-join, possibly cast: r3.id or CAST(r3.id AS DOUBLE).
SIDE = r'(?:CAST\()?r(\d+)\.("(?:[^"]|"")+"|\w+)(?: AS \w+\))?'
EQUI_JOIN = re.compile(rf"^\({SIDE} = {SIDE}\)$")


def equi_join(sql):
    """'(r4.movie_id = r6.id)' -> ((4, 'movie_id'), (6, 'id')), or None."""
    m = EQUI_JOIN.match(sql or "")
    if not m:
        return None
    a, ca, b, cb = m.groups()
    return (int(a), ca.strip('"')), (int(b), cb.strip('"'))


class Schema:
    """Primary keys, foreign keys, and table sizes."""

    def __init__(self, pk, fk, rows):
        self.pk = pk      # {(table, column)}
        self.fk = fk      # {(table, column): (table, column)}
        self.rows = rows  # {table: rows}

    def fanouts(self):
        """{table: [(pk, fk table, fk column)]}: the fanout columns to fit."""
        out = collections.defaultdict(list)
        for (table, col), (pk_table, pk) in sorted(self.fk.items()):
            out[pk_table].append((pk, table, col))
        return out


def infer_schema(con, graphs):
    """Keys from the data and the join predicates of every question graph.

    graphs: [(relations, edges)], one per optimizer invocation.
    """
    pairs = set()
    for rels, edges in graphs:
        table = {r["id"]: r["table"] for r in rels if r["kind"] == "table"}
        for e in edges:
            join = equi_join(e["sql"]) if len(e["set"]) == 2 else None
            if join is None:
                continue
            (a, ca), (b, cb) = join
            if a not in table or b not in table:
                continue
            pairs.add(((table[a], ca), (table[b], cb)))

    columns = {c for pair in pairs for c in pair}
    distinct = {c: _distinct(con, *c) for c in columns}
    unique = {c for c in columns if distinct[c] is not None}

    # A foreign key references the primary keys it is equated to: exactly one.
    # Both unique: one-to-one, the side with fewer values references the other,
    # e.g. tags.ExcerptPostId -> posts.Id.
    refs = collections.defaultdict(set)
    for x, y in pairs:
        if x in unique and y in unique and distinct[x] != distinct[y]:
            small, large = (x, y) if distinct[x] < distinct[y] else (y, x)
            refs[small].add(large)
    pk = unique - set(refs)

    for x, y in pairs:
        if (x in pk) != (y in pk):
            key, other = (x, y) if x in pk else (y, x)
            refs[other].add(key)
    fk = {c: next(iter(r)) for c, r in refs.items() if len(r) == 1}

    tables = {t for t, _ in columns}
    rows = {t: con.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0] for t in tables}
    return Schema(pk, fk, rows)


def _distinct(con, table, col):
    """Distinct values of a unique column, or None if it has duplicates."""
    n, distinct = con.execute(f'SELECT count("{col}"), count(DISTINCT "{col}") FROM "{table}"').fetchone()
    return distinct if n > 0 and n == distinct else None


class _Class:
    """Columns made equal by joins, and the primary key they share."""

    def __init__(self, domain, d, fks):
        self.domain = domain  # domain table
        self.d = d            # relation holding the primary key, or None
        self.fks = fks        # {relation: foreign-key column}


class JoinEstimator:
    """card(S) for questions whose relations and classes form a tree."""

    def __init__(self, schema, tables):
        self._schema = schema
        self._tables = tables  # {table: tables.Table}
        # Why questions were not supported: {reason: count}.
        self.unsupported = collections.Counter()

    def estimate(self, rels, local, joins):
        """rels {id: relation}, local {id: filters}, joins [SQL] -> card, or None."""
        classes = self._classes(rels, joins)
        if classes is None:
            return None
        if not _is_tree(list(local), classes):
            self.unsupported["cycle"] += 1
            return None

        self._rels, self._local = rels, local
        self._dom = {c.d: c for c in classes if c.d is not None}
        self._fk = collections.defaultdict(list)
        for c in classes:
            for r in c.fks:
                self._fk[r].append(c)

        root = min(local)
        try:
            return rels[root]["base_rows"] * self._weight(root, None)
        except KeyError:
            # A domain table without the fanout this class needs.
            self.unsupported["missing fanout"] += 1
            return None

    def _classes(self, rels, joins):
        """Union-find over join columns -> [_Class], or None if unsupported."""
        parent = {}

        def find(x):
            while parent.setdefault(x, x) != x:
                x = parent[x]
            return x

        for sql in joins:
            join = equi_join(sql)
            if join is None:
                self.unsupported["not an equi-join"] += 1
                return None
            a, b = join
            parent[find(a)] = find(b)

        groups = collections.defaultdict(list)
        for x in parent:
            groups[find(x)].append(x)

        classes = []
        for members in groups.values():
            c = self._class(rels, members)
            if c is None:
                return None
            classes.append(c)
        return classes

    def _class(self, rels, members):
        """[(relation, column)] -> _Class, or None."""
        relations = [r for r, _ in members]
        if len(set(relations)) != len(relations):
            # Two columns of one relation in a class: a selection, not a join.
            self.unsupported["two columns of a relation"] += 1
            return None

        keyed = [(r, (rels[r]["table"], col)) for r, col in members]
        pks = [(r, key) for r, key in keyed if key in self._schema.pk]
        if len(pks) > 1:
            self.unsupported["primary key = primary key"] += 1
            return None
        if pks:
            d, domain = pks[0]
        else:
            refs = {self._schema.fk.get(key) for _, key in keyed}
            if len(refs) != 1 or None in refs:
                self.unsupported["unknown domain"] += 1
                return None
            d, domain = None, refs.pop()

        fks = {}
        for r, key in keyed:
            if r == d:
                continue
            if self._schema.fk.get(key) != domain:
                self.unsupported["not a foreign key of the domain"] += 1
                return None
            fks[r] = key[1]
        return _Class(domain[0], d, fks)

    def _table(self, r):
        return self._rels[r]["table"]

    def _expect(self, table, filters, powers):
        """E_table[1(filters) · Π F^k]. Tables without a model have no filters."""
        if table in self._tables:
            return self._tables[table].expect(filters, powers)
        if filters or powers:
            raise KeyError(table)
        return 1.0

    def _members(self, c, skip):
        """Fanout powers and member weights of class c, except relation skip."""
        powers, scalar = collections.Counter(), 1.0
        for m, col in c.fks.items():
            if m == skip:
                continue
            powers[(self._table(m), col)] += 1
            scalar *= self._weight(m, c)
        return powers, scalar

    def _weight(self, r, parent):
        """Expected matches per row of r, for its subtree away from class parent."""
        powers, scalar = collections.Counter(), 1.0
        dom = self._dom.get(r)
        if dom is not None and dom is not parent:
            powers, scalar = self._members(dom, None)

        for c in self._fk[r]:
            if c is not parent:
                scalar *= self._avg(c, r)
        return self._expect(self._table(r), self._local[r], powers) * scalar

    def _avg(self, c, r):
        """Expected matches through class c per row of r, a foreign key in c."""
        powers, scalar = self._members(c, r)
        powers[(self._table(r), c.fks[r])] += 1

        filters = ()
        if c.d is not None:
            filters = self._local[c.d]
            for c2 in self._fk[c.d]:
                scalar *= self._avg(c2, c.d)

        rows = self._schema.rows
        e = self._expect(c.domain, filters, powers)
        return rows[c.domain] / rows[self._table(r)] * e * scalar


def _is_tree(relations, classes):
    """Relations and classes, linked by membership, form a tree."""
    links = sum(len(c.fks) + (c.d is not None) for c in classes)
    if links != len(relations) + len(classes) - 1:
        return False

    # Connected: a tree has n - 1 links only when connected.
    parent = {r: r for r in relations}

    def find(x):
        while parent[x] != x:
            x = parent[x]
        return x

    for c in classes:
        members = list(c.fks) + ([c.d] if c.d is not None else [])
        for m in members[1:]:
            parent[find(m)] = find(members[0])
    return len({find(r) for r in relations}) == 1
