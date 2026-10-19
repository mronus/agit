#ifndef PROVENANCE_STAT_H
#define PROVENANCE_STAT_H

#include "provenance.h"

struct rev_info;
struct repository;

/*
 * Summing the line records of the commits of a revision walk, for
 * "git provenance stat".
 */

/* The lines of line records, by side and mark. */
struct provenance_stat {
	/* the commits walked, and those of them with a line record */
	uintmax_t commits;
	uintmax_t recorded;
	/* the lines of the '+' and '-' ranges, by enum provenance_mark */
	uintmax_t added[PROVENANCE_MARK_UNKNOWN + 1];
	uintmax_t removed[PROVENANCE_MARK_UNKNOWN + 1];
};

/* Add the lines of the ranges of `rec` to `stat`. */
void provenance_stat_add_record(struct provenance_stat *stat,
				const struct provenance_record *rec);

/*
 * Return the share of the added lines of `stat` that are AI or MIXED,
 * as a percentage rounded to the nearest integer, or 0 if there are
 * no added lines.
 */
uintmax_t provenance_stat_share(const struct provenance_stat *stat);

/*
 * Walk the commits of `revs`, which must have been set up but not
 * prepared, and set `stat` to the sum of their line records in `r`.
 * A record that cannot be read or parsed is skipped, with a warning,
 * and its commit does not count as recorded.  Returns 0 on success,
 * and -1, after reporting an error, if refs/provenance/lines cannot
 * be read or the walk cannot be set up.
 */
int provenance_stat_walk(struct repository *r, struct rev_info *revs,
			 struct provenance_stat *stat);

#endif /* PROVENANCE_STAT_H */
