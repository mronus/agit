#ifndef PROVENANCE_REMAP_H
#define PROVENANCE_REMAP_H

#include "hash.h"

struct oid_array;
struct repository;

/*
 * Carrying the records of commits over to the commits that replace them
 * under other ids, as an import of an edited "git fast-export" stream
 * makes, and removing the records of commits that are gone.  See the
 * "Re-anchoring" section of Documentation/technical/git-provenance.adoc.
 */

enum provenance_remap_result {
	/* the old commit has no records: nothing was done */
	PROVENANCE_REMAP_NONE,
	/* the records were moved as they are */
	PROVENANCE_REMAP_MOVED,
	/* the line record was located in the new commit and adopted */
	PROVENANCE_REMAP_REATTACHED,
	/* the new id does not name a commit */
	PROVENANCE_REMAP_NOT_COMMIT,
	/* the new commit has a line record already */
	PROVENANCE_REMAP_HAS_RECORD,
	/* a record of the old commit cannot be parsed */
	PROVENANCE_REMAP_BAD_RECORD,
};

struct provenance_remap {
	struct object_id old_oid;
	struct object_id new_oid;
	/* the outcome, set by provenance_remap() */
	enum provenance_remap_result result;
};

/*
 * Move the records of the old commit of each of the `nr` pairs in
 * `pairs`, in order, to its new commit, rewriting their `commit` lines.
 * A line record that provenance_verify() accepts for the new commit is
 * moved as it is.  Otherwise it is located in the new commit as "git
 * am" locates the record of a patch mail (see provenance_patch_locate()),
 * and the record of the new commit is computed from it with the lines
 * it does not account for UNKNOWN and `capture` set to `adopted`.  The
 * reasoning record is moved as it is.  A pair whose ids are equal is
 * ignored.  The records of a pair whose new id does not name a commit,
 * or whose new commit has a line record already, or that cannot be
 * parsed, are left alone with a warning.
 *
 * Unless `dry_run` is set, the records are written as one notes commit
 * on top of each provenance ref that changes, and the refs are updated
 * in one transaction with the reflog message "remap".  Nothing is
 * written if no record moves.  Returns 0 on success and -1, after
 * reporting an error, if the records cannot be read or written.
 */
int provenance_remap(struct repository *r, struct provenance_remap *pairs,
		     size_t nr, int dry_run);

/*
 * Remove the line and reasoning records of the commits that are not
 * objects of `r`, as "git notes prune" removes notes, and append the
 * ids of those commits to `pruned`, once per record.  Unless `dry_run`
 * is set, the refs are written as by provenance_remap(), with the
 * reflog message "prune".  Returns 0 on success and -1, after
 * reporting an error, if the records cannot be read or written.
 */
int provenance_prune(struct repository *r, int dry_run,
		     struct oid_array *pruned);

#endif /* PROVENANCE_REMAP_H */
