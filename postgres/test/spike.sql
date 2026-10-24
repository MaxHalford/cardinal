-- The planner follows join sizes set from outside.
DROP TABLE IF EXISTS a, b, c;
CREATE TABLE a AS SELECT i AS id FROM generate_series(1, 10000) i;
CREATE TABLE b AS SELECT i AS id, i % 10000 + 1 AS a_id FROM generate_series(1, 100000) i;
CREATE TABLE c AS SELECT i AS id, i % 100000 + 1 AS b_id FROM generate_series(1, 100000) i;
ANALYZE a, b, c;

LOAD :'library';
\echo === PostgreSQL alone
EXPLAIN (COSTS ON) SELECT count(*) FROM a, b, c WHERE a.id = b.a_id AND b.id = c.b_id;
\echo === a,b forced to 1e9 rows: the planner should join b and c first
SET cardinal.rows = 'a,b=1000000000';
EXPLAIN (COSTS ON) SELECT count(*) FROM a, b, c WHERE a.id = b.a_id AND b.id = c.b_id;
\echo === b,c forced to 1e9 rows too: a tie, and b,c shows 1e9 rows
SET cardinal.rows = 'a,b=1000000000; b,c=1000000000';
EXPLAIN (COSTS ON) SELECT count(*) FROM a, b, c WHERE a.id = b.a_id AND b.id = c.b_id;
