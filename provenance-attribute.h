#ifndef PROVENANCE_ATTRIBUTE_H
#define PROVENANCE_ATTRIBUTE_H

struct commit;
struct commit_list;
struct object_id;
struct provenance_journal;
struct provenance_reasoning;
struct provenance_record;
struct repository;

/*
 * Attribute the lines changed by a commit whose tree is `tree` and
 * whose parents are `parents` (NULL for a root commit), using the
 * stamps in `journal`.  See the "Attribution" section of
 * Documentation/technical/git-provenance.adoc.
 *
 * The lines are those that the commit changes from its first parent.
 * Those that came from another parent of a merge are left out: the
 * lines that the version of another parent has as they are, and the
 * lines of the first parent's version that the commit deletes and that
 * another parent lacks.  The other parents are not actors: a line that
 * replaces one of theirs belongs to whoever wrote it, alone.  A file
 * left with no lines gets no block, and a merge that takes all its
 * changes from its other parents gets a record without files.
 *
 * `session` is the session named with "git commit --session", or NULL.
 * `human` identifies the human who made the commit, as
 * "Name <email>".
 *
 * On success the line record is stored in `lines` and the reasoning
 * record in `reasoning`, which must have been initialized with
 * PROVENANCE_RECORD_INIT and PROVENANCE_REASONING_INIT; their commit
 * ids are left for the caller to fill in.  The reasoning record names
 * no session if no session is attached to the commit.  Stamps whose
 * objects are missing are skipped with a warning.  Returns 0 on
 * success, or -1 after reporting an error.
 *
 * The stamps are used by the commit that HEAD pointed at when they
 * were made: those taken on the commit HEAD points at now make the
 * chain of versions of each file, and the others are located by
 * content.  The entries of `journal` are marked as foreign, crossing
 * and used accordingly (see struct provenance_journal_entry).
 */
int provenance_attribute(struct repository *r,
			 const struct commit_list *parents,
			 const struct object_id *tree,
			 struct provenance_journal *journal,
			 const char *session,
			 const char *human,
			 struct provenance_record *lines,
			 struct provenance_reasoning *reasoning);

/* A commit whose records seed the attribution of a rewritten commit. */
struct provenance_source {
	struct commit *commit;
	/* NULL if the commit has no line record */
	const struct provenance_record *lines;
	/* NULL if the commit has no reasoning record */
	const struct provenance_reasoning *reasoning;
	/*
	 * The commit may not be one that the new commit was made from:
	 * its reasoning record is only carried over if a line of the new
	 * commit takes its owners from it.
	 */
	unsigned tentative : 1;
	/*
	 * The new commit undoes the changes of the commit.  The lines
	 * that the commit deleted, which the new commit restores, are
	 * owned as a blame of the commit's first parent finds them: by
	 * the actors of the range that covers the line in the line record
	 * of the commit that introduced it; such a line is UNKNOWN if
	 * that commit has no line record or the range is UNKNOWN.  The
	 * lines that the commit added, which the new commit deletes, are
	 * attributed as its line record says.  Its reasoning record is
	 * not carried over.
	 */
	unsigned revert : 1;
};

/*
 * Flags for provenance_attribute_rewrite().
 *
 * PROVENANCE_REWRITE_AMEND: `sources[0]` is the commit being amended.
 *
 * PROVENANCE_REWRITE_ADOPT: the commit was made without the extension
 * (see the "Re-anchoring" section of
 * Documentation/technical/git-provenance.adoc).  The lines that no
 * source accounts for are UNKNOWN instead of the human's, and
 * `capture` is `adopted`.  The sources are usually tentative.  The
 * journal must be empty.
 */
#define PROVENANCE_REWRITE_AMEND (1 << 0)
#define PROVENANCE_REWRITE_ADOPT (1 << 1)

/*
 * As provenance_attribute(), for a commit that rewrites the `nr`
 * commits in `sources`, carrying their records over to it.  See the
 * "History rewriting" section of
 * Documentation/technical/git-provenance.adoc.
 *
 * The changes of each source are attributed as its line record says,
 * or undone if it is a revert, before the stamps in `journal`.  The
 * lines that a source without a line record changes, like those its
 * record marks UNKNOWN, are of unknown origin, and stay UNKNOWN
 * however they are edited after.
 *
 * With PROVENANCE_REWRITE_AMEND and other sources, the commit being
 * amended is a version the commit was made from: the chain of each
 * file goes through its post-image first.  The changes of every other
 * source, and of the commit being amended if it is the only source,
 * are found in the commit, wherever they were applied.  A line found
 * in several sources gets the owners that the first of them in
 * `sources` gives it, preferring a source that has the line as it is
 * to one that has a line it replaces.  The other parents of a merge
 * come after all the sources.
 *
 * The reasoning record names the sessions of the reasoning records of
 * the sources that are not reverts, followed by the sessions the
 * journal attaches to the commit.  The reasoning record of
 * a tentative source is left out if no line of the commit takes its
 * owners from that source.
 */
int provenance_attribute_rewrite(struct repository *r,
				 const struct commit_list *parents,
				 const struct object_id *tree,
				 const struct provenance_source *sources,
				 size_t nr, unsigned flags,
				 struct provenance_journal *journal,
				 const char *session,
				 const char *human,
				 struct provenance_record *lines,
				 struct provenance_reasoning *reasoning);

/*
 * The attribution walk for one path at a time, as used to show the
 * provenance of changes that are not committed yet.
 *
 * provenance_attribution_new() prepares to attribute changes against
 * `base`, the tree of the commit that would be the first parent (or
 * the empty tree), and `others`, the other parents of the commit if it
 * would be a merge, using the stamps in `journal`, which must outlive
 * the result.  Returns NULL after reporting an error.  Release the
 * result with provenance_attribution_free().
 *
 * provenance_attribute_path() attributes the lines of `path` whose
 * content would be the `len` bytes at `buf` with mode `mode` (`buf` is
 * NULL if the path would be deleted), relative to its version in
 * `base`, exactly as provenance_attribute() would attribute them, and
 * appends the block for `path`, if it gets one, to `lines`.  The
 * content is not written to the object database.  Returns 0 on
 * success, or -1 after reporting an error.
 */
struct provenance_attribution;

struct provenance_attribution *
provenance_attribution_new(struct repository *r, const struct object_id *base,
			   const struct commit_list *others,
			   struct provenance_journal *journal);
int provenance_attribute_path(struct provenance_attribution *pa,
			      const char *path, const char *buf, size_t len,
			      unsigned mode, struct provenance_record *lines);
void provenance_attribution_free(struct provenance_attribution *pa);

#endif /* PROVENANCE_ATTRIBUTE_H */
