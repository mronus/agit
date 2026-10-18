#ifndef PROVENANCE_ADOPT_H
#define PROVENANCE_ADOPT_H

#include "provenance.h"

struct commit;
struct repository;

/*
 * Giving records to commits that were made without the extension, from
 * the records of the commits they were made from.  See the
 * "Re-anchoring" section of Documentation/technical/git-provenance.adoc.
 */

struct provenance_adopt {
	struct repository *r;
	/* the line records of the candidates */
	struct provenance_cache cache;
	/* the commits that may be sources, oldest first */
	struct commit **candidates;
	size_t candidates_nr, candidates_alloc;
};

/*
 * Prepare to adopt the `targets_nr` commits in `targets`.  The
 * candidate sources are the commits with a line record that none of
 * the targets can reach and, if `from_nr` is not 0, that one of the
 * `from_nr` commits in `from` can reach.  A record that is not
 * attached to a commit of the repository, or that cannot be parsed, is
 * ignored with a warning.  This walks history, and clears the marks
 * that revision walks leave on commits.
 */
void provenance_adopt_init(struct provenance_adopt *pa, struct repository *r,
			   struct commit **targets, size_t targets_nr,
			   struct commit **from, size_t from_nr);

enum provenance_adopt_result {
	/* the commit has been given records */
	PROVENANCE_ADOPT_ADOPTED,
	/* no candidate accounts for a line of the commit */
	PROVENANCE_ADOPT_NO_SOURCE,
	/* the commit has a line record already */
	PROVENANCE_ADOPT_HAS_RECORD,
	/* the commit is a merge */
	PROVENANCE_ADOPT_MERGE,
};

/*
 * Give `commit` the records that the candidates which touch the paths
 * it changes carry over to it, found by content as a rewrite finds its
 * sources, and write them in their own ref transaction.  The lines no
 * candidate accounts for are UNKNOWN, and nothing is written unless a
 * candidate accounts for one.  A commit that has a line record is left
 * alone, unless `force` is set and the record was adopted.  The
 * outcome is stored in `result`.  Returns 0 on success, or -1 after
 * reporting an error.
 */
int provenance_adopt_commit(struct provenance_adopt *pa, struct commit *commit,
			    int force, enum provenance_adopt_result *result);

void provenance_adopt_release(struct provenance_adopt *pa);

#endif /* PROVENANCE_ADOPT_H */
