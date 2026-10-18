#ifndef PROVENANCE_VIEW_H
#define PROVENANCE_VIEW_H

#include "oidmap.h"

struct commit;
struct diff_filespec;
struct diff_options;
struct object_id;
struct provenance_record;
struct repository;

/*
 * Showing provenance records to the user.  See the "Viewing" section
 * of Documentation/technical/git-provenance.adoc.
 */

/* Parsed line records, read on demand and kept by commit. */
struct provenance_cache {
	struct repository *r;
	struct oidmap records;
	/* whether refs/provenance/lines exists: 1, 0, or -1 if not checked */
	int have_ref;
};

void provenance_cache_init(struct provenance_cache *cache,
			   struct repository *r);

/*
 * Return the line record of `commit`, or NULL if it has none.  A record
 * that cannot be read or parsed, or that names another commit, counts
 * as none, with a warning the first time it is looked up.
 */
const struct provenance_record *
provenance_cache_get(struct provenance_cache *cache,
		     const struct object_id *commit);

/* Forget all records, and whether the ref exists. */
void provenance_cache_release(struct provenance_cache *cache);

/*
 * Where "diff --provenance" takes the marks of its lines from.  The
 * live sources attribute the changes since HEAD with the capture
 * journal, as "git commit" would.
 */
enum provenance_diff_source {
	/* nothing: every line is shown as '?' */
	PROVENANCE_DIFF_NONE,
	/* the line record of a commit, diffed against its first parent */
	PROVENANCE_DIFF_COMMIT,
	/* HEAD against the index */
	PROVENANCE_DIFF_INDEX,
	/* HEAD against the working tree */
	PROVENANCE_DIFF_WORKTREE,
	/* the index against the working tree */
	PROVENANCE_DIFF_FILES,
};

/*
 * Tell the diff `opt` where the marks of the lines it is about to show
 * come from.  These do nothing unless "--provenance" was given.
 *
 * provenance_diff_set_commit() uses the line record of `commit` if the
 * diff is about to show it against its first parent, that is, if
 * `old_tree` is the tree of its first parent, or the empty tree if it
 * has none; otherwise no source is used.
 *
 * provenance_diff_set_live() uses a live source.  For the sources
 * PROVENANCE_DIFF_INDEX and PROVENANCE_DIFF_WORKTREE, `old_tree` is
 * the tree the diff compares with, and no source is used unless it is
 * the tree of HEAD.  No live source is used either if commits do not
 * record provenance in the repository.
 *
 * provenance_diff_unset() uses no source.
 */
void provenance_diff_set_commit(struct diff_options *opt,
				struct commit *commit,
				const struct object_id *old_tree);
void provenance_diff_set_live(struct diff_options *opt,
			      enum provenance_diff_source source,
			      const struct object_id *old_tree);
void provenance_diff_unset(struct diff_options *opt);

/* Release what the functions above set up; called by diff_free(). */
void provenance_diff_release(struct diff_options *opt);

/* The marks of the lines of the two sides of a filepair. */
struct provenance_marks {
	char *pre, *post;
	size_t pre_nr, post_nr;
};

#define PROVENANCE_MARKS_INIT { 0 }

/*
 * Compute the marks of the lines of the filepair `one` and `two` with
 * status `status`, as shown by the diff `opt`, whose content are `pre`
 * and `pre_len`, and `post` and `post_len`.
 */
void provenance_diff_marks(struct diff_options *opt,
			   const struct diff_filespec *one,
			   const struct diff_filespec *two, char status,
			   const char *pre, size_t pre_len,
			   const char *post, size_t post_len,
			   struct provenance_marks *marks);

/*
 * Return the mark of 1-based line `line` of the post-image (if `post`
 * is set) or the pre-image: 'A', 'M', 'H' or '?'.
 */
char provenance_marks_get(const struct provenance_marks *marks, int post,
			  long line);

void provenance_marks_release(struct provenance_marks *marks);

#endif /* PROVENANCE_VIEW_H */
