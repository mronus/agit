#ifndef PROVENANCE_ATTRIBUTE_H
#define PROVENANCE_ATTRIBUTE_H

struct commit;
struct object_id;
struct provenance_journal;
struct provenance_reasoning;
struct provenance_record;
struct repository;

/*
 * Attribute the lines changed by a commit whose tree is `tree` and
 * whose first parent is `parent` (NULL for a root commit), using the
 * stamps in `journal`.  See the "Attribution" section of
 * Documentation/technical/git-provenance.adoc.
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
 */
int provenance_attribute(struct repository *r,
			 struct commit *parent,
			 const struct object_id *tree,
			 const struct provenance_journal *journal,
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
};

/*
 * As provenance_attribute(), for a commit that rewrites the `nr`
 * commits in `sources`, carrying their records over to it.  See the
 * "History rewriting" section of
 * Documentation/technical/git-provenance.adoc.
 *
 * The changes of each source are attributed as its line record says,
 * before the stamps in `journal`.  The lines that a source without a
 * line record changes, like those its record marks UNKNOWN, are of
 * unknown origin, and stay UNKNOWN however they are edited after.
 * All sources but the last are
 * versions the commit was made from, as the commit being amended is;
 * the changes of the last one are found in the commit, wherever they
 * were applied.  If `revert` is set, there must be one source, and
 * the commit undoes its changes.
 *
 * The reasoning record names the sessions of the reasoning records of
 * the sources, unless `revert` is set, followed by the sessions the
 * journal attaches to the commit.
 */
int provenance_attribute_rewrite(struct repository *r,
				 struct commit *parent,
				 const struct object_id *tree,
				 const struct provenance_source *sources,
				 size_t nr, int revert,
				 const struct provenance_journal *journal,
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
 * the empty tree), using the stamps in `journal`, which must outlive
 * the result.  Release the result with provenance_attribution_free().
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
			   const struct provenance_journal *journal);
int provenance_attribute_path(struct provenance_attribution *pa,
			      const char *path, const char *buf, size_t len,
			      unsigned mode, struct provenance_record *lines);
void provenance_attribution_free(struct provenance_attribution *pa);

#endif /* PROVENANCE_ATTRIBUTE_H */
