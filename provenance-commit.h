#ifndef PROVENANCE_COMMIT_H
#define PROVENANCE_COMMIT_H

#include "oid-array.h"

struct commit;
struct commit_list;
struct object_id;
struct provenance_reasoning;
struct provenance_record;
struct ref_transaction;
struct repository;
struct strbuf;

/*
 * The commits that a commit about to be made rewrites.  See the
 * "History rewriting" section of
 * Documentation/technical/git-provenance.adoc.
 */
struct provenance_rewrite {
	/*
	 * The commit that the new one replaces ("commit --amend", and
	 * the "fixup" and "squash" commands of "rebase"), or NULL.
	 */
	struct commit *amend;
	/*
	 * The commit whose changes the new one applies again or, with
	 * `revert`, undoes, or NULL.
	 */
	struct commit *pick;
	unsigned revert : 1;
	/*
	 * The commits that "cherry-pick --no-commit" applied before the
	 * commit was made (see provenance_picks_read()), oldest first;
	 * not with `revert`.
	 */
	struct oid_array picks;
	/*
	 * The commit being made, if "git am" made it from a patch mail,
	 * or NULL; not with the others.  Its changes are owned as
	 * `mail_lines` says, the line record that the mail carried,
	 * located in the commit (see provenance_patch_locate()), or, if it
	 * is NULL, are of unknown origin, as those of a commit without a
	 * line record.
	 */
	struct commit *mail;
	const struct provenance_record *mail_lines;
};

/*
 * Compute the records of a commit about to be made in `r`, with the
 * tree `tree`, the parents `parents` (NULL for a root commit) and the
 * author `author` (as in a commit object), from the journal of the
 * worktree, as "git commit" does.  If `rw` is not NULL, the records of
 * the commits it names are carried over.  `session` is as for
 * provenance_attribute(), which also says what is recorded for a merge.
 *
 * The sources of the commit are `amend`, `pick` and the `picks`, in
 * that order, each named once (see provenance_attribute_rewrite()).
 * The `picks` are tentative sources, as they may name commits whose
 * changes were discarded before the commit was made.  A pick that is
 * not a commit is ignored with a warning.  With `mail`, the commit is
 * its own source, with the line record `mail_lines` and no reasoning
 * record.
 *
 * The records are stored in `lines` and `reasoning` as by
 * provenance_attribute().  The lines that the commit takes from a
 * commit without a line record are UNKNOWN; a line record that cannot
 * be parsed is ignored with a warning.  Returns 0 on success, or -1
 * after reporting an error.  The journal is left alone.
 */
int provenance_commit_records(struct repository *r,
			      const struct commit_list *parents,
			      const struct object_id *tree,
			      const char *author,
			      const char *session,
			      const struct provenance_rewrite *rw,
			      struct provenance_record *lines,
			      struct provenance_reasoning *reasoning);

/*
 * Write `lines` and `reasoning` as the records of `commit`, filling in
 * their commit ids, and queue the updates of the provenance refs into
 * `transaction`, with the reflog message `reflog_msg`.  The reasoning
 * record is only written if it names a session.  Returns 0 on success,
 * or -1 with a message in `err`.
 */
int provenance_queue_records(struct repository *r,
			     struct ref_transaction *transaction,
			     const struct object_id *commit,
			     struct provenance_record *lines,
			     struct provenance_reasoning *reasoning,
			     const char *reflog_msg,
			     struct strbuf *err);

#endif /* PROVENANCE_COMMIT_H */
