/*
 * cardinal for PostgreSQL, a spike: the planner's join search with join sizes set from
 * outside. PostgreSQL has no hook for join sizes: the extension replaces the join search
 * (join_search_hook) with PostgreSQL's own, copied in joinrels.c, which sets each join's rows
 * before costing its paths, as pg_hint_plan does.
 *
 * For now, sizes come from a setting, by table alias:
 *
 *   SET cardinal.rows = 'a,b=100000000; b,c=10';
 */
#include "cardinal.h"

#include "fmgr.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static char *rows_setting = NULL;

static int CompareNames(const ListCell *a, const ListCell *b) {
	return strcmp((const char *)lfirst(a), (const char *)lfirst(b));
}

/* e.g. "a,b": the aliases of the join's tables, sorted. */
static char *JoinKey(PlannerInfo *root, RelOptInfo *joinrel) {
	List *names = NIL;
	int rti = -1;
	StringInfoData key;
	ListCell *lc;

	while ((rti = bms_next_member(joinrel->relids, rti)) >= 0) {
		RangeTblEntry *rte;
		if (rti >= root->simple_rel_array_size || root->simple_rel_array[rti] == NULL) {
			/* An outer join's relid: not a table. */
			continue;
		}
		rte = planner_rt_fetch(rti, root);
		names = lappend(names, rte->eref->aliasname);
	}
	list_sort(names, CompareNames);

	initStringInfo(&key);
	foreach (lc, names) {
		appendStringInfo(&key, "%s%s", key.len > 0 ? "," : "", (char *)lfirst(lc));
	}
	return key.data;
}

void cardinal_join_rows(PlannerInfo *root, RelOptInfo *joinrel) {
	char *key;
	char *copy;
	char *saveptr = NULL;

	if (rows_setting == NULL || rows_setting[0] == '\0') {
		return;
	}
	key = JoinKey(root, joinrel);

	/* "a,b=100; b,c=5": entries separated by ';'. */
	copy = pstrdup(rows_setting);
	for (char *entry = strtok_r(copy, ";", &saveptr); entry != NULL; entry = strtok_r(NULL, ";", &saveptr)) {
		char *equals = strchr(entry, '=');
		if (equals == NULL) {
			continue;
		}
		*equals = '\0';
		while (*entry == ' ') {
			entry++;
		}
		if (strcmp(entry, key) == 0) {
			joinrel->rows = clamp_row_est(strtod(equals + 1, NULL));
		}
	}
}

/* PostgreSQL's standard_join_search, calling the copied join_search_one_level. */
static RelOptInfo *JoinSearch(PlannerInfo *root, int levels_needed, List *initial_rels) {
	RelOptInfo *rel;

	/* Many joins: PostgreSQL's genetic search, as without the extension. */
	if (enable_geqo && levels_needed >= geqo_threshold) {
		return geqo(root, levels_needed, initial_rels);
	}

	root->join_rel_level = (List **)palloc0((levels_needed + 1) * sizeof(List *));
	root->join_rel_level[1] = initial_rels;

	for (int lev = 2; lev <= levels_needed; lev++) {
		ListCell *lc;
		cardinal_join_search_one_level(root, lev);
		foreach (lc, root->join_rel_level[lev]) {
			RelOptInfo *rel = (RelOptInfo *)lfirst(lc);
			generate_partitionwise_join_paths(root, rel);
			if (!bms_equal(rel->relids, root->all_query_rels)) {
				generate_useful_gather_paths(root, rel, false);
			}
			set_cheapest(rel);
		}
	}

	if (root->join_rel_level[levels_needed] == NIL) {
		elog(ERROR, "failed to build any %d-way joins", levels_needed);
	}
	rel = (RelOptInfo *)linitial(root->join_rel_level[levels_needed]);
	root->join_rel_level = NULL;
	return rel;
}

void _PG_init(void) {
	DefineCustomStringVariable("cardinal.rows", "Join sizes by table aliases, e.g. 'a,b=1000; b,c=10'.", NULL,
	                           &rows_setting, "", PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("cardinal");
	join_search_hook = JoinSearch;
}
