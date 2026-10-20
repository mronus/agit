#ifndef PROVENANCE_JOURNAL_H
#define PROVENANCE_JOURNAL_H

#include "hash.h"

struct pathspec;
struct repository;
struct strbuf;

/*
 * The capture journal is a per-worktree, append-only list of the
 * stamps made by AI agents ("git stamp") since the last commit.  It
 * lives at "$GIT_DIR/provenance/journal" and is never transferred.
 * See the "Capture journal" section of Documentation/technical/
 * git-provenance-format.adoc for the format.
 */

enum provenance_journal_type {
	PROVENANCE_JOURNAL_BEGIN,
	PROVENANCE_JOURNAL_END,
	PROVENANCE_JOURNAL_EDIT,
	PROVENANCE_JOURNAL_REASON,
	PROVENANCE_JOURNAL_CLAIM_PATH,
	PROVENANCE_JOURNAL_CLAIM_PATCH,
};

struct provenance_journal_entry {
	enum provenance_journal_type type;
	/* Unix time of the stamp */
	timestamp_t time;
	char *session;
	/*
	 * The commit HEAD pointed at when the stamp was made, or the null
	 * oid if HEAD was unborn or the entry does not say (it was written
	 * by an earlier version).
	 */
	struct object_id head;
	/*
	 * BEGIN, END: the snapshot tree.
	 * EDIT: the pre-image blob.
	 * REASON: the summary blob.
	 * CLAIM_PATCH: the patch blob.
	 */
	struct object_id oid;
	/*
	 * EDIT: the post-image blob.
	 * REASON: the transcript blob, or the null oid if there is none.
	 */
	struct object_id oid2;
	/* BEGIN: optional, NULL if not given */
	char *model;
	char *tool;
	/* EDIT, CLAIM_PATH */
	char *path;

	/*
	 * Set by the attribution of a commit (see the "Attribution"
	 * section of Documentation/technical/git-provenance.adoc) for
	 * provenance_journal_consume():
	 *
	 * foreign: the stamp was taken on another commit than the one
	 * HEAD points at;
	 * crossing: the entry is the "begin" or the "end" of an interval
	 * during which HEAD moved, which is ignored;
	 * used: the entry is foreign and the commit used it.
	 */
	unsigned foreign : 1;
	unsigned crossing : 1;
	unsigned used : 1;
};

struct provenance_journal {
	struct provenance_journal_entry *entries;
	size_t nr, alloc;
	/* the number of lines read, malformed ones included */
	size_t lines;
};

#define PROVENANCE_JOURNAL_INIT { 0 }

/*
 * Return 1 if `session` is a valid session id: a non-empty string of
 * the characters [A-Za-z0-9._:-].
 */
int provenance_session_is_valid(const char *session);

/*
 * Store in `head` the commit that HEAD of the current worktree of `r`
 * points at, or the null oid if HEAD is unborn.
 */
void provenance_journal_head(struct repository *r, struct object_id *head);

/*
 * Append the serialized form of `e`, without the trailing LF, to
 * `out`.
 */
void provenance_journal_entry_format(const struct provenance_journal_entry *e,
				     struct strbuf *out);

/*
 * Append `e` to the journal of the current worktree of `r`, holding
 * the journal's lock while doing so.  Returns 0 on success, or -1
 * after reporting an error, for instance if the session id or a
 * field is invalid or a path contains a LF.
 */
int provenance_journal_append(struct repository *r,
			      const struct provenance_journal_entry *e);

/*
 * Remove from the journal of the current worktree of `r` the entries
 * of `j` that the commit `commit` consumed, holding the journal's lock
 * while doing so.  `j` is the journal as it was read for the
 * attribution of the commit, which marked its entries; entries
 * appended to the journal since it was read are kept.
 *
 * The commit consumes the entries taken on the commit HEAD pointed at
 * while it was made, and those without a head, unless it is
 * `partial`: a partial commit keeps them, as taken on `commit`.
 * Every commit drops the entries of intervals during which HEAD
 * moved, and the foreign entries taken on a commit that no branch
 * reaches; a commit that is not `partial` also consumes the foreign
 * entries it used, and removes the picks (see
 * provenance_picks_append()).  The journal is removed if no entry is
 * left.  Returns 0 on success, or -1 after reporting an error.
 */
int provenance_journal_consume(struct repository *r,
			       const struct provenance_journal *j,
			       const struct object_id *commit, int partial);

/*
 * The journal of a stash entry.  A "git stash push" that removes all
 * the changes from the working tree takes the journal with it: the
 * journal is moved to "provenance/stash/<stash>" in the common git
 * directory, as the stash is shared by all worktrees, where <stash> is
 * the full object id of the stash commit, and its entries come back
 * when the stash is applied.
 *
 * provenance_journal_stash() moves the journal of the current worktree
 * of `r` to the stash commit `stash`, holding the journal's lock while
 * doing so.  A missing journal is not an error.
 *
 * provenance_journal_unstash() appends the entries moved to `stash` to
 * the journal of the current worktree of `r`, holding the journal's
 * lock while doing so, and leaves them with `stash` too.
 *
 * provenance_journal_stash_prune() removes the entries moved to the
 * commits that the reflog of "refs/stash" does not name any more, as
 * after "git stash pop", "drop" or "clear".
 *
 * All return 0 on success, or -1 after reporting an error.
 */
int provenance_journal_stash(struct repository *r,
			     const struct object_id *stash);
int provenance_journal_unstash(struct repository *r,
			       const struct object_id *stash);
int provenance_journal_stash_prune(struct repository *r);

/*
 * The picks of a worktree are the commits that "cherry-pick
 * --no-commit" applied to it, or that "revert --no-commit" undid,
 * since the last commit, whose records the next commit carries over.
 * They are listed in "$GIT_DIR/provenance/picks", next to the journal,
 * one per line, as "pick <oid>" or "revert <oid>" with a full object
 * id, and removed with the journal.  A line with an object id alone
 * reads as a pick.
 */
struct provenance_pick {
	struct object_id oid;
	/* the commit was reverted, not picked */
	unsigned revert : 1;
};

struct provenance_picks {
	struct provenance_pick *items;
	size_t nr, alloc;
};

#define PROVENANCE_PICKS_INIT { 0 }

/*
 * provenance_picks_append() appends `oid` to the picks of the current
 * worktree of `r`, as a revert if `revert` is set, holding the
 * journal's lock while doing so.  Returns 0 on success, or -1 after
 * reporting an error.
 *
 * provenance_picks_read() appends the picks of the current worktree of
 * `r` to `picks`, in the order in which they were made.  A missing
 * picks file is empty.  Malformed lines are skipped with a warning.
 * Returns 0 on success, or -1 after reporting an error if the file
 * cannot be read.
 */
int provenance_picks_append(struct repository *r,
			    const struct object_id *oid, int revert);
int provenance_picks_read(struct repository *r,
			  struct provenance_picks *picks);
void provenance_picks_release(struct provenance_picks *picks);

/*
 * Read the journal of the current worktree of `r` into `j`, which
 * must have been initialized with PROVENANCE_JOURNAL_INIT.  A missing
 * journal is empty.  Malformed lines are skipped with a warning.
 * Object ids are not checked for existence: objects named by the
 * journal may have been pruned since they were recorded.  Returns 0
 * on success, or -1 after reporting an error if the journal cannot
 * be read.
 */
int provenance_journal_read(struct repository *r,
			    struct provenance_journal *j);

void provenance_journal_release(struct provenance_journal *j);

/*
 * Write the working tree of `r` as a tree object and store its id in
 * `tree`, without touching the index file.  The snapshot starts from
 * the index.  Paths matching `pathspec` (all paths if it is empty) are
 * updated from the working tree as "git add -A" would update them:
 * modified tracked files are re-hashed, deleted ones removed, and
 * untracked files that are not ignored added.  Paths outside
 * `pathspec` keep their index entries, as do submodules and entries
 * outside a sparse checkout; nested repositories that are not
 * submodules are left out.  New objects are written to the object
 * database.  Returns 0 on success, or -1 after reporting an error.
 */
int provenance_journal_snapshot(struct repository *r,
				const struct pathspec *pathspec,
				struct object_id *tree);

#endif /* PROVENANCE_JOURNAL_H */
