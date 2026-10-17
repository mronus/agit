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
};

struct provenance_journal {
	struct provenance_journal_entry *entries;
	size_t nr, alloc;
};

#define PROVENANCE_JOURNAL_INIT { 0 }

/*
 * Return 1 if `session` is a valid session id: a non-empty string of
 * the characters [A-Za-z0-9._:-].
 */
int provenance_session_is_valid(const char *session);

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
 * Remove the journal of the current worktree of `r`, holding the
 * journal's lock while doing so.  A missing journal is not an error.
 * Returns 0 on success, or -1 with errno set, without reporting the
 * error.
 */
int provenance_journal_clear(struct repository *r);

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
