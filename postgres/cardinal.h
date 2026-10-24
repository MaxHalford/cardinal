/*
 * cardinal for PostgreSQL: join sizes from outside the planner.
 */
#pragma once

#include "postgres.h"
#include "nodes/pathnodes.h"

/* Sets a join's rows, before the planner costs its paths. */
extern void cardinal_join_rows(PlannerInfo *root, RelOptInfo *joinrel);

/* From joinrels.c, copied from PostgreSQL: they call cardinal_join_rows. */
extern void cardinal_join_search_one_level(PlannerInfo *root, int level);
extern RelOptInfo *cardinal_make_join_rel(PlannerInfo *root, RelOptInfo *rel1, RelOptInfo *rel2);
